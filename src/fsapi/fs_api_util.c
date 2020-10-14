/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <limits.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/sched_thread.h"
#include "fs_api_util.h"

int fsapi_remove_dentry_by_pname_ex(FSAPIContext *ctx,
        const int64_t parent_inode, const string_t *name)
{
    FDIRDEntryPName pname;
    FDIRDEntryInfo dentry;
    int result;

    FDIR_SET_DENTRY_PNAME_PTR(&pname, parent_inode, name);
    if ((result=fdir_client_stat_dentry_by_pname(ctx->contexts.fdir,
                    &pname, &dentry)) != 0)
    {
        return result;
    }

    if (S_ISREG(dentry.stat.mode)) {
        result = fs_unlink_file(ctx->contexts.fs, dentry.inode,
                dentry.stat.size);
    } else {
        result = 0;
    }

    if (result == 0) {
        result = fdir_client_remove_dentry_by_pname(
                ctx->contexts.fdir, &ctx->ns, &pname);
    }
    return result;
}

int fsapi_rename_dentry_by_pname_ex(FSAPIContext *ctx,
        const int64_t src_parent_inode, const string_t *src_name,
        const int64_t dest_parent_inode, const string_t *dest_name,
        const int flags)
{
    FDIRDEntryPName src_pname;
    FDIRDEntryPName dest_pname;
    FDIRDEntryInfo dentry;
    FDIRDEntryInfo *pe;
    int result;

    FDIR_SET_DENTRY_PNAME_PTR(&src_pname, src_parent_inode, src_name);
    FDIR_SET_DENTRY_PNAME_PTR(&dest_pname, dest_parent_inode, dest_name);
    pe = &dentry;
    if ((result=fdir_client_rename_dentry_by_pname_ex(ctx->contexts.fdir,
                    &ctx->ns, &src_pname, &ctx->ns, &dest_pname, flags,
                    &pe)) != 0)
    {
        return result;
    }

    if (pe != NULL && S_ISREG(pe->stat.mode)) {
        fs_unlink_file(ctx->contexts.fs, pe->inode, pe->stat.size);
    }
    return result;
}
