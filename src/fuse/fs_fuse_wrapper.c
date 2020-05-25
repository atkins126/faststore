#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "fastcommon/common_define.h"
#include "fastcommon/sched_thread.h"
#include "fs_fuse_wrapper.h"

#define FS_ATTR_TIMEOUT  1.0
#define FS_ENTRY_TIMEOUT 1.0

static struct fast_mblock_man fh_allocator;

static void fill_stat(const FDIRDEntryInfo *dentry, struct stat *stat)
{
    stat->st_ino = dentry->inode;
    stat->st_mode = dentry->stat.mode;
    stat->st_size = dentry->stat.size;
    stat->st_atime = dentry->stat.atime;
    stat->st_mtime = dentry->stat.mtime;
    stat->st_ctime = dentry->stat.ctime;
    stat->st_uid = dentry->stat.uid;
    stat->st_gid = dentry->stat.gid;

    stat->st_blksize = 512;
    if (stat->st_size > 0) {
        stat->st_blocks = (stat->st_size + stat->st_blksize - 1) /
            stat->st_blksize;
    }
    stat->st_nlink = 1;
}

static void fill_entry_param(const FDIRDEntryInfo *dentry,
        struct fuse_entry_param *param)
{
    memset(param, 0, sizeof(*param));
    param->ino = dentry->inode;
    param->attr_timeout = FS_ATTR_TIMEOUT;
    param->entry_timeout = FS_ENTRY_TIMEOUT;
    fill_stat(dentry, &param->attr);
}

static inline int fs_convert_inode(const fuse_ino_t ino, int64_t *new_inode)
{
    int result;
    static int64_t root_inode = 0;

    if (ino == FUSE_ROOT_ID) {
        if (root_inode == 0) {
            if ((result=fsapi_lookup_inode("/", new_inode)) != 0) {
                return result;
            }
            root_inode = *new_inode;
        } else {
            *new_inode = root_inode;
        }
    } else {
        *new_inode = ino;
    }
    return 0;
}

static inline void do_reply_attr(fuse_req_t req, FDIRDEntryInfo *dentry)
{
    struct stat stat;
    memset(&stat, 0, sizeof(stat));
    fill_stat(dentry, &stat);
    fuse_reply_attr(req, &stat, FS_ATTR_TIMEOUT);
}

