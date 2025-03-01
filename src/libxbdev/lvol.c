/**
 * @file lvol.c
 * @brief LVOL逻辑卷管理实现
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/bdev.h>
#include <spdk/thread.h>
#include <spdk/blob.h>
#include <spdk/lvol.h>
#include <stdlib.h>
#include <string.h>

/**
 * LVOL上下文结构
 */
struct lvol_context {
    char lvs_name[256];              // 存储池名称
    char lvol_name[256];             // 逻辑卷名称
    uint64_t size_mb;                // 卷大小(MB)
    bool thin_provision;             // 是否精简配置
    int rc;                          // 返回值
    bool done;                       // 是否完成
};

/**
 * 创建LVOL存储池上下文
 */
struct lvol_create_pool_ctx {
    const char *bdev_name;
    const char *lvs_name;
    uint64_t cluster_size;
    bool done;
    int rc;
};

/**
 * 销毁LVOL存储池上下文
 */
struct lvol_destroy_pool_ctx {
    const char *lvs_name;
    bool done;
    int rc;
};

/**
 * 创建LVOL卷上下文
 */
struct lvol_create_ctx {
    const char *lvs_name;
    const char *lvol_name;
    uint64_t size_mb;
    bool thin_provision;
    bool done;
    int rc;
};

/**
 * 销毁LVOL卷上下文
 */
struct lvol_destroy_ctx {
    const char *lvol_name;
    bool done;
    int rc;
};

/**
 * 创建快照上下文
 */
struct lvol_snapshot_ctx {
    const char *lvol_name;
    const char *snap_name;
    bool done;
    int rc;
};

/**
 * 创建克隆上下文
 */
struct lvol_clone_ctx {
    const char *snap_name;
    const char *clone_name;
    bool done;
    int rc;
};

/**
 * 完成回调 - LVOL存储池创建/销毁
 */
static void lvs_op_complete(void *cb_arg, int lvserrno)
{
    struct {
        bool *done;
        int *rc;
    } *args = cb_arg;
    
    *args->rc = lvserrno;
    *args->done = true;
}

/**
 * 完成回调 - LVOL卷创建
 */
static void lvol_create_complete(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
    struct {
        bool *done;
        int *rc;
    } *args = cb_arg;
    
    *args->rc = lvolerrno;
    *args->done = true;
}

/**
 * 完成回调 - LVOL卷操作
 */
static void lvol_op_complete(void *cb_arg, int lvolerrno)
{
    struct {
        bool *done;
        int *rc;
    } *args = cb_arg;
    
    *args->rc = lvolerrno;
    *args->done = true;
}

/**
 * 创建LVOL存储池
 */
