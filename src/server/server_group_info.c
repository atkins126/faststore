#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include "fastcommon/logger.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/local_ip_func.h"
#include "server_global.h"
#include "cluster_topology.h"
#include "server_group_info.h"

typedef struct {
    int id1;
    int id2;
    int offset;
} ServerPairBaseIndexEntry;

typedef struct {
    int count;
    ServerPairBaseIndexEntry *entries;
} ServerPairBaseIndexArray;

#define DATA_GROUP_INFO_FILENAME           "data_group.info"

#define DATA_GROUP_SECTION_PREFIX_STR      "data-group-"
#define SERVER_GROUP_INFO_ITEM_VERSION     "version"
#define SERVER_GROUP_INFO_ITEM_IS_LEADER   "is_leader"
#define SERVER_GROUP_INFO_ITEM_SERVER      "server"

static ServerPairBaseIndexArray server_pair_index_array = {0, NULL};
static time_t last_shutdown_time = 0;
static int last_refresh_file_time = 0;

static int server_group_info_write_to_file(const uint64_t current_version);

static int init_cluster_data_server_array(FSClusterDataGroupInfo *group)
{
    FSServerGroup *server_group;
    FCServerInfo **pp;
    FCServerInfo **end;
    FSClusterDataServerInfo *sp;
    int bytes;

    if ((server_group=fs_cluster_cfg_get_server_group(&CLUSTER_CONFIG_CTX,
                    group->data_group_id - 1)) == NULL)
    {
        return ENOENT;
    }

    bytes = sizeof(FSClusterDataServerInfo) * server_group->server_array.count;
    group->data_server_array.servers = (FSClusterDataServerInfo *)malloc(bytes);
    if (group->data_server_array.servers == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(group->data_server_array.servers, 0, bytes);
    group->data_server_array.count = server_group->server_array.count;

    bytes = sizeof(FSClusterServerInfo *) * server_group->server_array.count;
    group->active_slaves.servers = (FSClusterServerInfo **)malloc(bytes);
    if (group->active_slaves.servers == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(group->active_slaves.servers, 0, bytes);

    end = server_group->server_array.servers + server_group->server_array.count;
    for (pp=server_group->server_array.servers,
            sp=group->data_server_array.servers;
            pp < end; pp++, sp++)
    {
        sp->cs = fs_get_server_by_id((*pp)->id);
        sp->index = sp - group->data_server_array.servers;
    }

    return 0;
}

static int init_cluster_data_group_array(const char *filename)
{
    FSIdArray *id_array;
    FSClusterDataGroupInfo *group;
    int result;
    int bytes;
    int count;
    int min_id;
    int max_id;
    int data_group_id;
    int i;

    if ((id_array=fs_cluster_cfg_get_server_group_ids(&CLUSTER_CONFIG_CTX,
            CLUSTER_MYSELF_PTR->server->id)) == NULL)
    {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, no data group",
                __LINE__, filename);
        return ENOENT;
    }

    if ((min_id=fs_cluster_cfg_get_server_min_group_id(&CLUSTER_CONFIG_CTX,
                    CLUSTER_MYSELF_PTR->server->id)) <= 0)
    {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, no data group",
                __LINE__, filename);
        return ENOENT;
    }
    if ((max_id=fs_cluster_cfg_get_server_max_group_id(&CLUSTER_CONFIG_CTX,
                    CLUSTER_MYSELF_PTR->server->id)) <= 0)
    {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, no data group",
                __LINE__, filename);
        return ENOENT;
    }

    count = (max_id - min_id) + 1;
    bytes = sizeof(FSClusterDataGroupInfo) * count;
    CLUSTER_DATA_RGOUP_ARRAY.groups = (FSClusterDataGroupInfo *)malloc(bytes);
    if (CLUSTER_DATA_RGOUP_ARRAY.groups == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(CLUSTER_DATA_RGOUP_ARRAY.groups, 0, bytes);

    for (i=0; i<id_array->count; i++) {
        data_group_id = id_array->ids[i];
        group = CLUSTER_DATA_RGOUP_ARRAY.groups + (data_group_id - min_id);
        group->data_group_id = data_group_id;
        if ((result=init_cluster_data_server_array(group)) != 0) {
            return result;
        }
    }

    CLUSTER_DATA_RGOUP_ARRAY.count = count;
    CLUSTER_DATA_RGOUP_ARRAY.base_id = min_id;
    return 0;
}