static void fs_do_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
    int64_t new_inode;
    FDIRDEntryInfo dentry;

    if (fs_convert_inode(ino, &new_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    fprintf(stderr, "file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", new_inode: %"PRId64", fi: %p\n",
            __LINE__, __FUNCTION__, ino, new_inode, fi);

    if (fsapi_stat_dentry_by_inode(new_inode, &dentry) == 0) {
        do_reply_attr(req, &dentry);
    } else {
        fuse_reply_err(req, ENOENT);
    }
}

void fs_do_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
             int to_set, struct fuse_file_info *fi)
{
    int result;
    int64_t new_inode;
    FDIRStatModifyFlags options;
    FDIRDEntryInfo *pe;
    FDIRDEntryInfo dentry;

    logInfo("=====file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fi: %p ====",
            __LINE__, __FUNCTION__, ino, fi);

    options.flags = 0;
    if ((to_set & FUSE_SET_ATTR_MODE)) {
        options.mode = 1;
    }

    if ((to_set & FUSE_SET_ATTR_UID)) {
        options.uid = 1;

        logInfo("file: "__FILE__", line: %d, func: %s, "
                "set uid: %d", __LINE__, __FUNCTION__, attr->st_gid);
    }

    if ((to_set & FUSE_SET_ATTR_GID)) {
        options.gid = 1;

        logInfo("file: "__FILE__", line: %d, func: %s, "
                "set gid: %d", __LINE__, __FUNCTION__, attr->st_gid);
    }

    if ((to_set & FUSE_SET_ATTR_SIZE)) {
        FSAPIFileInfo *fh;
        if (fi == NULL) {
            fuse_reply_err(req, EBADF);
            return;
        }

        fh = (FSAPIFileInfo *)fi->fh;
        if (fh == NULL) {
            fuse_reply_err(req, EBADF);
            return;
        }

        logInfo("file: "__FILE__", line: %d, func: %s, "
                "SET file size from %"PRId64" to: %"PRId64,
                __LINE__, __FUNCTION__, fh->dentry.stat.size,
                (int64_t)attr->st_size);

        if ((result=fsapi_ftruncate(fh, attr->st_size)) != 0) {
            fuse_reply_err(req, result);
            return;
        }

        fh->dentry.stat.size = attr->st_size;
        pe = &fh->dentry;
    } else {
        pe = NULL;
    }

    if ((to_set & FUSE_SET_ATTR_CTIME)) {
        options.ctime = 1;
    }

    if ((to_set & FUSE_SET_ATTR_ATIME)) {
        options.atime = 1;
    } else if ((to_set & FUSE_SET_ATTR_ATIME_NOW)) {
        options.atime = 1;
        //attr->st_atime = get_current_time();
    }

    if ((to_set & FUSE_SET_ATTR_MTIME)) {
        options.mtime = 1;
    } else if ((to_set & FUSE_SET_ATTR_MTIME_NOW)) {
        options.mtime = 1;
        //attr->st_mtime = get_current_time();
    }

    if (fs_convert_inode(ino, &new_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, new_inode: %"PRId64", "
            "flags: %"PRId64", atime bit: %d, mtime bit: %d",
            __LINE__, __FUNCTION__, new_inode, options.flags,
            options.atime, options.mtime);

    if (options.flags == 0) {
        if (pe == NULL) {
            pe = &dentry;
            result = fsapi_stat_dentry_by_inode(new_inode, &dentry);
        } else {
            result = 0;
        }
    } else {
        pe = &dentry;
        result = fsapi_modify_dentry_stat(ino, attr, options.flags, &dentry);
    }
    if (result != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    do_reply_attr(req, pe);
}

static void fs_do_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int result;
    int64_t parent_inode;
    FDIRDEntryInfo dentry;
    string_t nm;
    struct fuse_entry_param param;

    fprintf(stderr, "parent1: %"PRId64", name: %s\n", parent, name);

    if (fs_convert_inode(parent, &parent_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    fprintf(stderr, "parent2: %"PRId64", name: %s\n", parent_inode, name);

    FC_SET_STRING(nm, (char *)name);
    if ((result=fsapi_stat_dentry_by_pname(parent_inode, &nm, &dentry)) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    fill_entry_param(&dentry, &param);
    fuse_reply_entry(req, &param);
}

static int dentry_list_to_buff(fuse_req_t req, FSAPIOpendirSession *session)
{
    FDIRClientDentry *dentry;
    FDIRClientDentry *end;
    struct stat stat;
    int result;
    int len;
    int next_offset;
    char name[NAME_MAX];

    fast_buffer_reset(&session->buffer);
    if (session->array.count == 0) {
        return 0;
    }

    memset(&stat, 0, sizeof(stat));
    end = session->array.entries + session->array.count;
    for (dentry=session->array.entries; dentry<end; dentry++) {
        snprintf(name, sizeof(name), "%.*s",
                dentry->name.len, dentry->name.str);
        len = fuse_add_direntry(req, NULL, 0, name, NULL, 0);
        next_offset = session->buffer.length + len;
        if (next_offset > session->buffer.alloc_size) {
            if ((result=fast_buffer_set_capacity(&session->buffer,
                            next_offset)) != 0)
            {
                return result;
            }
        }

        stat.st_ino = dentry->inode;
        fuse_add_direntry(req, session->buffer.data + session->buffer.length,
                session->buffer.alloc_size - session->buffer.length,
                name, &stat, next_offset);
        session->buffer.length = next_offset;
    }

    return 0;
}

static void fs_do_opendir(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi)
{
    int64_t new_inode;
    FSAPIOpendirSession *session;
    int result;

    if (fs_convert_inode(ino, &new_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if ((session=fsapi_alloc_opendir_session()) == NULL) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    do {
        if ((result=fsapi_list_dentry_by_inode(new_inode,
                        &session->array)) != 0)
        {
            break;
        }

        if ((result=dentry_list_to_buff(req, session)) != 0) {
            break;
        }
    } while (0);

    if (result != 0) {
        fsapi_free_opendir_session(session);
        fuse_reply_err(req, result);
    } else {
        fi->fh = (long)session;
        fuse_reply_open(req, fi);
    }
}

static void fs_do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			     off_t offset, struct fuse_file_info *fi)
{
    FSAPIOpendirSession *session;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64", offset: %"PRId64", size: %"PRId64"\n",
            __LINE__, __FUNCTION__, ino, fi->fh,
            (int64_t)offset, (int64_t)size);

    session = (FSAPIOpendirSession *)fi->fh;
    if (session == NULL) {
        fuse_reply_err(req, EBUSY);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, offset: %d, length: %d",
            __LINE__, __FUNCTION__, (int)offset, session->buffer.length);

    if (offset < session->buffer.length) {
        fuse_reply_buf(req, session->buffer.data + offset,
                FC_MIN(session->buffer.length - offset, size));
    } else {
        fuse_reply_buf(req, NULL, 0);
    }
}

static void fs_do_releasedir(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi)
{
    FSAPIOpendirSession *session;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64,
            __LINE__, __FUNCTION__, ino, fi->fh);

    session = (FSAPIOpendirSession *)fi->fh;
    if (session != NULL) {
        fsapi_free_opendir_session(session);
        fi->fh = 0;
    }

    fuse_reply_err(req, 0);
}

static int do_open(fuse_req_t req, FDIRDEntryInfo *dentry,
        struct fuse_file_info *fi)
{
    int result;
    FSAPIFileInfo *fh;

    fh = (FSAPIFileInfo *)fast_mblock_alloc_object(&fh_allocator);
    if (fh == NULL) {
        return ENOMEM;
    }

    if ((result=fsapi_open_by_dentry(fh, dentry, fi->flags)) != 0) {
        logError("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64", flags: %d, result: %d\n",
            __LINE__, __FUNCTION__, dentry->inode, fi->fh, fi->flags, result);

        fast_mblock_free_object(&fh_allocator, fh);
        if (!(result == EISDIR || result == ENOENT)) {
            result = EACCES;
        }
        return result;
    }

    fi->fh = (long)fh;
    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64"\n",
            __LINE__, __FUNCTION__, dentry->inode, fi->fh);

    fi->fh = (long)fh;
    return 0;
}

static void fs_do_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    int64_t new_inode;

    if (fs_convert_inode(ino, &new_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "inode: %"PRId64", mask: %d",
            __LINE__, __FUNCTION__, new_inode, mask);

    fuse_reply_err(req, 0);
}

static void fs_do_create(fuse_req_t req, fuse_ino_t parent,
        const char *name, mode_t mode, struct fuse_file_info *fi)
{
    int result;
    int64_t parent_inode;
    string_t nm;
    FDIRDEntryInfo dentry;
    struct fuse_entry_param param;

    if (fs_convert_inode(parent, &parent_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "parent ino: %"PRId64", fi: %p, name: %s",
            __LINE__, __FUNCTION__, parent_inode, fi, name);

    FC_SET_STRING(nm, (char *)name);
    if ((result=fsapi_create_dentry_by_pname(parent_inode, &nm,
                    mode, &dentry)) != 0)
    {
        if (result != EEXIST) {
            fuse_reply_err(req, ENOENT);
            return;
        }

        if ((fi->flags & O_EXCL)) {
            fuse_reply_err(req, EEXIST);
            return;
        }

        if ((result=fsapi_stat_dentry_by_pname(parent_inode,
                        &nm, &dentry)) != 0)
        {
            fuse_reply_err(req, ENOENT);
            return;
        }
    }

    fi->flags &= ~(O_CREAT | O_EXCL);
    if ((result=do_open(req, &dentry, fi)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fill_entry_param(&dentry, &param);
    fuse_reply_create(req, &param, fi);
}

static void do_mknod(fuse_req_t req, fuse_ino_t parent,
        const char *name, mode_t mode, dev_t rdev)
{
    int result;
    int64_t parent_inode;
    string_t nm;
    FDIRDEntryInfo dentry;
    struct fuse_entry_param param;

    if (fs_convert_inode(parent, &parent_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "parent ino: %"PRId64", name: %s, mode: %03o, isdir: %d",
            __LINE__, __FUNCTION__, parent_inode, name, mode, S_ISDIR(mode));

    FC_SET_STRING(nm, (char *)name);
    if ((result=fsapi_create_dentry_by_pname(parent_inode, &nm,
                    mode, &dentry)) != 0)
    {
        if (result == EEXIST || result == ENOENT) {
            fuse_reply_err(req, result);
        } else {
            fuse_reply_err(req, ENOENT);
        }
        return;
    }

    fill_entry_param(&dentry, &param);
    fuse_reply_entry(req, &param);
}

static void fs_do_mknod(fuse_req_t req, fuse_ino_t parent,
        const char *name, mode_t mode, dev_t rdev)
{
    do_mknod(req, parent, name, mode, rdev);
}

static void fs_do_mkdir(fuse_req_t req, fuse_ino_t parent,
        const char *name, mode_t mode)
{
    mode |= S_IFDIR;
    do_mknod(req, parent, name, mode, 0);
}

static int remove_dentry(fuse_ino_t parent, const char *name)
{
    int64_t parent_inode;
    string_t nm;

    if (fs_convert_inode(parent, &parent_inode) != 0) {
        return ENOENT;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "parent ino: %"PRId64", name: %s",
            __LINE__, __FUNCTION__, parent_inode, name);

    FC_SET_STRING(nm, (char *)name);
    return fsapi_remove_dentry_by_pname(parent_inode, &nm);
}

static void fs_do_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int result;
    if ((result=remove_dentry(parent, name)) != 0) {
        result = ENOENT;
    }
    fuse_reply_err(req, result);
}

static void fs_do_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    int result;
    if ((result=remove_dentry(parent, name)) != 0) {
        result = ENOENT;
    }
    fuse_reply_err(req, result);
}

void fs_do_rename(fuse_req_t req, fuse_ino_t oldparent, const char *oldname,
            fuse_ino_t newparent, const char *newname, unsigned int flags)
{
    int64_t old_parent_inode;
    int64_t new_parent_inode;
    string_t old_nm;
    string_t new_nm;
    int result;

    if (fs_convert_inode(oldparent, &old_parent_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (fs_convert_inode(newparent, &new_parent_inode) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "parent ino: %"PRId64", name: %s, "
            "newparent ino: %"PRId64", new name: %s",
            __LINE__, __FUNCTION__, old_parent_inode, oldname,
            new_parent_inode, newname);

    FC_SET_STRING(old_nm, (char *)oldname);
    FC_SET_STRING(new_nm, (char *)newname);
    result = fsapi_rename_dentry_by_pname(old_parent_inode, &old_nm,
            new_parent_inode, &new_nm, flags);
    fuse_reply_err(req, result);
}

static void fs_do_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", nlookup: %"PRId64,
            __LINE__, __FUNCTION__, ino, nlookup);
    fuse_reply_none(req);
}

static void fs_do_forget_multi(fuse_req_t req, size_t count,
        struct fuse_forget_data *forgets)
{
    logInfo("file: "__FILE__", line: %d, func: %s, "
            "count: %d", __LINE__, __FUNCTION__, (int)count);
    fuse_reply_none(req);
}

static void fs_do_open(fuse_req_t req, fuse_ino_t ino,
			  struct fuse_file_info *fi)
{
    int result;
    FDIRDEntryInfo dentry;

    if ((result=fsapi_stat_dentry_by_inode(ino, &dentry)) != 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }


    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64", O_APPEND flag: %d",
            __LINE__, __FUNCTION__, ino, fi->fh, (fi->flags & O_APPEND));

    if ((result=do_open(req, &dentry, fi)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fuse_reply_open(req, fi);
}

static void fs_do_flush(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi)
{
    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64"\n",
            __LINE__, __FUNCTION__, ino, fi->fh);

    fuse_reply_err(req, 0);
}

static void fs_do_fsync(fuse_req_t req, fuse_ino_t ino,
        int datasync, struct fuse_file_info *fi)
{
    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64", datasync: %d",
            __LINE__, __FUNCTION__, ino, fi->fh, datasync);
    fuse_reply_err(req, 0);
}

static void fs_do_release(fuse_req_t req, fuse_ino_t ino,
             struct fuse_file_info *fi)
{
    int result;
    FSAPIFileInfo *fh;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64"\n",
            __LINE__, __FUNCTION__, ino, fi->fh);

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh != NULL) {
        result = fsapi_close(fh);
        fast_mblock_free_object(&fh_allocator, fh);
    } else {
        result = EBADF;
    }
    fuse_reply_err(req, result);
}

//static int write_fd = -1;

static void fs_do_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			  off_t offset, struct fuse_file_info *fi)
{
    FSAPIFileInfo *fh;
    int result;
    int read_bytes;
    char fixed_buff[128 * 1024];
    char *buff;
    
    if (size < sizeof(fixed_buff)) {
        buff = fixed_buff;
    } else if ((buff=(char *)malloc(size)) == NULL) {
        logError("file: "__FILE__", line: %d, func: %s, "
                "malloc %d bytes fail", __LINE__, __FUNCTION__, (int)size);
        fuse_reply_err(req, ENOMEM);
        return;
    }

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        fuse_reply_err(req, EBADF);
        return;
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %p, size: %"PRId64", offset: %"PRId64,
            __LINE__, __FUNCTION__, ino, fh, size, offset);

    if ((result=fsapi_pread(fh, buff, size, offset, &read_bytes)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fuse_reply_buf(req, buff, read_bytes);
    if (buff != fixed_buff) {
        free(buff);
    }
}

void fs_do_write(fuse_req_t req, fuse_ino_t ino, const char *buff,
        size_t size, off_t offset, struct fuse_file_info *fi)
{
    FSAPIFileInfo *fh;
    int result;
    int written_bytes;

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        fuse_reply_err(req, EBADF);
        return;
    }

    logInfo("=======file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", size: %"PRId64", offset: %"PRId64,
            __LINE__, __FUNCTION__, ino, size, offset);

    if ((result=fsapi_pwrite(fh, buff, size, offset, &written_bytes)) != 0) {
        fuse_reply_err(req, result);
        return;
    }


    logInfo("=======file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", size: %"PRId64", offset: %"PRId64", written_bytes: %d",
            __LINE__, __FUNCTION__, ino, size, offset, written_bytes);

    fuse_reply_write(req, written_bytes);
}

