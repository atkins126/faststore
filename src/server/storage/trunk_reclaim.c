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

#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/fast_mblock.h"
#include "fastcommon/fc_queue.h"
#include "sf/sf_global.h"
#include "../server_global.h"
#include "trunk_reclaim.h"

typedef struct trunk_reclaim_task_info {
    bool urgent;
    struct trunk_reclaim_task_info *next;
} TrunkReclaimTaskInfo;

typedef struct trunk_reclaim_thread_context {
    struct fast_mblock_man task_allocator;
    struct fc_queue queue;
    pthread_t tid;
    bool running;
} TrunkReclaimThreadContext;

static TrunkReclaimThreadContext reclaim_thread_ctx;

static int trunk_reclaim_deal_task(TrunkReclaimTaskInfo *task)
{
    return 0;
}

static void *trunk_reclaim_thread_func(void *arg)
{
    TrunkReclaimThreadContext *thread;
    TrunkReclaimTaskInfo *task;

    thread = (TrunkReclaimThreadContext *)arg;
    thread->running = true;
    while (SF_G_CONTINUE_FLAG) {
        task = (TrunkReclaimTaskInfo *)fc_queue_pop(&thread->queue);
        if (task == NULL) {
            continue;
        }

        if (trunk_reclaim_deal_task(task) != 0) {
            logCrit("file: "__FILE__", line: %d, "
                    "deal_binlog_records fail, "
                    "program exit!", __LINE__);
        }

        fast_mblock_free_object(&thread->task_allocator, task);
    }

    thread->running = false;
    return NULL;
}

int trunk_reclaim_init()
{
    int result;

    if ((result=fast_mblock_init_ex1(&reclaim_thread_ctx.task_allocator,
                    "trunk_reclaim_task", sizeof(TrunkReclaimTaskInfo),
                    1024, 0, NULL, NULL, true)) != 0)
    {
        return result;
    }

    if ((result=fc_queue_init(&reclaim_thread_ctx.queue, (long)
                    (&((TrunkReclaimTaskInfo *)NULL)->next))) != 0)
    {
        return result;
    }

    return fc_create_thread(&reclaim_thread_ctx.tid,
            trunk_reclaim_thread_func, &reclaim_thread_ctx,
            SF_G_THREAD_STACK_SIZE);
}