void lvol_create_pool_on_thread(void *arg)
{
    struct lvol_create_pool_ctx *ctx = arg;
    struct spdk_bdev *bdev = NULL;
    int rc;
    
    // 查找基础设备
    bdev = spdk_bdev_get_by_name(ctx->bdev_name);
    if (bdev == NULL) {
        XBDEV_ERRLOG("找不到设备: %s\n", ctx->bdev_name);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 创建LVOL存储池
    rc = spdk_lvs_create(bdev, ctx->cluster_size, ctx->lvs_name, 
                       lvs_op_complete, &cb_arg);
    if (rc != 0) {
        XBDEV_ERRLOG("无法创建LVOL存储池: %s, rc=%d\n", ctx->lvs_name, rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 销毁LVOL存储池
 */
void lvol_destroy_pool_on_thread(void *arg)
{
    struct lvol_destroy_pool_ctx *ctx = arg;
    struct spdk_lvol_store *lvs = NULL;
    int rc;
    
    // 查找存储池
    lvs = spdk_lvol_store_find_by_name(ctx->lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", ctx->lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 销毁LVOL存储池
    rc = spdk_lvs_destroy(lvs, lvs_op_complete, &cb_arg);
    if (rc != 0) {
        XBDEV_ERRLOG("无法销毁LVOL存储池: %s, rc=%d\n", ctx->lvs_name, rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 创建LVOL卷
 */
void lvol_create_on_thread(void *arg)
{
    struct lvol_create_ctx *ctx = arg;
    struct spdk_lvol_store *lvs = NULL;
    int rc;
    
    // 查找存储池
    lvs = spdk_lvol_store_find_by_name(ctx->lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", ctx->lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 计算卷大小(MB转字节)
    uint64_t size_bytes = ctx->size_mb * 1024 * 1024;
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 创建LVOL卷
    rc = spdk_lvol_create(lvs, ctx->lvol_name, size_bytes, 
                        ctx->thin_provision ? SPDK_LVOL_THIN_PROVISION : SPDK_LVOL_DEFAULT,
                        lvol_create_complete, &cb_arg);
    if (rc != 0) {
        XBDEV_ERRLOG("无法创建LVOL卷: %s/%s, rc=%d\n", ctx->lvs_name, ctx->lvol_name, rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 销毁LVOL卷
 */
void lvol_destroy_on_thread(void *arg)
{
    struct lvol_destroy_ctx *ctx = arg;
    struct spdk_lvol *lvol = NULL;
    int rc;
    
    // 解析卷名称
    char lvs_name[256];
    char lvol_name[256];
    if (_xbdev_parse_lvol_name(ctx->lvol_name, lvs_name, sizeof(lvs_name), 
                             lvol_name, sizeof(lvol_name)) != 0) {
        XBDEV_ERRLOG("无效的LVOL卷名: %s\n", ctx->lvol_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 查找存储池
    struct spdk_lvol_store *lvs = spdk_lvol_store_find_by_name(lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 查找卷
    lvol = spdk_lvol_find_by_name(lvs, lvol_name);
    if (lvol == NULL) {
        XBDEV_ERRLOG("找不到LVOL卷: %s\n", ctx->lvol_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 销毁LVOL卷
    spdk_lvol_destroy(lvol, lvol_op_complete, &cb_arg);
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 创建快照
 */
void lvol_create_snapshot_on_thread(void *arg)
{
    struct lvol_snapshot_ctx *ctx = arg;
    struct spdk_lvol *lvol = NULL;
    int rc;
    
    // 解析卷名称
    char lvs_name[256];
    char lvol_name[256];
    if (_xbdev_parse_lvol_name(ctx->lvol_name, lvs_name, sizeof(lvs_name), 
                             lvol_name, sizeof(lvol_name)) != 0) {
        XBDEV_ERRLOG("无效的LVOL卷名: %s\n", ctx->lvol_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 查找存储池
    struct spdk_lvol_store *lvs = spdk_lvol_store_find_by_name(lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 查找卷
    lvol = spdk_lvol_find_by_name(lvs, lvol_name);
    if (lvol == NULL) {
        XBDEV_ERRLOG("找不到LVOL卷: %s\n", ctx->lvol_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 创建快照
    spdk_lvol_create_snapshot(lvol, ctx->snap_name, lvol_create_complete, &cb_arg);
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 创建克隆
 */
void lvol_create_clone_on_thread(void *arg)
{
    struct lvol_clone_ctx *ctx = arg;
    struct spdk_lvol *snap = NULL;
    int rc;
    
    // 解析快照名称
    char lvs_name[256];
    char snap_name[256];
    if (_xbdev_parse_lvol_name(ctx->snap_name, lvs_name, sizeof(lvs_name), 
                             snap_name, sizeof(snap_name)) != 0) {
        XBDEV_ERRLOG("无效的快照名: %s\n", ctx->snap_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 查找存储池
    struct spdk_lvol_store *lvs = spdk_lvol_store_find_by_name(lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 查找快照
    snap = spdk_lvol_find_by_name(lvs, snap_name);
    if (snap == NULL) {
        XBDEV_ERRLOG("找不到快照: %s\n", ctx->snap_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 创建克隆
    spdk_lvol_create_clone(snap, ctx->clone_name, lvol_create_complete, &cb_arg);
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 重命名LVOL卷
 */
void lvol_rename_on_thread(void *arg)
{
    struct {
        const char *old_name;
        const char *new_name;
        bool done;
        int rc;
    } *ctx = arg;
    
    // 解析旧卷名称
    char lvs_name[256];
    char lvol_name[256];
    if (_xbdev_parse_lvol_name(ctx->old_name, lvs_name, sizeof(lvs_name), 
                             lvol_name, sizeof(lvol_name)) != 0) {
        XBDEV_ERRLOG("无效的LVOL卷名: %s\n", ctx->old_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 查找存储池
    struct spdk_lvol_store *lvs = spdk_lvol_store_find_by_name(lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 查找卷
    struct spdk_lvol *lvol = spdk_lvol_find_by_name(lvs, lvol_name);
    if (lvol == NULL) {
        XBDEV_ERRLOG("找不到LVOL卷: %s\n", ctx->old_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 解析新名称，获取卷名部分
    char new_lvs_name[256];
    char new_lvol_name[256];
    if (_xbdev_parse_lvol_name(ctx->new_name, new_lvs_name, sizeof(new_lvs_name), 
                             new_lvol_name, sizeof(new_lvol_name)) != 0) {
        XBDEV_ERRLOG("无效的新LVOL卷名: %s\n", ctx->new_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 检查存储池是否相同
    if (strcmp(lvs_name, new_lvs_name) != 0) {
        XBDEV_ERRLOG("不支持跨存储池重命名: %s -> %s\n", ctx->old_name, ctx->new_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 重命名LVOL卷
    spdk_lvol_rename(lvol, new_lvol_name, lvol_op_complete, &cb_arg);
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 调整LVOL卷大小
 */
void lvol_resize_on_thread(void *arg)
{
    struct {
        const char *lvol_name;
        uint64_t new_size_mb;
        bool done;
        int rc;
    } *ctx = arg;
    
    // 解析卷名称
    char lvs_name[256];
    char lvol_name[256];
    if (_xbdev_parse_lvol_name(ctx->lvol_name, lvs_name, sizeof(lvs_name), 
                             lvol_name, sizeof(lvol_name)) != 0) {
        XBDEV_ERRLOG("无效的LVOL卷名: %s\n", ctx->lvol_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 查找存储池
    struct spdk_lvol_store *lvs = spdk_lvol_store_find_by_name(lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 查找卷
    struct spdk_lvol *lvol = spdk_lvol_find_by_name(lvs, lvol_name);
    if (lvol == NULL) {
        XBDEV_ERRLOG("找不到LVOL卷: %s\n", ctx->lvol_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 计算新大小(MB转字节)
    uint64_t new_size_bytes = ctx->new_size_mb * 1024 * 1024;
    
    // 构造回调参数
    struct {
        bool *done;
        int *rc;
    } cb_arg = {
        .done = &ctx->done,
        .rc = &ctx->rc
    };
    
    // 调整LVOL卷大小
    spdk_lvol_resize(lvol, new_size_bytes, lvol_op_complete, &cb_arg);
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 获取LVOL卷信息
 */
void lvol_get_info_on_thread(void *arg)
{
    struct {
        const char *lvol_name;
        xbdev_lvol_info_t *info;
        bool done;
        int rc;
    } *ctx = arg;
    
    // 解析卷名称
    char lvs_name[256];
    char lvol_name[256];
    if (_xbdev_parse_lvol_name(ctx->lvol_name, lvs_name, sizeof(lvs_name), 
                             lvol_name, sizeof(lvol_name)) != 0) {
        XBDEV_ERRLOG("无效的LVOL卷名: %s\n", ctx->lvol_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 查找存储池
    struct spdk_lvol_store *lvs = spdk_lvol_store_find_by_name(lvs_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", lvs_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 查找卷
    struct spdk_lvol *lvol = spdk_lvol_find_by_name(lvs, lvol_name);
    if (lvol == NULL) {
        XBDEV_ERRLOG("找不到LVOL卷: %s\n", ctx->lvol_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 获取卷信息
    strncpy(ctx->info->lvs_name, lvs_name, sizeof(ctx->info->lvs_name) - 1);
    strncpy(ctx->info->lvol_name, lvol_name, sizeof(ctx->info->lvol_name) - 1);
    
    ctx->info->size = spdk_lvol_get_size(lvol);
    ctx->info->thin_provision = (spdk_lvol_get_type(lvol) == SPDK_LVOL_TYPE_THIN);
    ctx->info->snapshot = (spdk_lvol_get_type(lvol) == SPDK_LVOL_TYPE_SNAPSHOT);
    ctx->info->clone = (spdk_lvol_get_type(lvol) == SPDK_LVOL_TYPE_CLONE);
    ctx->info->cluster_size = spdk_lvs_get_cluster_size(lvs);
    
    // 获取UUID
    const struct spdk_uuid *lvol_uuid = spdk_lvol_get_uuid(lvol);
    spdk_uuid_fmt_lower(ctx->info->uuid, sizeof(ctx->info->uuid), lvol_uuid);
    
    // 后期可添加已分配大小等更多信息
    // ctx->info->allocated_size = ...
    
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 获取LVOL存储池信息
 */
void lvol_get_pool_info_on_thread(void *arg)
{
    struct {
        const char *pool_name;
        xbdev_lvol_pool_info_t *info;
        bool done;
        int rc;
    } *ctx = arg;
    
    // 查找存储池
    struct spdk_lvol_store *lvs = spdk_lvol_store_find_by_name(ctx->pool_name);
    if (lvs == NULL) {
        XBDEV_ERRLOG("找不到LVOL存储池: %s\n", ctx->pool_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 获取基础块设备
    struct spdk_bdev *bdev = spdk_lvol_get_base_bdev(lvs);
    if (bdev == NULL) {
        XBDEV_ERRLOG("无法获取LVOL存储池的基础设备: %s\n", ctx->pool_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 填充存储池信息
    strncpy(ctx->info->pool_name, ctx->pool_name, sizeof(ctx->info->pool_name) - 1);
    strncpy(ctx->info->bdev_name, spdk_bdev_get_name(bdev), sizeof(ctx->info->bdev_name) - 1);
    
    ctx->info->cluster_size = spdk_lvs_get_cluster_size(lvs);
    ctx->info->total_clusters = spdk_lvs_get_total_clusters(lvs);
    ctx->info->free_clusters = spdk_lvs_get_free_clusters(lvs);
    
    // 获取存储池UUID
    const struct spdk_uuid *lvs_uuid = spdk_lvs_get_uuid(lvs);
    spdk_uuid_fmt_lower(ctx->info->uuid, sizeof(ctx->info->uuid), lvs_uuid);
    
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 解析LVOL名称，分离存储池和卷名称
 *
 * @param full_name 完整名称，格式如 "lvs_name/lvol_name"
 * @param lvs_name 输出参数，存储池名称
 * @param lvs_name_size 存储池名称缓冲区大小
 * @param lvol_name 输出参数，卷名称
 * @param lvol_name_size 卷名称缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_parse_lvol_name(const char *full_name, char *lvs_name, 
                          size_t lvs_name_size, char *lvol_name, 
                          size_t lvol_name_size)
{
    // 检查参数有效性
    if (!full_name || !lvs_name || !lvol_name || 
        lvs_name_size == 0 || lvol_name_size == 0) {
        return -EINVAL;
    }
    
    // 查找分隔符
    const char *separator = strchr(full_name, '/');
    if (!separator) {
        XBDEV_ERRLOG("无效的LVOL名称格式: %s (应为 lvs_name/lvol_name)\n", full_name);
        return -EINVAL;
    }
    
    // 计算长度
    size_t lvs_length = separator - full_name;
    size_t lvol_length = strlen(separator + 1);
    
    // 检查长度是否超过缓冲区
    if (lvs_length >= lvs_name_size || lvol_length >= lvol_name_size) {
        XBDEV_ERRLOG("LVOL名称太长: %s\n", full_name);
        return -ENAMETOOLONG;
    }
    
    // 分别复制存储池名称和卷名称
    memcpy(lvs_name, full_name, lvs_length);
    lvs_name[lvs_length] = '\0';
    
    memcpy(lvol_name, separator + 1, lvol_length);
    lvol_name[lvol_length] = '\0';
    
    return 0;
}

/**
 * 创建LVOL存储池
 * 
 * @param bdev_name 基础设备名称
 * @param pool_name 池名称
 * @param cluster_size 簇大小，0表示使用默认值
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create_pool(const char *bdev_name, const char *pool_name, uint64_t cluster_size)
{
    int rc;
    xbdev_request_t *req;
    struct lvol_create_pool_ctx ctx = {0};
    
    if (!bdev_name || !pool_name) {
        XBDEV_ERRLOG("无效的参数: bdev_name=%p, pool_name=%p\n", 
                   bdev_name, pool_name);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.bdev_name = bdev_name;
    ctx.lvs_name = pool_name;
    ctx.cluster_size = cluster_size;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_CREATE_POOL;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("创建LVOL存储池失败: %s (在设备 %s 上), rc=%d\n", 
                   pool_name, bdev_name, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功创建LVOL存储池: %s (在设备 %s 上)\n", 
                  pool_name, bdev_name);
    
    return 0;
}

/**
 * 销毁LVOL存储池
 * 
 * @param pool_name 池名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_destroy_pool(const char *pool_name)
{
    int rc;
    xbdev_request_t *req;
    struct lvol_destroy_pool_ctx ctx = {0};
    
    if (!pool_name) {
        XBDEV_ERRLOG("无效的存储池名称\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvs_name = pool_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_DESTROY_POOL;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("销毁LVOL存储池失败: %s, rc=%d\n", pool_name, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功销毁LVOL存储池: %s\n", pool_name);
    
    return 0;
}

/**
 * 创建LVOL卷
 * 
 * @param pool_name 池名称
 * @param lvol_name 卷名称
 * @param size_mb 卷大小(MB)
 * @param thin_provision 是否启用精简配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create(const char *pool_name, const char *lvol_name, uint64_t size_mb, bool thin_provision)
{
    int rc;
    xbdev_request_t *req;
    struct lvol_create_ctx ctx = {0};
    
    if (!pool_name || !lvol_name || size_mb == 0) {
        XBDEV_ERRLOG("无效的参数: pool_name=%p, lvol_name=%p, size_mb=%lu\n", 
                   pool_name, lvol_name, size_mb);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvs_name = pool_name;
    ctx.lvol_name = lvol_name;
    ctx.size_mb = size_mb;
    ctx.thin_provision = thin_provision;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_CREATE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("创建LVOL卷失败: %s/%s, rc=%d\n", 
                   pool_name, lvol_name, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功创建LVOL卷: %s/%s (大小: %lu MB, 精简配置: %s)\n", 
                  pool_name, lvol_name, size_mb, thin_provision ? "是" : "否");
    
    return 0;
}

/**
 * 销毁LVOL卷
 * 
 * @param lvol_name 卷名称，格式为 "lvs_name/lvol_name"
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_destroy(const char *lvol_name)
{
    int rc;
    xbdev_request_t *req;
    struct lvol_destroy_ctx ctx = {0};
    
    if (!lvol_name) {
        XBDEV_ERRLOG("无效的卷名称\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvol_name = lvol_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_DESTROY;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("销毁LVOL卷失败: %s, rc=%d\n", lvol_name, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功销毁LVOL卷: %s\n", lvol_name);
    
    return 0;
}

/**
 * 创建快照
 * 
 * @param lvol_name 源卷名称，格式为 "lvs_name/lvol_name"
 * @param snapshot_name 快照名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create_snapshot(const char *lvol_name, const char *snapshot_name)
{
    int rc;
    xbdev_request_t *req;
    struct lvol_snapshot_ctx ctx = {0};
    
    if (!lvol_name || !snapshot_name) {
        XBDEV_ERRLOG("无效的参数: lvol_name=%p, snapshot_name=%p\n", 
                   lvol_name, snapshot_name);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvol_name = lvol_name;
    ctx.snap_name = snapshot_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_SNAPSHOT;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("创建快照失败: %s -> %s, rc=%d\n", 
                   lvol_name, snapshot_name, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功创建快照: %s -> %s\n", lvol_name, snapshot_name);
    
    return 0;
}

/**
 * 创建克隆卷
 * 
 * @param snapshot_name 快照名称，格式为 "lvs_name/snap_name"
 * @param clone_name 克隆卷名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create_clone(const char *snapshot_name, const char *clone_name)
{
    int rc;
    xbdev_request_t *req;
    struct lvol_clone_ctx ctx = {0};
    
    if (!snapshot_name || !clone_name) {
        XBDEV_ERRLOG("无效的参数: snapshot_name=%p, clone_name=%p\n", 
                   snapshot_name, clone_name);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.snap_name = snapshot_name;
    ctx.clone_name = clone_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_CLONE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("创建克隆卷失败: %s -> %s, rc=%d\n", 
                   snapshot_name, clone_name, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功创建克隆卷: %s -> %s\n", snapshot_name, clone_name);
    
    return 0;
}

/**
 * 调整LVOL卷大小
 * 
 * @param lvol_name 卷名称，格式为 "lvs_name/lvol_name"
 * @param new_size_mb 新大小(MB)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_resize(const char *lvol_name, uint64_t new_size_mb)
{
    int rc;
    xbdev_request_t *req;
    struct {
        const char *lvol_name;
        uint64_t new_size_mb;
        bool done;
        int rc;
    } ctx = {0};
    
    if (!lvol_name || new_size_mb == 0) {
        XBDEV_ERRLOG("无效的参数: lvol_name=%p, new_size_mb=%lu\n", 
                   lvol_name, new_size_mb);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvol_name = lvol_name;
    ctx.new_size_mb = new_size_mb;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_RESIZE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("调整LVOL卷大小失败: %s, 新大小=%lu MB, rc=%d\n", 
                   lvol_name, new_size_mb, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功调整LVOL卷大小: %s, 新大小=%lu MB\n", 
                  lvol_name, new_size_mb);
    
    return 0;
}

/**
 * 获取LVOL卷信息
 * 
 * @param lvol_name 卷名称，格式为 "lvs_name/lvol_name"
 * @param info 输出参数，卷信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_get_info(const char *lvol_name, xbdev_lvol_info_t *info)
{
    int rc;
    xbdev_request_t *req;
    struct {
        const char *lvol_name;
        xbdev_lvol_info_t *info;
        bool done;
        int rc;
    } ctx = {0};
    
    if (!lvol_name || !info) {
        XBDEV_ERRLOG("无效的参数: lvol_name=%p, info=%p\n", lvol_name, info);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvol_name = lvol_name;
    ctx.info = info;
    ctx.done = false;
    ctx.rc = 0;
    
    // 初始化info结构体
    memset(info, 0, sizeof(*info));
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_GET_INFO;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    return ctx.rc;
}

/**
 * 获取LVOL存储池信息
 * 
 * @param pool_name 池名称
 * @param info 输出参数，池信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_get_pool_info(const char *pool_name, xbdev_lvol_pool_info_t *info)
{
    int rc;
    xbdev_request_t *req;
    struct {
        const char *pool_name;
        xbdev_lvol_pool_info_t *info;
        bool done;
        int rc;
    } ctx = {0};
    
    if (!pool_name || !info) {
        XBDEV_ERRLOG("无效的参数: pool_name=%p, info=%p\n", pool_name, info);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.pool_name = pool_name;
    ctx.info = info;
    ctx.done = false;
    ctx.rc = 0;
    
    // 初始化info结构体
    memset(info, 0, sizeof(*info));
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_GET_POOL_INFO;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    return ctx.rc;
}

/**
 * 重命名LVOL卷
 * 
 * @param old_name 旧名称，格式为 "lvs_name/old_lvol_name"
 * @param new_name 新名称，格式为 "lvs_name/new_lvol_name"
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_rename(const char *old_name, const char *new_name)
{
    int rc;
    xbdev_request_t *req;
    struct {
        const char *old_name;
        const char *new_name;
        bool done;
        int rc;
    } ctx = {0};
    
    if (!old_name || !new_name) {
        XBDEV_ERRLOG("无效的参数: old_name=%p, new_name=%p\n", old_name, new_name);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.old_name = old_name;
    ctx.new_name = new_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_LVOL_RENAME;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        XBDEV_ERRLOG("重命名LVOL卷失败: %s -> %s, rc=%d\n", 
                   old_name, new_name, ctx.rc);
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功重命名LVOL卷: %s -> %s\n", old_name, new_name);
    
    return 0;
}