void fs_do_lseek(fuse_req_t req, fuse_ino_t ino, off_t offset,
        int whence, struct fuse_file_info *fi)
{
    FSAPIFileInfo *fh;
    int result;

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        fuse_reply_err(req, EBADF);
        return;
    }

    logInfo("@@@@@@@ file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", offset: %"PRId64", whence: %d @@@@@@",
            __LINE__, __FUNCTION__, ino, (int64_t)offset, whence);

    if ((result=fsapi_lseek(fh, offset, whence)) != 0) {
        fuse_reply_err(req, result);
        return;
    }

    fuse_reply_lseek(req, fh->offset);
}

static void fs_do_getlk(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi, struct flock *lock)
{
    int result;
    FSAPIFileInfo *fh;
    int64_t owner_id;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64", type: %d, pid: %d",
            __LINE__, __FUNCTION__, ino, fi->fh, lock->l_type, lock->l_pid);

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        result = EBADF;
    } else {
        result = fsapi_getlk(fh, lock, &owner_id);
    }

    if (result == 0) {
        fuse_reply_lock(req, lock);
    } else {
        fuse_reply_err(req, result);
    }
}

static void fs_do_setlk(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi, struct flock *lock, int sleep)
{
    int result;
    FSAPIFileInfo *fh;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64", lock_owner: %"PRId64", pid: %d",
            __LINE__, __FUNCTION__, ino, fi->fh, fi->lock_owner, lock->l_pid);

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        result = EBADF;
    } else {
        result = fsapi_setlk(fh, lock, fi->lock_owner);
    }
    fuse_reply_err(req, result);
}