static FCServerInfo *get_myself_in_cluster_cfg(const char *filename,
        int *err_no)
{
    const char *local_ip;
    struct {
        const char *ip_addr;
        int port;
    } found;
    FCServerInfo *server;
    FCServerInfo *myself;
    int ports[2];
    int count;
    int i;

    count = 0;
    ports[count++] = g_sf_context.inner_port;
    if (g_sf_context.outer_port != g_sf_context.inner_port) {
        ports[count++] = g_sf_context.outer_port;
    }

    myself = NULL;
    found.ip_addr = NULL;
    found.port = 0;
    local_ip = get_first_local_ip();
    while (local_ip != NULL) {
        for (i=0; i<count; i++) {
            server = fc_server_get_by_ip_port(&SERVER_CONFIG_CTX,
                    local_ip, ports[i]);
            if (server != NULL) {
                if (myself == NULL) {
                    myself = server;
                } else if (myself != server) {
                    logError("file: "__FILE__", line: %d, "
                            "cluster config file: %s, my ip and port "
                            "in more than one servers, %s:%d in "
                            "server id %d, and %s:%d in server id %d",
                            __LINE__, filename, found.ip_addr, found.port,
                            myself->id, local_ip, ports[i], server->id);
                    *err_no = EEXIST;
                    return NULL;
                }
            }

            found.ip_addr = local_ip;
            found.port = ports[i];
        }

        local_ip = get_next_local_ip(local_ip);
    }

    if (myself == NULL) {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, can't find myself "
                "by my local ip and listen port", __LINE__, filename);
        *err_no = ENOENT;
    }
    return myself;
}

static int compare_server_ptr(const void *p1, const void *p2)
{
    return (*((FCServerInfo **)p1))->id - (*((FCServerInfo **)p2))->id;
}

