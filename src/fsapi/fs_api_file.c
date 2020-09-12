#include <sys/stat.h>
#include <limits.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/sched_thread.h"
#include "fs_api_util.h"
#include "fs_api_file.h"

#define FS_API_MAGIC_NUMBER    1588076578

static int file_truncate(FSAPIContext *ctx, const int64_t oid,
        const int64_t new_size);

static int deal_open_flags(FSAPIFileInfo *fi, FDIRDEntryFullName *fullname,
        const mode_t mode, int result)
{
    if (result == 0) {
        if (S_ISDIR(fi->dentry.stat.mode)) {
            return EISDIR;
        }
    }

    if (!((fi->flags & O_WRONLY) || (fi->flags & O_RDWR))) {
        fi->offset = 0;
        return result;
    }

    if ((fi->flags & O_CREAT)) {
        if (result == 0) {
            if ((fi->flags & O_EXCL)) {
                return EEXIST;
            }
        } else if (result == ENOENT) {
            if ((result=fdir_client_create_dentry(fi->ctx->contexts.fdir,
                    fullname, mode, &fi->dentry)) != 0)
            {
                if (result == EEXIST) {
                    if ((fi->flags & O_EXCL)) {
                        return EEXIST;
                    }

                    if ((result=fdir_client_stat_dentry_by_path(
                                    fi->ctx->contexts.fdir, fullname,
                                    &fi->dentry)) != 0)
                    {
                        return result;
                    }
                    if (S_ISDIR(fi->dentry.stat.mode)) {
                        return EISDIR;
                    }
                } else {
                    return result;
                }
            }
        } else {
            return result;
        }
    } else if (result != 0) {
        return result;
    }

    fi->write_notify.last_modified_time = fi->dentry.stat.mtime;
    if ((fi->flags & O_TRUNC)) {
        logInfo("file: "__FILE__", line: %d, func: %s, do truncate!",
                __LINE__, __FUNCTION__);
        if (fi->dentry.stat.size > 0) {
            if ((result=file_truncate(fi->ctx, fi->dentry.inode, 0)) != 0) {
                return result;
            }

            fi->dentry.stat.size = 0;
        }
    }

    if ((fi->flags & O_APPEND)) {
        fi->offset = fi->dentry.stat.size;
    } else {
        fi->offset = 0;
    }
    return 0;
}

int fsapi_open_ex(FSAPIContext *ctx, FSAPIFileInfo *fi,
            const char *path, const int flags, const mode_t mode)
{
    FDIRDEntryFullName fullname;
    int result;
    mode_t new_mode;

    if ((mode & S_IFMT)) {
        new_mode = (mode & (~S_IFMT))  | S_IFREG;
    } else {
        new_mode = mode | S_IFREG;
    }

    fi->ctx = ctx;
    fi->flags = flags;
    fi->sessions.flock.mconn = NULL;
    fullname.ns = ctx->ns;
    FC_SET_STRING(fullname.path, (char *)path);
    result = fdir_client_stat_dentry_by_path(ctx->contexts.fdir,
            &fullname, &fi->dentry);
    if ((result=deal_open_flags(fi, &fullname, new_mode, result)) != 0) {
        return result;
    }

    fi->magic = FS_API_MAGIC_NUMBER;
    return 0;
}

int fsapi_open_by_dentry_ex(FSAPIContext *ctx, FSAPIFileInfo *fi,
            const FDIRDEntryInfo *dentry, const int flags)
{
    int result;

    fi->dentry = *dentry;
    fi->ctx = ctx;
    fi->flags = flags;
    fi->sessions.flock.mconn = NULL;
    result = 0;
    if ((result=deal_open_flags(fi, NULL, 0755, result)) != 0) {
        return result;
    }

    fi->magic = FS_API_MAGIC_NUMBER;
    return 0;
}