static void fs_do_flock(fuse_req_t req, fuse_ino_t ino,
        struct fuse_file_info *fi, int op)
{
    int result;
    FSAPIFileInfo *fh;

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", fh: %"PRId64", lock_owner: %"PRId64", op: %d, operation: %d",
            __LINE__, __FUNCTION__, ino, fi->fh, fi->lock_owner, op,
            (op & (LOCK_SH | LOCK_EX | LOCK_UN)));

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        result = EBADF;
    } else {
        result = fsapi_flock_ex(fh, op, fi->lock_owner);
    }
    fuse_reply_err(req, result);
}

static void fs_do_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct statvfs stbuf;

    if (statvfs("/", &stbuf) < 0) {
        fuse_reply_err(req, errno != 0 ? errno : ENOENT);
    }

    logInfo("file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64, __LINE__, __FUNCTION__, ino);

    fuse_reply_statfs(req, &stbuf);
}

static void fs_do_fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
        off_t offset, off_t length, struct fuse_file_info *fi)
{
    int result;
    FSAPIFileInfo *fh;

    logInfo("!!!!!!!!!! file: "__FILE__", line: %d, func: %s, "
            "ino: %"PRId64", mode: %d", __LINE__, __FUNCTION__, ino, mode);

    fh = (FSAPIFileInfo *)fi->fh;
    if (fh == NULL) {
        result = EBADF;
    } else {
        result = fsapi_fallocate(fh, mode, offset, len);
    }

    fuse_reply_err(req, result);
}