static int init_cluster_server_array(const char *filename)
{
#define MAX_GROUP_SERVERS 64
    int bytes;
    int result;
    int count;
    FCServerInfo *svr;
    FSClusterServerInfo *cs;
    FCServerInfo *servers[MAX_GROUP_SERVERS];
    FCServerInfo **server;
    FCServerInfo **end;

    if ((svr=get_myself_in_cluster_cfg(filename, &result)) == NULL) {
        return result;
    }

    if ((result=fs_cluster_cfg_get_group_servers(&CLUSTER_CONFIG_CTX,
                    svr->id, servers, MAX_GROUP_SERVERS, &count)) != 0)
    {
        logError("file: "__FILE__", line: %d, "
                "get group servers fail, errno: %d, error info: %s",
                __LINE__, result, STRERROR(result));
        return result;
    }
    qsort(servers, count, sizeof(FCServerInfo *), compare_server_ptr);

    bytes = sizeof(FSClusterServerInfo) * count;
    CLUSTER_SERVER_ARRAY.servers = (FSClusterServerInfo *)malloc(bytes);
    if (CLUSTER_SERVER_ARRAY.servers == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(CLUSTER_SERVER_ARRAY.servers, 0, bytes);

    end = servers + count;
    for (server=servers, cs=CLUSTER_SERVER_ARRAY.servers;
            server<end; server++, cs++)
    {
        logInfo("%d. id = %d", (int)(server - servers) + 1, (*server)->id);
        cs->server = *server;
    }

    CLUSTER_SERVER_ARRAY.count = count;
    return 0;
}

static int init_cluster_notify_contexts()
{
    FSClusterServerInfo *cs;
    FSClusterServerInfo *end;
    int result;

    end = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    for (cs=CLUSTER_SERVER_ARRAY.servers; cs<end; cs++) {
        if ((result=cluster_topology_init_notify_ctx(&cs->notify_ctx)) != 0) {
            return result;
        }
    }

    return 0;
}

static int compare_server_pair_entry(const void *p1, const void *p2)
{
    int sub;

    sub = ((ServerPairBaseIndexEntry *)p1)->id1 -
        ((ServerPairBaseIndexEntry *)p2)->id1;
    if (sub != 0) {
        return sub;
    }

    return ((ServerPairBaseIndexEntry *)p1)->id2 -
        ((ServerPairBaseIndexEntry *)p2)->id2;
}

static int init_server_pair_index_array()
{
    ServerPairBaseIndexEntry *entry;
    FSClusterServerInfo *cs1;
    FSClusterServerInfo *cs2;
    FSClusterServerInfo *end;
    int count;
    int bytes;

    if (CLUSTER_SERVER_ARRAY.count <= 1) {
        return 0;
    }

    count = CLUSTER_SERVER_ARRAY.count * (CLUSTER_SERVER_ARRAY.count - 1) / 2;
    bytes = sizeof(ServerPairBaseIndexEntry) * count;
    server_pair_index_array.entries = (ServerPairBaseIndexEntry *)malloc(bytes);
    if (server_pair_index_array.entries == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }

    entry = server_pair_index_array.entries;
    end = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    for (cs1=CLUSTER_SERVER_ARRAY.servers; cs1<end; cs1++) {
        for (cs2=cs1+1; cs2<end; cs2++) {
            entry->id1 = cs1->server->id;
            entry->id2 = cs2->server->id;
            entry->offset = (entry - server_pair_index_array.entries) *
                REPLICA_CHANNELS_BETWEEN_TWO_SERVERS;
            entry++;
        }
    }

    logInfo("server count: %d, server_pair_index_array.count: %d, "
            "replica_channels_between_two_servers: %d",
            CLUSTER_SERVER_ARRAY.count, count, REPLICA_CHANNELS_BETWEEN_TWO_SERVERS);

    server_pair_index_array.count = count;
    return 0;
}

int fs_get_server_pair_base_offset(const int server_id1, const int server_id2)
{
    ServerPairBaseIndexEntry target;
    ServerPairBaseIndexEntry *found;

    target.id1 = FC_MIN(server_id1, server_id2);
    target.id2 = FC_MAX(server_id1, server_id2);
    if ((found=(ServerPairBaseIndexEntry *)bsearch(&target,
                    server_pair_index_array.entries,
                    server_pair_index_array.count,
                    sizeof(ServerPairBaseIndexEntry),
                    compare_server_pair_entry)) != NULL)
    {
        return found->offset;
    }
    return -1;
}

static int find_myself_in_cluster_config(const char *filename)
{
    FCServerInfo *server;
    int result;

    if ((server=get_myself_in_cluster_cfg(filename, &result)) == NULL) {
        return result;
    }

    CLUSTER_MYSELF_PTR = fs_get_server_by_id(server->id);
    if (CLUSTER_MYSELF_PTR == NULL) {
        logError("file: "__FILE__", line: %d, "
                "cluster config file: %s, can't find myself "
                "by my server id: %d", __LINE__, filename, server->id);
        return ENOENT;
    }
    return 0;
}

FSClusterServerInfo *fs_get_server_by_id(const int server_id)
{
    FSClusterServerInfo *cs;
    FSClusterServerInfo *end;

    end = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    for (cs=CLUSTER_SERVER_ARRAY.servers; cs<end; cs++) {
        if (cs->server->id == server_id) {
            return cs;
        }
    }

    return NULL;
}

static inline void get_server_group_filename(
        char *full_filename, const int size)
{
    snprintf(full_filename, size, "%s/%s",
            DATA_PATH_STR, DATA_GROUP_INFO_FILENAME);
}

static int load_group_servers_from_ini(const char *group_filename,
        IniContext *ini_context, FSClusterDataGroupInfo *group)
{
#define MAX_FIELD_COUNT 4
    FSClusterDataServerInfo *sp;
    FSClusterDataServerInfo *sp_end;
    IniItem *items;
    IniItem *it_end;
    IniItem *it;
    string_t fields[MAX_FIELD_COUNT];
    string_t value;
    int item_count;
    int field_count;
    int server_id;
    int status;
    int64_t last_data_version;
    char section_name[64];

    sprintf(section_name, "%s%d", DATA_GROUP_SECTION_PREFIX_STR,
            group->data_group_id);
    if ((items=iniGetValuesEx(section_name, SERVER_GROUP_INFO_ITEM_SERVER,
            ini_context, &item_count)) == NULL)
    {
        return 0;
    }

    sp_end = group->data_server_array.servers + group->data_server_array.count;
    it_end = items + item_count;
    for (it=items; it<it_end; it++) {
        FC_SET_STRING(value, it->value);
        field_count = split_string_ex(&value, ',', fields,
                MAX_FIELD_COUNT, false);
        if (field_count != 3) {
            logError("file: "__FILE__", line: %d, "
                    "group filename: %s, section: %s, item: %s, "
                    "invalid value: %s, field count: %d != 3",
                    __LINE__, group_filename, section_name,
                    SERVER_GROUP_INFO_ITEM_SERVER,
                    it->value, field_count);
            return EINVAL;
        }

        server_id = strtol(fields[0].str, NULL, 10);
        status = strtol(fields[1].str, NULL, 10);
        last_data_version = strtoll(fields[2].str, NULL, 10);
        if (status == FS_SERVER_STATUS_SYNCING ||
                status == FS_SERVER_STATUS_ACTIVE)
        {
            status = FS_SERVER_STATUS_OFFLINE;
        }
        for (sp=group->data_server_array.servers; sp<sp_end; sp++) {
            if (sp->cs->server->id == server_id) {
                sp->status = status;
                sp->last_data_version = last_data_version;
                break;
            }
        }
    }

    return 0;
}

static int get_server_group_info_file_mtime(time_t *mtime)
{
    char full_filename[PATH_MAX];
    struct stat buf;

    get_server_group_filename(full_filename, sizeof(full_filename));
    if (stat(full_filename, &buf) < 0) {
        logError("file: "__FILE__", line: %d, "
                "stat file \"%s\" fail, errno: %d, error info: %s",
                __LINE__, full_filename, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    *mtime = buf.st_mtime;
    return 0;
}

static int load_server_groups()
{
    FSClusterDataGroupInfo *group;
    FSClusterDataGroupInfo *end;
    char full_filename[PATH_MAX];
    IniContext ini_context;
    int result;

    get_server_group_filename(full_filename, sizeof(full_filename));
    if (access(full_filename, F_OK) != 0) {
        if (errno == ENOENT) {
            return server_group_info_write_to_file(CLUSTER_CURRENT_VERSION);
        }
    }

    if ((result=get_server_group_info_file_mtime(&last_shutdown_time)) != 0) {
        return result;
    }

    if ((result=iniLoadFromFile(full_filename, &ini_context)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "load from file \"%s\" fail, error code: %d",
                __LINE__, full_filename, result);
        return result;
    }

    CLUSTER_MYSELF_PTR->is_leader = iniGetBoolValue(NULL,
            SERVER_GROUP_INFO_ITEM_IS_LEADER, &ini_context, false);
    CLUSTER_CURRENT_VERSION = iniGetInt64Value(NULL,
            SERVER_GROUP_INFO_ITEM_VERSION, &ini_context, 0);
    logInfo("current version: %"PRId64, CLUSTER_CURRENT_VERSION);

    end = CLUSTER_DATA_RGOUP_ARRAY.groups + CLUSTER_DATA_RGOUP_ARRAY.count;
    for (group=CLUSTER_DATA_RGOUP_ARRAY.groups; group<end; group++) {
        if ((result=load_group_servers_from_ini(full_filename,
                        &ini_context, group)) != 0)
        {
            break;
        }
    }

    iniFreeContext(&ini_context);
    return result;
}

static FastBuffer file_buffer;

int server_group_info_init(const char *cluster_config_filename)
{
    int result;
    time_t t;
    struct tm tm_current;

    if ((result=fast_buffer_init_ex(&file_buffer, 2048)) != 0) {
        return result;
    }

    if ((result=init_cluster_server_array(cluster_config_filename)) != 0) {
        return result;
    }

    if ((result=find_myself_in_cluster_config(cluster_config_filename)) != 0) {
        return result;
    }

    if ((result=init_server_pair_index_array()) != 0) {
        return result;
    }

    if ((result=init_cluster_data_group_array(cluster_config_filename)) != 0) {
        return result;
    }

    if ((result=load_server_groups()) != 0) {
        return result;
    }

    t = g_current_time + 89;
    localtime_r(&t, &tm_current);
    tm_current.tm_sec = 0;
    last_refresh_file_time = mktime(&tm_current);

    return init_cluster_notify_contexts();
}

static int server_group_info_to_file_buffer(FSClusterDataGroupInfo *group)
{
    FSClusterDataServerInfo *sp;
    FSClusterDataServerInfo *end;
    int result;

    if ((result=fast_buffer_append(&file_buffer, "[%s%d]\n",
                    DATA_GROUP_SECTION_PREFIX_STR,
                    group->data_group_id)) != 0)
    {
        return result;
    }

    end = group->data_server_array.servers + group->data_server_array.count;
    for (sp=group->data_server_array.servers; sp<end; sp++) {
        if ((result=fast_buffer_append(&file_buffer, "%s=%d,%d,%"PRId64"\n",
                        SERVER_GROUP_INFO_ITEM_SERVER, sp->cs->server->id,
                        sp->status, sp->last_data_version)) != 0)
        {
            return result;
        }
    }

    return 0;
}

static int server_group_info_write_to_file(const uint64_t current_version)
{
    FSClusterDataGroupInfo *group;
    FSClusterDataGroupInfo *end;
    char full_filename[PATH_MAX];
    int result;

    fast_buffer_reset(&file_buffer);
    fast_buffer_append(&file_buffer,
            "%s=%d\n"
            "%s=%"PRId64"\n",
            SERVER_GROUP_INFO_ITEM_IS_LEADER,
            CLUSTER_MYSELF_PTR->is_leader,
            SERVER_GROUP_INFO_ITEM_VERSION, current_version);

    end = CLUSTER_DATA_RGOUP_ARRAY.groups + CLUSTER_DATA_RGOUP_ARRAY.count;
    for (group=CLUSTER_DATA_RGOUP_ARRAY.groups; group<end; group++) {
        if ((result=server_group_info_to_file_buffer(group)) != 0) {
            return result;
        }
    }

    get_server_group_filename(full_filename, sizeof(full_filename));
    if ((result=safeWriteToFile(full_filename, file_buffer.data,
                    file_buffer.length)) != 0)
    {
        logError("file: "__FILE__", line: %d, "
                "write to file \"%s\" fail, "
                "errno: %d, error info: %s",
                __LINE__, full_filename,
                result, STRERROR(result));
    }
    return result;
}

time_t fs_get_last_shutdown_time()
{
    return last_shutdown_time;
}

static int server_group_info_set_file_mtime()
{
    char full_filename[PATH_MAX];
    struct timeval times[2];

    times[0].tv_sec = g_current_time;
    times[0].tv_usec = 0;
    times[1].tv_sec = g_current_time;
    times[1].tv_usec = 0;

    get_server_group_filename(full_filename, sizeof(full_filename));
    if (utimes(full_filename, times) < 0) {
        logError("file: "__FILE__", line: %d, "
                "utimes file \"%s\" fail, errno: %d, error info: %s",
                __LINE__, full_filename, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    logInfo("=====file: "__FILE__", line: %d, "
            "utimes file: %s", __LINE__, full_filename);
    return 0;
}

static int server_group_info_sync_to_file(void *args)
{
    static uint64_t last_synced_version = 0;
    uint64_t current_version;
    int result;

    current_version = __sync_add_and_fetch(&CLUSTER_CURRENT_VERSION, 0);
    if (last_synced_version == current_version) {
        if (g_current_time - last_refresh_file_time > 60) {
            last_refresh_file_time = g_current_time;
            return server_group_info_set_file_mtime();
        }
        return 0;
    }

    if ((result=server_group_info_write_to_file(current_version)) == 0) {
        last_synced_version = current_version;
    }
    last_refresh_file_time = g_current_time;
    return result;
}

int server_group_info_setup_sync_to_file_task()
{
    ScheduleEntry schedule_entry;
    ScheduleArray schedule_array;

    INIT_SCHEDULE_ENTRY(schedule_entry, sched_generate_next_id(),
            0, 0, 0, 1, server_group_info_sync_to_file, NULL);

    schedule_array.count = 1;
    schedule_array.entries = &schedule_entry;
    return sched_add_entries(&schedule_array);
}