int fsapi_open_by_inode_ex(FSAPIContext *ctx, FSAPIFileInfo *fi,
            const int64_t inode, const int flags)
{
    int result;

    if ((result=fdir_client_stat_dentry_by_inode(ctx->contexts.fdir,
                    inode, &fi->dentry)) != 0)
    {
        return result;
    }

    fi->ctx = ctx;
    fi->flags = flags;
    fi->sessions.flock.mconn = NULL;
    if ((result=deal_open_flags(fi, NULL, 0755, result)) != 0) {
        return result;
    }

    fi->magic = FS_API_MAGIC_NUMBER;
    return 0;
}

int fsapi_close(FSAPIFileInfo *fi)
{
    if (fi->magic != FS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if (fi->sessions.flock.mconn != NULL) {
        /* force close connection to unlock */
        fdir_client_close_session(&fi->sessions.flock, true);
    }

    fi->ctx = NULL;
    fi->magic = 0;
    return 0;
}

static inline void print_block_slice_key(FSBlockSliceKeyInfo *bs_key)
{
    logInfo("block {oid: %"PRId64", offset: %"PRId64"}, "
            "slice {offset: %d, length: %d}", bs_key->block.oid,
            bs_key->block.offset, bs_key->slice.offset, bs_key->slice.length);
}

static int do_pwrite(FSAPIFileInfo *fi, const char *buff,
        const int size, const int64_t offset, int *written_bytes,
        int *total_inc_alloc, const bool need_report_modified)
{
    FSBlockSliceKeyInfo bs_key;
    int64_t new_offset;
    int result;
    int current_written;
    int inc_alloc;
    int remain;

    *total_inc_alloc = *written_bytes = 0;
    new_offset = offset;
    fs_set_block_slice(&bs_key, fi->dentry.inode, offset, size);
    while (1) {
        print_block_slice_key(&bs_key);
        if ((result=fs_client_slice_write(fi->ctx->contexts.fs,
                        &bs_key, buff + *written_bytes,
                        &current_written, &inc_alloc)) != 0)
        {
            if (current_written == 0) {
                break;
            }
        }

        new_offset += current_written;
        *written_bytes += current_written;
        *total_inc_alloc += inc_alloc;
        remain = size - *written_bytes;
        if (remain <= 0) {
            break;
        }

        if (current_written == bs_key.slice.length) {  //fully completed
            fs_next_block_slice_key(&bs_key, remain);
        } else {  //partially completed, try again the remain part
            fs_set_slice_size(&bs_key, new_offset, remain);
        }
    }

    logInfo("file: "__FILE__", line: %d, "
            "offset: %"PRId64", *written_bytes: %d, need_report_modified: %d",
            __LINE__, offset, *written_bytes, need_report_modified);

    if (*written_bytes > 0) {
        int64_t new_size;
        int flags;

        if (!need_report_modified) {
            return 0;
        }

        new_size = offset + *written_bytes;
        if (new_size > fi->dentry.stat.size) {
            flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE |
                FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        } else {
            int current_time;

            if (new_size > fi->dentry.stat.space_end) {
                flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
            } else {
                flags = 0;
            }

            current_time = get_current_time();
            if (current_time > fi->write_notify.last_modified_time) {
                fi->write_notify.last_modified_time = current_time;
                flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_MTIME;
            }
        }

        if (*total_inc_alloc != 0)  {
            flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_INC_ALLOC;
        }

        if (flags != 0) {
            result = fdir_client_set_dentry_size(fi->ctx->contexts.fdir,
                    &fi->ctx->ns, fi->dentry.inode, new_size,
                    *total_inc_alloc, false, &fi->dentry, flags);

            logInfo("file: "__FILE__", line: %d, func: %s, set_dentry_size result: %d",
                    __LINE__, __FUNCTION__, result);
        }
        return 0;
    } else {
        return EIO;
    }
}

int fsapi_pwrite(FSAPIFileInfo *fi, const char *buff,
        const int size, const int64_t offset, int *written_bytes)
{
    int total_inc_alloc;

    if (size == 0) {
        return 0;
    } else if (size < 0) {
        return EINVAL;
    }

    if (fi->magic != FS_API_MAGIC_NUMBER || !((fi->flags & O_WRONLY) ||
                (fi->flags & O_RDWR)))
    {
        return EBADF;
    }

    return do_pwrite(fi, buff, size, offset, written_bytes,
            &total_inc_alloc, true);
}

int fsapi_write(FSAPIFileInfo *fi, const char *buff,
        const int size, int *written_bytes)
{
    FDIRClientSession session;
    bool need_report_modified;
    int result;
    int total_inc_alloc;
    int flags;
    int64_t old_size;
    int64_t space_end;

    if (size == 0) {
        return 0;
    } else if (size < 0) {
        return EINVAL;
    }

    if (fi->magic != FS_API_MAGIC_NUMBER || !((fi->flags & O_WRONLY) ||
                (fi->flags & O_RDWR)))
    {
        return EBADF;
    }

    if ((fi->flags & O_APPEND)) {
        if ((result=fsapi_dentry_sys_lock(&session, fi->dentry.inode,
                        0, &old_size, &space_end)) != 0)
        {
            return result;
        }

        fi->offset = old_size;
        need_report_modified = false;
    } else {
        old_size = 0;
        need_report_modified = true;
    }

    if ((result=do_pwrite(fi, buff, size, fi->offset, written_bytes,
                    &total_inc_alloc, need_report_modified)) == 0)
    {
        fi->offset += *written_bytes;
    }

    if ((fi->flags & O_APPEND)) {
        string_t *ns;
        if (fi->offset > old_size) {
            ns = &fi->ctx->ns; //set ns for update file_size, space_end etc.
        } else {
            ns = NULL;  //do NOT update
        }
        logInfo("=========pid: %d, old file size: %"PRId64", new file size: %"PRId64", "
                "written_bytes: %d", getpid(), old_size, fi->offset, *written_bytes);

        flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE |
            FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        fsapi_dentry_sys_unlock(&session, ns, fi->dentry.inode,
                false, old_size, fi->offset, total_inc_alloc, flags);
    }

    return result;
}

int fsapi_pread(FSAPIFileInfo *fi, char *buff, const int size,
        const int64_t offset, int *read_bytes)
{
    FSBlockSliceKeyInfo bs_key;
    int result;
    int current_read;
    int remain;
    int64_t current_offset;
    int64_t hole_bytes;
    int fill_bytes;

    *read_bytes = 0;
    if (size == 0) {
        return 0;
    } else if (size < 0) {
        return EINVAL;
    }

    if (fi->magic != FS_API_MAGIC_NUMBER || (fi->flags & O_WRONLY)) {
        return EBADF;
    }

    fs_set_block_slice(&bs_key, fi->dentry.inode, offset, size);
    while (1) {
        print_block_slice_key(&bs_key);
        if ((result=fs_client_slice_read(fi->ctx->contexts.fs,
                        &bs_key, buff + *read_bytes, &current_read)) != 0)
        {
            if (result == ENODATA) {
                result = 0;
            } else {
                break;
            }
        }

        /*
           logInfo("=====slice.length: %d, current_read: %d==",
           bs_key.slice.length, current_read);
           */
        while (current_read < bs_key.slice.length) {
            /* deal file hole caused by ftruncate and lseek */
            current_offset = offset + *read_bytes + current_read;
            if (current_offset == fi->dentry.stat.size) {
                break;
            }

            if (current_offset > fi->dentry.stat.size) {
                if ((result=fdir_client_stat_dentry_by_inode(fi->
                                ctx->contexts.fdir, fi->dentry.inode,
                                &fi->dentry)) != 0)
                {
                    return result;
                }
            }

            hole_bytes = fi->dentry.stat.size - current_offset;
            if (hole_bytes > 0) {
                if (current_read + hole_bytes > (int64_t)bs_key.slice.length) {
                    fill_bytes = bs_key.slice.length - current_read;
                } else {
                    fill_bytes = hole_bytes;
                }

                logInfo("=====offset: %"PRId64", current_read: %d, "
                        "hole_bytes: %"PRId64", fill_bytes: %d, "
                        "buff offset: %d =====", offset, current_read,
                        hole_bytes, fill_bytes, *read_bytes + current_read);

                memset(buff + *read_bytes + current_read, 0, fill_bytes);
                current_read += fill_bytes;
            }

            break;
        }

        *read_bytes += current_read;
        remain = size - *read_bytes;
        if (remain <= 0 || current_read < bs_key.slice.length) {
            break;
        }

        fs_next_block_slice_key(&bs_key, remain);
    }

    return result;
}

int fsapi_read(FSAPIFileInfo *fi, char *buff, const int size, int *read_bytes)
{
    int result;

    if ((result=fsapi_pread(fi, buff, size, fi->offset, read_bytes)) != 0) {
        return result;
    }

    fi->offset += *read_bytes;
    return 0;
}

static int do_truncate(FSAPIContext *ctx, const int64_t oid,
        const int64_t old_space_end, const int64_t offset,
        const int64_t length, int64_t *total_dec_alloc)
{
    FSBlockSliceKeyInfo bs_key;
    int64_t remain;
    int dec_alloc;
    int result;

    *total_dec_alloc = 0;
    if (offset >= old_space_end) {
        return 0;
    }

    if (offset + length > old_space_end) {
        remain = old_space_end - offset;
    } else {
        remain = length;
    }
    fs_set_block_slice(&bs_key, oid, offset, remain);
    while (1) {
        print_block_slice_key(&bs_key);
        if (bs_key.slice.length == FS_FILE_BLOCK_SIZE) {
            result = fs_client_block_delete(ctx->contexts.fs,
                    &bs_key.block, &dec_alloc);
        } else {
            result = fs_client_slice_delete(ctx->contexts.fs,
                    &bs_key, &dec_alloc);
        }
        if (result == 0) {
            *total_dec_alloc -= dec_alloc;
        } else if (result == ENOENT) {
            result = 0;
        } else if (result != 0) {
            break;
        }

        remain -= bs_key.slice.length;
        if (remain <= 0) {
            break;
        }

        fs_next_block_slice_key(&bs_key, remain);
    }

    return result;
}

static int do_allocate(FSAPIContext *ctx, const int64_t oid,
        const int64_t offset, const int64_t length,
        int64_t *total_inc_alloc)
{
    FSBlockSliceKeyInfo bs_key;
    int64_t remain;
    int inc_alloc;
    int result;

    *total_inc_alloc = 0;
    if (length <= 0) {
        return 0;
    }

    remain = length;
    fs_set_block_slice(&bs_key, oid, offset, remain);
    while (1) {
        print_block_slice_key(&bs_key);
        if ((result=fs_client_slice_allocate(ctx->contexts.fs,
                        &bs_key, &inc_alloc)) != 0)
        {
            break;
        }
        *total_inc_alloc += inc_alloc;

        remain -= bs_key.slice.length;
        if (remain <= 0) {
            break;
        }

        fs_next_block_slice_key(&bs_key, remain);
    }

    return result;
}

static int file_truncate(FSAPIContext *ctx, const int64_t oid,
        const int64_t new_size)
{
    FDIRClientSession session;
    int64_t old_size;
    string_t *ns;
    int64_t alloc_bytes;
    int64_t space_end;
    int result;
    int flags;
    int unlock_res;

    if (new_size < 0) {
        return EINVAL;
    }

    if ((result=fsapi_dentry_sys_lock(&session, oid, 0,
                    &old_size, &space_end)) != 0)
    {
        return result;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, SYS_LOCK",
            __LINE__, __FUNCTION__);

    if (new_size >= old_size) {
        result = 0;
        alloc_bytes = 0;
        if (new_size == old_size) {
            flags = 0;
        } else {
            flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
        }
    } else {
        result = do_truncate(ctx, oid, space_end, new_size,
                old_size - new_size, &alloc_bytes);

        flags = FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
        if (new_size < space_end) {
            flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        }
    }

    if (result == 0) {
        ns = &ctx->ns;  //set ns for update file_size, space_end etc.
    } else {
        ns = NULL;  //do NOT update
    }
    unlock_res = fsapi_dentry_sys_unlock(&session, ns, oid,
            true, old_size, new_size, alloc_bytes, flags);

    logInfo("file: "__FILE__", line: %d, func: %s, SYS_UNLOCK: %d",
            __LINE__, __FUNCTION__, unlock_res);

    return result == 0 ? unlock_res : result;
}

int fsapi_ftruncate(FSAPIFileInfo *fi, const int64_t new_size)
{
    if (fi->magic != FS_API_MAGIC_NUMBER || !((fi->flags & O_WRONLY) ||
                (fi->flags & O_RDWR)))
    {
        return EBADF;
    }

    return file_truncate(fi->ctx, fi->dentry.inode, new_size);
}

static int get_regular_file_inode(FSAPIContext *ctx, const char *path,
        int64_t *inode)
{
    FDIRDEntryFullName fullname;
    FDIRDEntryInfo dentry;
    int result;

    fullname.ns = ctx->ns;
    FC_SET_STRING(fullname.path, (char *)path);
    if ((result=fdir_client_stat_dentry_by_path(ctx->contexts.fdir,
                    &fullname, &dentry)) != 0)
    {
        return result;
    }
    if (S_ISDIR(dentry.stat.mode)) {
        return EISDIR;
    }

    *inode = dentry.inode;
    return 0;
}

int fsapi_truncate_ex(FSAPIContext *ctx, const char *path,
        const int64_t new_size)
{
    int result;
    int64_t inode;

    if ((result=get_regular_file_inode(ctx, path, &inode)) != 0) {
        return result;
    }
    return file_truncate(ctx, inode, new_size);
}

int fsapi_unlink_ex(FSAPIContext *ctx, const char *path)
{
    FDIRDEntryFullName fullname;
    FDIRDEntryInfo dentry;
    int result;

    fullname.ns = ctx->ns;
    FC_SET_STRING(fullname.path, (char *)path);
    if ((result=fdir_client_stat_dentry_by_path(ctx->contexts.fdir,
                    &fullname, &dentry)) != 0)
    {
        return result;
    }

    if (S_ISDIR(dentry.stat.mode)) {
        return EISDIR;
    }

    if ((result=fs_unlink_file(ctx->contexts.fs, dentry.inode,
                    dentry.stat.size)) == 0)
    {
        result = fdir_client_remove_dentry(ctx->contexts.fdir, &fullname);
    }

    return result;
}

#define calc_file_offset(fi, offset, whence, new_offset)  \
    calc_file_offset_ex(fi, offset, whence, false, new_offset)

static inline int calc_file_offset_ex(FSAPIFileInfo *fi,
        const int64_t offset, const int whence,
        const bool refresh_fsize, int64_t *new_offset)
{
    int result;

    switch (whence) {
        case SEEK_SET:
            if (offset < 0) {
                return EINVAL;
            }
            *new_offset = offset;
            break;
        case SEEK_CUR:
            *new_offset = fi->offset + offset;
            if (*new_offset < 0) {
                return EINVAL;
            }
            break;
        case SEEK_END:
            if (refresh_fsize) {
                if ((result=fdir_client_stat_dentry_by_inode(
                                fi->ctx->contexts.fdir,
                                fi->dentry.inode, &fi->dentry)) != 0)
                {
                    return result;
                }
            }

            *new_offset = fi->dentry.stat.size + offset;
            if (*new_offset < 0) {
                return EINVAL;
            }
            break;
        default:
            logError("file: "__FILE__", line: %d, "
                    "invalid whence: %d", __LINE__, whence);
            return EINVAL;
    }

    return 0;
}

int fsapi_lseek(FSAPIFileInfo *fi, const int64_t offset, const int whence)
{
    int64_t new_offset;
    int result;

    if (fi->magic != FS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((result=calc_file_offset_ex(fi, offset, whence,
                    true, &new_offset)) != 0)
    {
        return result;
    }

    fi->offset = new_offset;
    return 0;
}

static void fill_stat(const FDIRDEntryInfo *dentry, struct stat *buf)
{
    memset(buf, 0, sizeof(struct stat));
    buf->st_ino = dentry->inode;
    buf->st_mode = dentry->stat.mode;
    buf->st_size = dentry->stat.size;
    buf->st_mtime = dentry->stat.mtime;
    buf->st_ctime = dentry->stat.ctime;
}

int fsapi_fstat(FSAPIFileInfo *fi, struct stat *buf)
{
    int result;

    if (fi->magic != FS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((result=fdir_client_stat_dentry_by_inode(fi->ctx->contexts.
                    fdir, fi->dentry.inode, &fi->dentry)) != 0)
    {
        return result;
    }

    fill_stat(&fi->dentry, buf);
    return 0;
}

int fsapi_stat_ex(FSAPIContext *ctx, const char *path, struct stat *buf)
{
    int result;
    FDIRDEntryFullName fullname;
    FDIRDEntryInfo dentry;

    fullname.ns = ctx->ns;
    FC_SET_STRING(fullname.path, (char *)path);
    if ((result=fdir_client_stat_dentry_by_path(ctx->contexts.fdir,
                    &fullname, &dentry)) != 0)
    {
        return result;
    }

    fill_stat(&dentry, buf);
    return 0;
}

static inline int flock_lock(FSAPIFileInfo *fi, const int operation,
        const int64_t owner_id, const pid_t pid)
{
    int result;
    const int64_t offset = 0;
    const int64_t length = 0;

    if ((result=fdir_client_init_session(fi->ctx->contexts.fdir,
                    &fi->sessions.flock)) != 0)
    {
        return result;
    }

    if ((result=fdir_client_flock_dentry_ex2(&fi->sessions.flock,
                    fi->dentry.inode, operation, offset, length,
                    owner_id, pid)) != 0)
    {
        fdir_client_close_session(&fi->sessions.flock, result != 0);
    }

    return result;
}

static inline int flock_unlock(FSAPIFileInfo *fi, const int operation,
        const int64_t owner_id, const pid_t pid)
{
    int result;
    const int64_t offset = 0;
    const int64_t length = 0;

    result = fdir_client_flock_dentry_ex2(&fi->sessions.flock,
            fi->dentry.inode, operation, offset, length, owner_id, pid);
    fdir_client_close_session(&fi->sessions.flock, result != 0);
    return result;
}

int fsapi_flock_ex2(FSAPIFileInfo *fi, const int operation,
        const int64_t owner_id, const pid_t pid)
{
    if (fi->magic != FS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((operation & LOCK_UN)) {
        if (fi->sessions.flock.mconn == NULL) {
            return ENOENT;
        }
        return flock_unlock(fi, operation, owner_id, pid);
    } else if ((operation & LOCK_SH) || (operation & LOCK_EX)) {
        if (fi->sessions.flock.mconn != NULL) {
            logError("file: "__FILE__", line: %d, "
                    "flock on inode: %"PRId64" already exist",
                    __LINE__, fi->dentry.inode);
            return EEXIST;
        }
        return flock_lock(fi, operation, owner_id, pid);
    } else {
        return EINVAL;
    }
}

static inline int fcntl_lock(FSAPIFileInfo *fi, const int operation,
        const int64_t offset, const int64_t length,
        const int64_t owner_id, const pid_t pid)
{
    int result;
    if ((result=fdir_client_init_session(fi->ctx->contexts.fdir,
                    &fi->sessions.flock)) != 0)
    {
        return result;
    }

    if ((result=fdir_client_flock_dentry_ex2(&fi->sessions.flock,
                    fi->dentry.inode, operation, offset, length,
                    owner_id, pid)) != 0)
    {
        fdir_client_close_session(&fi->sessions.flock, result != 0);
    }

    return result;
}

static inline int fcntl_unlock(FSAPIFileInfo *fi, const int operation,
        const int64_t offset, const int64_t length,
        const int64_t owner_id, const pid_t pid)
{
    int result;
    result = fdir_client_flock_dentry_ex2(&fi->sessions.flock,
            fi->dentry.inode, operation, offset, length, owner_id, pid);
    fdir_client_close_session(&fi->sessions.flock, result != 0);
    return result;
}

static inline int fcntl_type_to_flock_op(const short type, int *operation)
{
    switch (type) {
        case F_RDLCK:
            *operation = LOCK_SH;
            break;
        case F_WRLCK:
            *operation = LOCK_EX;
            break;
        case F_UNLCK:
            *operation = LOCK_UN;
            break;
        default:
            return EINVAL;
    }

    return 0;
}

static inline int flock_op_to_fcntl_type(const int operation, short *type)
{
    switch (operation) {
        case LOCK_SH:
            *type = F_RDLCK;
            break;
        case LOCK_EX:
            *type = F_WRLCK;
            break;
        case LOCK_UN:
            *type = F_UNLCK;
            break;
        default:
            return EINVAL;
    }

    return 0;
}

int fsapi_setlk(FSAPIFileInfo *fi, const struct flock *lock,
        const int64_t owner_id)
{
    int operation;
    int result;
    int64_t offset;

    if (fi->magic != FS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((result=fcntl_type_to_flock_op(lock->l_type, &operation)) != 0) {
        return result;
    }

    if ((result=calc_file_offset(fi, lock->l_start,
                    lock->l_whence, &offset)) != 0)
    {
        return result;
    }

    if (operation == LOCK_UN) {
        if (fi->sessions.flock.mconn == NULL) {
            return ENOENT;
        }
        return fcntl_unlock(fi, operation, offset, lock->l_len,
                owner_id, lock->l_pid);
    } else {
        if (fi->sessions.flock.mconn != NULL) {
            logError("file: "__FILE__", line: %d, "
                    "flock on inode: %"PRId64" already exist",
                    __LINE__, fi->dentry.inode);
            return EEXIST;
        }
        return fcntl_lock(fi, operation, offset, lock->l_len,
                owner_id, lock->l_pid);
    }
}

int fsapi_getlk(FSAPIFileInfo *fi, struct flock *lock, int64_t *owner_id)
{
    int operation;
    int result;
    int64_t offset;
    int64_t length;

    if (fi->magic != FS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    if ((result=fcntl_type_to_flock_op(lock->l_type, &operation)) != 0) {
        return result;
    }

    if ((result=calc_file_offset(fi, lock->l_start,
                    lock->l_whence, &offset)) != 0)
    {
        return result;
    }

    if (operation == LOCK_UN) {
        return EINVAL;
    }

    length = lock->l_len;
    if ((result=fdir_client_getlk_dentry(fi->ctx->contexts.fdir,
                    fi->dentry.inode, &operation, &offset, &length,
                    owner_id, &lock->l_pid)) == 0)
    {
        flock_op_to_fcntl_type(operation, &lock->l_type);
        lock->l_whence = SEEK_SET;
        lock->l_start = offset;
        lock->l_len = length;
    }

    return result;
}

int fsapi_fallocate(FSAPIFileInfo *fi, const int mode,
        const int64_t offset, const int64_t length)
{
    FDIRClientSession session;
    int64_t old_size;
    int64_t new_size;
    int64_t space_end;
    int64_t alloc_bytes;
    string_t *ns;
    int op;
    int flags;
    int result;
    int unlock_res;

    if (offset < 0 || length < 0) {
        return EINVAL;
    }

    if (fi->magic != FS_API_MAGIC_NUMBER) {
        return EBADF;
    }

    op = mode & (~FALLOC_FL_KEEP_SIZE);
    if (!(op == 0 || op == FALLOC_FL_PUNCH_HOLE)) {
        return EOPNOTSUPP;
    }

    if (length == 0) {
        return 0;
    }

    if ((result=fsapi_dentry_sys_lock(&session, fi->dentry.inode,
                    0, &old_size, &space_end)) != 0)
    {
        return result;
    }

    flags = 0;
    if (op == 0) {   //allocate space
        result = do_allocate(fi->ctx, fi->dentry.inode,
                offset, length, &alloc_bytes);
        new_size = offset + length;
        if (new_size > space_end) {
            flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
        }
        if (new_size > old_size) {
            flags |= (mode & FALLOC_FL_KEEP_SIZE) ? 0 :
                FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
        }
    } else {  //deallocate space
        result = do_truncate(fi->ctx, fi->dentry.inode, space_end,
                offset, length, &alloc_bytes);
        if (offset + length >= old_size) {
            new_size = offset;
            flags |= (mode & FALLOC_FL_KEEP_SIZE) ? 0 :
                FDIR_DENTRY_FIELD_MODIFIED_FLAG_FILE_SIZE;
            if (new_size < space_end) {
                flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
            }
        } else {
            if (offset < space_end && offset + length >= space_end) {
                new_size = offset;
                flags |= FDIR_DENTRY_FIELD_MODIFIED_FLAG_SPACE_END;
            } else {
                new_size = old_size;
            }
        }
    }

    if (result == 0) {
        ns = &fi->ctx->ns; //set ns for update file_size, space_end etc.
    } else {
        ns = NULL;  //do NOT update
    }
    unlock_res = fsapi_dentry_sys_unlock(&session, ns, fi->dentry.inode,
            true, old_size, new_size, alloc_bytes, flags);

    return result == 0 ? unlock_res : result;
}

int fsapi_rename_ex(FSAPIContext *ctx, const char *old_path,
        const char *new_path, const int flags)
{
    FDIRDEntryFullName src_fullname;
    FDIRDEntryFullName dest_fullname;
    FDIRDEntryInfo dentry;
    FDIRDEntryInfo *pe;
    int result;

    src_fullname.ns = ctx->ns;
    FC_SET_STRING(src_fullname.path, (char *)old_path);
    dest_fullname.ns = ctx->ns;
    FC_SET_STRING(dest_fullname.path, (char *)new_path);

    pe = &dentry;
    if ((result=fdir_client_rename_dentry_ex(ctx->contexts.fdir,
                    &src_fullname, &dest_fullname, flags, &pe)) != 0)
    {
        return result;
    }

    if (pe != NULL && S_ISREG(pe->stat.mode)) {
        fs_unlink_file(ctx->contexts.fs, pe->inode, pe->stat.size);
    }

    return result;
}

int fsapi_symlink_ex(FSAPIContext *ctx, const char *target,
        const char *path, const mode_t mode)
{
    FDIRDEntryFullName fullname;
    string_t link;
    FDIRDEntryInfo dentry;

    fullname.ns = ctx->ns;
    FC_SET_STRING(fullname.path, (char *)path);
    FC_SET_STRING(link, (char *)target);
    return fdir_client_symlink_dentry(ctx->contexts.fdir,
            &link, &fullname, mode, &dentry);
}

int fsapi_readlink(FSAPIContext *ctx, const char *path,
        char *buff, const int size)
{
    FDIRDEntryFullName fullname;
    string_t link;

    fullname.ns = ctx->ns;
    FC_SET_STRING(fullname.path, (char *)path);
    link.str = buff;
    return fdir_client_readlink_by_path(ctx->contexts.fdir,
        &fullname, &link, size);
}

int fsapi_link_ex(FSAPIContext *ctx, const char *old_path,
        const char *new_path, const mode_t mode)
{
    FDIRDEntryFullName src_fullname;
    FDIRDEntryFullName dest_fullname;
    FDIRDEntryInfo dentry;

    src_fullname.ns = ctx->ns;
    FC_SET_STRING(src_fullname.path, (char *)old_path);
    dest_fullname.ns = ctx->ns;
    FC_SET_STRING(dest_fullname.path, (char *)new_path);

    return fdir_client_link_dentry(ctx->contexts.fdir,
            &src_fullname, &dest_fullname, mode, &dentry);
}