int fs_fuse_wrapper_init(struct fuse_lowlevel_ops *ops)
{
    int result;
    if ((result=fast_mblock_init_ex2(&fh_allocator, "fuse_fh",
                    sizeof(FSAPIFileInfo), 4096, NULL, NULL,
                    true, NULL, NULL, NULL)) != 0)
    {
        return result;
    }

    /*
    const char *filename = "/tmp/fuse.dat";
    write_fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0775);
    if (write_fd < 0) {
        logInfo("file: "__FILE__", line: %d, func: %s, "
                "open file %s fail, errno: %d, error info: %s",
                __LINE__, __FUNCTION__, filename, errno, strerror(errno));
        return errno != 0 ? errno : EPERM;
    }
    */

    ops->lookup  = fs_do_lookup;
    ops->getattr = fs_do_getattr;
    ops->setattr = fs_do_setattr;
    ops->opendir = fs_do_opendir;
    ops->readdir = fs_do_readdir;
    ops->releasedir = fs_do_releasedir;
    ops->create  = fs_do_create;
    ops->access  = fs_do_access;
    ops->open    = fs_do_open;
    ops->fsync   = fs_do_fsync;
    ops->flush   = fs_do_flush;
    ops->release = fs_do_release;
    ops->read    = fs_do_read;
    ops->write   = fs_do_write;
    ops->mknod   = fs_do_mknod;
    ops->mkdir   = fs_do_mkdir;
    ops->rmdir   = fs_do_rmdir;
    ops->unlink  = fs_do_unlink;
    ops->rename  = fs_do_rename;
    ops->forget  = fs_do_forget;
    ops->forget_multi = fs_do_forget_multi;
    ops->lseek   = fs_do_lseek;
    ops->getlk   = fs_do_getlk;
    ops->setlk   = fs_do_setlk;
    ops->flock   = fs_do_flock;
    ops->statfs  = fs_do_statfs;
    ops->fallocate = fs_do_fallocate;

    return 0;
}
