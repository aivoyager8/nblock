/**
 * @file lvol.c
 * @brief Implementation of LVOL functionality
 *
 * This file implements the Logical Volume (LVOL) functionality,
 * including LVOL store creation and management, volume operations,
 * snapshot and clone support.
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/bdev.h>
#include <spdk/lvol.h>
#include <spdk/env.h>
#include <spdk/thread.h>
#include <spdk/string.h>
#include <spdk/log.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * LVS creation context
 */
struct lvs_create_ctx {
    const char *bdev_name;          // Base bdev name
    const char *lvs_name;           // LVOL store name
    uint64_t cluster_size;          // Cluster size (0 for default)
    struct spdk_lvol_store *lvs;    // Resulting LVS handle
    bool done;                      // Operation complete flag
    int rc;                         // Return code
};

/**
 * LVOL creation context
 */
struct lvol_create_ctx {
    const char *lvs_name;           // LVOL store name
    const char *lvol_name;          // LVOL name
    uint64_t size_mb;               // Size in MB
    bool thin_provision;            // Whether to thin provision
    struct spdk_lvol *lvol;         // Resulting LVOL handle
    bool done;                      // Operation complete flag
    int rc;                         // Return code
};

/**
 * Snapshot creation context
 */
struct snapshot_create_ctx {
    const char *lvol_name;          // Source LVOL name
    const char *snapshot_name;      // Snapshot name
    struct spdk_lvol *snap;         // Resulting snapshot handle
    bool done;                      // Operation complete flag
    int rc;                         // Return code
};

/**
 * Clone creation context
 */
struct clone_create_ctx {
    const char *snapshot_name;      // Source snapshot name
    const char *clone_name;         // Clone name
    struct spdk_lvol *clone;        // Resulting clone handle
    bool done;                      // Operation complete flag
    int rc;                         // Return code
};

/**
 * Create LVOL store (LVS)
 *
 * @param bdev_name Base block device name
 * @param lvs_name LVOL store name
 * @param cluster_size Cluster size in bytes (0 for default)
 * @return 0 on success, negative error code on failure
 */
int xbdev_lvs_create(const char *bdev_name, const char *lvs_name, uint64_t cluster_size)
{
    xbdev_request_t *req;
    struct lvs_create_ctx ctx = {0};
    int rc;
    
    // Validate parameters
    if (!bdev_name || !lvs_name) {
        XBDEV_ERRLOG("Invalid parameters for LVS creation\n");
        return -EINVAL;
    }
    
    // Setup context
    ctx.bdev_name = bdev_name;
    ctx.lvs_name = lvs_name;
    ctx.cluster_size = cluster_size;
    ctx.done = false;
    ctx.rc = 0;
    
    // Allocate request
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // Set request
    req->type = XBDEV_REQ_LVS_CREATE;
    req->ctx = &ctx;
    
    // Execute request and wait for completion
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Check operation result
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to create LVS: %s on %s, rc=%d\n", 
                   lvs_name, bdev_name, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Free resources
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("Successfully created LVS: %s on %s\n", 
                  lvs_name, bdev_name);
    
    return 0;
}

/**
 * LVS creation callback
 */
static void _lvs_create_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
    struct lvs_create_ctx *ctx = cb_arg;
    
    ctx->rc = lvserrno;
    ctx->lvs = lvs;
    ctx->done = true;
}

/**
 * Create LVS in SPDK thread context
 */
void xbdev_lvs_create_on_thread(void *ctx)
{
    struct lvs_create_ctx *args = ctx;
    struct spdk_bdev *bdev;
    
    // Get base bdev
    bdev = spdk_bdev_get_by_name(args->bdev_name);
    if (!bdev) {
        XBDEV_ERRLOG("Base bdev not found: %s\n", args->bdev_name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // Create LVS
    spdk_lvs_opts opts;
    spdk_lvs_opts_init(&opts);
    
    opts.cluster_sz = args->cluster_size;
    snprintf(opts.name, sizeof(opts.name), "%s", args->lvs_name);
    
    spdk_lvs_init(bdev, &opts, _lvs_create_cb, args);
    
    // Wait for operation to complete
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * Find LVS callback
 */
static void _lvs_find_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
    struct lvol_create_ctx *ctx = cb_arg;
    
    if (lvserrno != 0) {
        XBDEV_ERRLOG("Failed to find LVS %s: %d\n", ctx->lvs_name, lvserrno);
        ctx->rc = lvserrno;
        ctx->done = true;
        return;
    }
    
    // LVS found, create LVOL
    uint64_t size_bytes = ctx->size_mb * 1024 * 1024;
    spdk_lvol_create(lvs, ctx->lvol_name, size_bytes, ctx->thin_provision,
                    SPDK_LVOL_CLEAR_WITH_DEFAULT, _lvol_create_cb, ctx);
}

/**
 * Create LVOL callback
 */
static void _lvol_create_cb(void *cb_arg, struct spdk_lvol *lvol, int lvolerrno)
{
    struct lvol_create_ctx *ctx = cb_arg;
    
    if (lvolerrno != 0) {
        XBDEV_ERRLOG("Failed to create LVOL %s: %d\n", ctx->lvol_name, lvolerrno);
        ctx->rc = lvolerrno;
        ctx->done = true;
        return;
    }
    
    ctx->lvol = lvol;
    ctx->rc = 0;
    ctx->done = true;
    
    XBDEV_NOTICELOG("Successfully created LVOL %s/%s of size %"PRIu64"MB\n",
                  ctx->lvs_name, ctx->lvol_name, ctx->size_mb);
}

/**
 * Create LVOL
 */
int xbdev_lvol_create(const char *lvs_name, const char *lvol_name, uint64_t size_mb, bool thin_provision) {
    xbdev_request_t *req;
    struct lvol_create_ctx ctx;
    int rc;
    
    if (!lvs_name || !lvol_name || size_mb == 0) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvs_name = lvs_name;
    ctx.lvol_name = lvol_name;
    ctx.size_mb = size_mb;
    ctx.thin_provision = thin_provision;
    ctx.rc = 0;
    ctx.done = false;
    ctx.lvs = NULL;
    ctx.lvol = NULL;
    
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
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求资源
    xbdev_sync_request_free(req);
    
    // 返回创建结果
    return ctx.rc;
}

/**
 * SPDK线程上下文中执行LVOL创建
 */
void xbdev_lvol_create_on_thread(void *ctx) {
    struct lvol_create_ctx *args = ctx;
    
    // 查找LVS
    spdk_lvs_open_by_name(args->lvs_name, _lvs_find_cb, args);
    
    // 等待操作完成
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * Create snapshot callback
 */
static void _snapshot_create_cb(void *cb_arg, struct spdk_lvol *snap, int lvolerrno)
{
    struct snapshot_create_ctx *ctx = cb_arg;
    
    if (lvolerrno != 0) {
        XBDEV_ERRLOG("Failed to create snapshot %s: %d\n", ctx->snapshot_name, lvolerrno);
        ctx->rc = lvolerrno;
        ctx->done = true;
        return;
    }
    
    ctx->snap = snap;
    ctx->rc = 0;
    ctx->done = true;
    
    XBDEV_NOTICELOG("Successfully created snapshot %s from LVOL %s\n",
                  ctx->snapshot_name, ctx->lvol_name);
}

/**
 * 创建LVOL卷快照
 */
int xbdev_snapshot_create(const char *lvol_name, const char *snapshot_name) {
    xbdev_request_t *req;
    struct snap_create_ctx ctx;
    int rc;
    
    if (!lvol_name || !snapshot_name) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.lvol_name = lvol_name;
    ctx.snapshot_name = snapshot_name;
    ctx.rc = 0;
    ctx.done = false;
    ctx.lvol = NULL;
    ctx.snap = NULL;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_SNAPSHOT_CREATE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求资源
    xbdev_sync_request_free(req);
    
    // 返回创建结果
    return ctx.rc;
}

/**
 * SPDK线程上下文中执行快照创建
 */
void xbdev_snapshot_create_on_thread(void *ctx) {
    struct snap_create_ctx *args = ctx;
    struct spdk_bdev *bdev;
    struct spdk_lvol *lvol = NULL;
    
    // 构造bdev名称 (lvs_name/lvol_name)
    bdev = spdk_bdev_get_by_name(args->lvol_name);
    if (bdev == NULL) {
        XBDEV_ERRLOG("找不到LVOL设备: %s\n", args->lvol_name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // 获取LVOL对象
    lvol = spdk_lvol_get_from_bdev(bdev);
    if (lvol == NULL) {
        XBDEV_ERRLOG("无法获取LVOL对象: %s\n", args->lvol_name);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 创建快照
    spdk_lvol_create_snapshot(lvol, args->snapshot_name, _snapshot_create_cb, args);
    
    // 等待操作完成
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * Destroy LVS
 *
 * @param lvs_name LVOL store name
 * @return 0 on success, negative error code on failure
 */
int xbdev_lvs_destroy(const char *lvs_name)
{
    xbdev_request_t *req;
    struct {
        const char *lvs_name;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    // Validate parameters
    if (!lvs_name) {
        XBDEV_ERRLOG("Invalid LVS name\n");
        return -EINVAL;
    }
    
    // Setup context
    ctx.lvs_name = lvs_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // Allocate request
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // Set request
    req->type = XBDEV_REQ_LVS_DESTROY;
    req->ctx = &ctx;
    
    // Execute request and wait for completion
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Check operation result
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to destroy LVS: %s, rc=%d\n", lvs_name, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Free resources
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("Successfully destroyed LVS: %s\n", lvs_name);
    
    return 0;
}

/**
 * LVS destroy callback
 */
static void _lvs_destroy_cb(void *cb_arg, int lvserrno)
{
    struct {
        const char *lvs_name;
        bool done;
        int rc;
    } *ctx = cb_arg;
    
    ctx->rc = lvserrno;
    ctx->done = true;
    
    if (lvserrno != 0) {
        XBDEV_ERRLOG("Failed to destroy LVS %s: %d\n", ctx->lvs_name, lvserrno);
        return;
    }
    
    XBDEV_NOTICELOG("Successfully destroyed LVS %s\n", ctx->lvs_name);
}

/**
 * Destroy LVS in SPDK thread context
 */
void xbdev_lvs_destroy_on_thread(void *ctx)
{
    struct {
        const char *lvs_name;
        bool done;
        int rc;
    } *args = ctx;
    
    // Find LVS
    spdk_lvs_open_by_name(args->lvs_name, _lvs_destroy_find_cb, args);
    
    // Wait for operation to complete
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * Find LVS for destroy callback
 */
static void _lvs_destroy_find_cb(void *cb_arg, struct spdk_lvol_store *lvs, int lvserrno)
{
    struct {
        const char *lvs_name;
        bool done;
        int rc;
    } *ctx = cb_arg;
    
    if (lvserrno != 0) {
        XBDEV_ERRLOG("Failed to find LVS %s: %d\n", ctx->lvs_name, lvserrno);
        ctx->rc = lvserrno;
        ctx->done = true;
        return;
    }
    
    // Destroy LVS
    spdk_lvs_destroy(lvs, _lvs_destroy_cb, ctx);
}

/**
 * Destroy LVOL
 *
 * @param lvol_name LVOL name (format: lvs_name/lvol_name)
 * @return 0 on success, negative error code on failure
 */
int xbdev_lvol_destroy(const char *lvol_name)
{
    xbdev_request_t *req;
    struct {
        const char *lvol_name;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    // Validate parameters
    if (!lvol_name) {
        XBDEV_ERRLOG("Invalid LVOL name\n");
        return -EINVAL;
    }
    
    // Setup context
    ctx.lvol_name = lvol_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // Allocate request
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // Set request
    req->type = XBDEV_REQ_LVOL_DESTROY;
    req->ctx = &ctx;
    
    // Execute request and wait for completion
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Check operation result
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to destroy LVOL: %s, rc=%d\n", lvol_name, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Free resources
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("Successfully destroyed LVOL: %s\n", lvol_name);
    
    return 0;
}

/**
 * LVOL destroy callback
 */
static void _lvol_destroy_cb(void *cb_arg, int lvolerrno)
{
    struct {
        const char *lvol_name;
        bool done;
        int rc;
    } *ctx = cb_arg;
    
    ctx->rc = lvolerrno;
    ctx->done = true;
    
    if (lvolerrno != 0) {
        XBDEV_ERRLOG("Failed to destroy LVOL %s: %d\n", ctx->lvol_name, lvolerrno);
        return;
    }
    
    XBDEV_NOTICELOG("Successfully destroyed LVOL %s\n", ctx->lvol_name);
}

/**
 * Destroy LVOL in SPDK thread context
 */
void xbdev_lvol_destroy_on_thread(void *ctx)
{
    struct {
        const char *lvol_name;
        bool done;
        int rc;
    } *args = ctx;
    struct spdk_bdev *bdev;
    struct spdk_lvol *lvol;
    
    // Find LVOL bdev
    bdev = spdk_bdev_get_by_name(args->lvol_name);
    if (!bdev) {
        XBDEV_ERRLOG("LVOL bdev not found: %s\n", args->lvol_name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // Get LVOL from bdev
    lvol = spdk_lvol_get_from_bdev(bdev);
    if (!lvol) {
        XBDEV_ERRLOG("Could not get LVOL from bdev: %s\n", args->lvol_name);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // Destroy LVOL
    spdk_lvol_destroy(lvol, _lvol_destroy_cb, args);
    
    // Wait for operation to complete
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * Resize LVOL
 *
 * @param lvol_name LVOL name (format: lvs_name/lvol_name)
 * @param new_size_mb New size in MB
 * @return 0 on success, negative error code on failure
 */
int xbdev_lvol_resize(const char *lvol_name, uint64_t new_size_mb)
{
    xbdev_request_t *req;
    struct {
        const char *lvol_name;
        uint64_t new_size_mb;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    // Validate parameters
    if (!lvol_name || new_size_mb == 0) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // Setup context
    ctx.lvol_name = lvol_name;
    ctx.new_size_mb = new_size_mb;
    ctx.done = false;
    ctx.rc = 0;
    
    // Allocate request
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // Set request
    req->type = XBDEV_REQ_LVOL_RESIZE;
    req->ctx = &ctx;
    
    // Execute request and wait for completion
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Check operation result
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to resize LVOL: %s to %"PRIu64"MB, rc=%d\n", 
                   lvol_name, new_size_mb, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Free resources
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("Successfully resized LVOL: %s to %"PRIu64"MB\n", 
                  lvol_name, new_size_mb);
    
    return 0;
}

/**
 * LVOL resize callback
 */
static void _lvol_resize_cb(void *cb_arg, int lvolerrno)
{
    struct {
        const char *lvol_name;
        uint64_t new_size_mb;
        bool done;
        int rc;
    } *ctx = cb_arg;
    
    ctx->rc = lvolerrno;
    ctx->done = true;
    
    if (lvolerrno != 0) {
        XBDEV_ERRLOG("Failed to resize LVOL %s to %"PRIu64"MB: %d\n", 
                   ctx->lvol_name, ctx->new_size_mb, lvolerrno);
        return;
    }
    
    XBDEV_NOTICELOG("Successfully resized LVOL %s to %"PRIu64"MB\n", 
                  ctx->lvol_name, ctx->new_size_mb);
}

/**
 * Resize LVOL in SPDK thread context
 */
void xbdev_lvol_resize_on_thread(void *ctx)
{
    struct {
        const char *lvol_name;
        uint64_t new_size_mb;
        bool done;
        int rc;
    } *args = ctx;
    struct spdk_bdev *bdev;
    struct spdk_lvol *lvol;
    uint64_t size_bytes;
    
    // Find LVOL bdev
    bdev = spdk_bdev_get_by_name(args->lvol_name);
    if (!bdev) {
        XBDEV_ERRLOG("LVOL bdev not found: %s\n", args->lvol_name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // Get LVOL from bdev
    lvol = spdk_lvol_get_from_bdev(bdev);
    if (!lvol) {
        XBDEV_ERRLOG("Could not get LVOL from bdev: %s\n", args->lvol_name);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // Convert size to bytes
    size_bytes = args->new_size_mb * 1024 * 1024;
    
    // Resize LVOL
    spdk_lvol_resize(lvol, size_bytes, _lvol_resize_cb, args);
    
    // Wait for operation to complete
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * Create clone from snapshot
 *
 * @param snapshot_name Snapshot name (format: lvs_name/snapshot_name)
 * @param clone_name Clone name
 * @return 0 on success, negative error code on failure
 */
int xbdev_clone_create(const char *snapshot_name, const char *clone_name)
{
    xbdev_request_t *req;
    struct clone_create_ctx ctx = {0};
    int rc;
    
    // Validate parameters
    if (!snapshot_name || !clone_name) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // Setup context
    ctx.snapshot_name = snapshot_name;
    ctx.clone_name = clone_name;
    ctx.done = false;
    ctx.rc = 0;
    
    // Allocate request
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // Set request
    req->type = XBDEV_REQ_CLONE_CREATE;
    req->ctx = &ctx;
    
    // Execute request and wait for completion
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Check operation result
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to create clone %s from snapshot %s, rc=%d\n", 
                   clone_name, snapshot_name, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // Free resources
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("Successfully created clone %s from snapshot %s\n", 
                  clone_name, snapshot_name);
    
    return 0;
}

/**
 * Clone creation callback
 */
static void _clone_create_cb(void *cb_arg, struct spdk_lvol *clone, int lvolerrno)
{
    struct clone_create_ctx *ctx = cb_arg;
    
    if (lvolerrno != 0) {
        XBDEV_ERRLOG("Failed to create clone %s: %d\n", ctx->clone_name, lvolerrno);
        ctx->rc = lvolerrno;
        ctx->done = true;
        return;
    }
    
    ctx->clone = clone;
    ctx->rc = 0;
    ctx->done = true;
    
    XBDEV_NOTICELOG("Successfully created clone %s from snapshot %s\n",
                  ctx->clone_name, ctx->snapshot_name);
}

/**
 * Create clone in SPDK thread context
 */
void xbdev_clone_create_on_thread(void *ctx)
{
    struct clone_create_ctx *args = ctx;
    struct spdk_bdev *bdev;
    struct spdk_lvol *lvol;
    
    // Find snapshot bdev
    bdev = spdk_bdev_get_by_name(args->snapshot_name);
    if (!bdev) {
        XBDEV_ERRLOG("Snapshot bdev not found: %s\n", args->snapshot_name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // Get LVOL from bdev
    lvol = spdk_lvol_get_from_bdev(bdev);
    if (!lvol) {
        XBDEV_ERRLOG("Could not get LVOL from bdev: %s\n", args->snapshot_name);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // Create clone
    spdk_lvol_clone(lvol, args->clone_name, _clone_create_cb, args);
    
    // Wait for operation to complete
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * Destroy snapshot
 *
 * @param snapshot_name Snapshot name (format: lvs_name/snapshot_name)
 * @return 0 on success, negative error code on failure
 */
int xbdev_snapshot_destroy(const char *snapshot_name)
{
    // Snapshots are LVOLs, so we can use the LVOL destroy function
    return xbdev_lvol_destroy(snapshot_name);
}

/**
 * Apply LVOL store configuration from JSON
 */
int _xbdev_apply_lvol_stores(xbdev_config_t *config)
{
    int rc = 0;
    
    // Check configuration validity
    if (!config || !config->lvol_stores) {
        return 0; // No LVS configuration, return success
    }
    
    // Iterate through LVS configurations
    struct spdk_json_val *val = config->lvol_stores + 1;  // Skip array header
    for (uint32_t i = 0; i < config->lvol_stores->len; i++) {
        // Parse LVS configuration
        char *name = NULL;
        char *base_bdev = NULL;
        uint64_t cluster_size = 0;
        
        struct spdk_json_object_decoder lvs_decoders[] = {
            {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
            {"base_bdev", offsetof(struct { char *base_bdev; }, base_bdev), spdk_json_decode_string},
            {"cluster_size", offsetof(struct { uint64_t cluster_size; }, cluster_size), 
                spdk_json_decode_uint64, true}
        };
        
        if (spdk_json_decode_object(val, lvs_decoders, SPDK_COUNTOF(lvs_decoders), 
                                   &name, &base_bdev, &cluster_size)) {
            XBDEV_ERRLOG("Failed to parse LVS configuration item #%d\n", i);
            if (name) free(name);
            if (base_bdev) free(base_bdev);
            return -EINVAL;
        }
        
        // Create LVS
        rc = xbdev_lvs_create(base_bdev, name, cluster_size);
        
        // Free allocated memory
        free(name);
        free(base_bdev);
        
        if (rc != 0) {
            XBDEV_ERRLOG("Failed to create LVS: rc=%d\n", rc);
            return rc;
        }
        
        // Move to next configuration item
        val = spdk_json_next(val);
    }
    
    return 0;
}

/**
 * Apply LVOL volume configuration from JSON
 */
int _xbdev_apply_lvol_volumes(xbdev_config_t *config)
{
    int rc = 0;
    
    // Check configuration validity
    if (!config || !config->lvol_volumes) {
        return 0; // No LVOL configuration, return success
    }
    
    // Iterate through LVOL configurations
    struct spdk_json_val *val = config->lvol_volumes + 1;  // Skip array header
    for (uint32_t i = 0; i < config->lvol_volumes->len; i++) {
        // Parse LVOL configuration
        char *name = NULL;
        char *lvs_name = NULL;
        uint64_t size_mb = 0;
        bool thin_provision = false;
        
        struct spdk_json_object_decoder lvol_decoders[] = {
            {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
            {"lvs_name", offsetof(struct { char *lvs_name; }, lvs_name), spdk_json_decode_string},
            {"size_mb", offsetof(struct { uint64_t size_mb; }, size_mb), spdk_json_decode_uint64},
            {"thin_provision", offsetof(struct { bool thin_provision; }, thin_provision), 
                spdk_json_decode_bool, true}
        };
        
        if (spdk_json_decode_object(val, lvol_decoders, SPDK_COUNTOF(lvol_decoders), 
                                   &name, &lvs_name, &size_mb, &thin_provision)) {
            XBDEV_ERRLOG("Failed to parse LVOL configuration item #%d\n", i);
            if (name) free(name);
            if (lvs_name) free(lvs_name);
            return -EINVAL;
        }
        
        // Create LVOL
        rc = xbdev_lvol_create(lvs_name, name, size_mb, thin_provision);
        
        // Free allocated memory
        free(name);
        free(lvs_name);
        
        if (rc != 0) {
            XBDEV_ERRLOG("Failed to create LVOL: rc=%d\n", rc);
            return rc;
        }
        
        // Move to next configuration item
        val = spdk_json_next(val);
    }
    
    return 0;
}

/**
 * Apply snapshot configuration from JSON
 */
int _xbdev_apply_snapshots(xbdev_config_t *config)
{
    int rc = 0;
    
    // Check configuration validity
    if (!config || !config->snapshots) {
        return 0; // No snapshot configuration, return success
    }
    
    // Iterate through snapshot configurations
    struct spdk_json_val *val = config->snapshots + 1;  // Skip array header
    for (uint32_t i = 0; i < config->snapshots->len; i++) {
        // Parse snapshot configuration
        char *name = NULL;
        char *source_vol = NULL;
        
        struct spdk_json_object_decoder snap_decoders[] = {
            {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
            {"source_vol", offsetof(struct { char *source_vol; }, source_vol), spdk_json_decode_string}
        };
        
        if (spdk_json_decode_object(val, snap_decoders, SPDK_COUNTOF(snap_decoders), 
                                   &name, &source_vol)) {
            XBDEV_ERRLOG("Failed to parse snapshot configuration item #%d\n", i);
            if (name) free(name);
            if (source_vol) free(source_vol);
            return -EINVAL;
        }
        
        // Create snapshot
        rc = xbdev_snapshot_create(source_vol, name);
        
        // Free allocated memory
        free(name);
        free(source_vol);
        
        if (rc != 0) {
            XBDEV_ERRLOG("Failed to create snapshot: rc=%d\n", rc);
            return rc;
        }
        
        // Move to next configuration item
        val = spdk_json_next(val);
    }
    
    return 0;
}

/**
 * Apply clone configuration from JSON
 */
int _xbdev_apply_clones(xbdev_config_t *config)
{
    int rc = 0;
    
    // Check configuration validity
    if (!config || !config->clones) {
        return 0; // No clone configuration, return success
    }
    
    // Iterate through clone configurations
    struct spdk_json_val *val = config->clones + 1;  // Skip array header
    for (uint32_t i = 0; i < config->clones->len; i++) {
        // Parse clone configuration
        char *name = NULL;
        char *source_snap = NULL;
        
        struct spdk_json_object_decoder clone_decoders[] = {
            {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
            {"source_snap", offsetof(struct { char *source_snap; }, source_snap), spdk_json_decode_string}
        };
        
        if (spdk_json_decode_object(val, clone_decoders, SPDK_COUNTOF(clone_decoders), 
                                   &name, &source_snap)) {
            XBDEV_ERRLOG("Failed to parse clone configuration item #%d\n", i);
            if (name) free(name);
            if (source_snap) free(source_snap);
            return -EINVAL;
        }
        
        // Create clone
        rc = xbdev_clone_create(source_snap, name);
        
        // Free allocated memory
        free(name);
        free(source_snap);
        
        if (rc != 0) {
            XBDEV_ERRLOG("Failed to create clone: rc=%d\n", rc);
            return rc;
        }
        
        // Move to next configuration item
        val = spdk_json_next(val);
    }
    
    return 0;
}

/**
 * Write LVOL stores and volumes to JSON
 */
int _xbdev_write_lvol_json(struct spdk_json_write_ctx *w, xbdev_config_t *config)
{
    int rc = 0;
    int lvs_count = 0;
    struct spdk_lvol_store **lvs_list = NULL;
    const int MAX_LVS = 64;
    
    // Allocate temporary array to store LVS pointers
    lvs_list = calloc(MAX_LVS, sizeof(struct spdk_lvol_store *));
    if (!lvs_list) {
        XBDEV_ERRLOG("Memory allocation failed\n");
        return -ENOMEM;
    }
    
    // Write LVOL stores array
    spdk_json_write_named_array_begin(w, "lvol_stores");
    
    // Enumerate all LVOL stores
    struct spdk_json_write_ctx *local_w = w;
    struct lvs_iter_ctx {
        struct spdk_json_write_ctx *w;
        struct spdk_lvol_store **lvs_list;
        int *lvs_count;
        int max_lvs;
    } iter_ctx = {
        .w = w,
        .lvs_list = lvs_list,
        .lvs_count = &lvs_count,
        .max_lvs = MAX_LVS
    };
    
    // Get all LVS through callback
    spdk_lvs_iterate(
        [](void *cb_ctx, struct spdk_lvol_store *lvs) -> int {
            struct lvs_iter_ctx *iter_ctx = cb_ctx;
            struct spdk_json_write_ctx *w = iter_ctx->w;
            
            // Check if we have space for this LVS
            if (*iter_ctx->lvs_count >= iter_ctx->max_lvs) {
                return 1; // Stop iteration
            }
            
            // Get LVS information
            const char *lvs_name = spdk_lvs_get_name(lvs);
            const struct spdk_bdev *base_bdev = spdk_lvs_get_base_bdev(lvs);
            const char *base_bdev_name = spdk_bdev_get_name(base_bdev);
            
            // Write LVS object
            spdk_json_write_object_begin(w);
            spdk_json_write_named_string(w, "name", lvs_name);
            spdk_json_write_named_string(w, "base_bdev", base_bdev_name);
            
            // Get cluster size
            uint64_t cluster_size = spdk_lvs_get_cluster_size(lvs);
            spdk_json_write_named_uint64(w, "cluster_size", cluster_size);
            
            spdk_json_write_object_end(w);
            
            // Store LVS for later use (for volumes)
            iter_ctx->lvs_list[*iter_ctx->lvs_count] = lvs;
            (*iter_ctx->lvs_count)++;
            
            return 0; // Continue iteration
        },
        &iter_ctx
    );
    
    // End LVOL stores array
    spdk_json_write_array_end(w);
    
    // Write LVOL volumes array
    spdk_json_write_named_array_begin(w, "lvol_volumes");
    
    // Iterate through all LVOL stores and their volumes
    for (int i = 0; i < lvs_count; i++) {
        struct spdk_lvol_store *lvs = lvs_list[i];
        const char *lvs_name = spdk_lvs_get_name(lvs);
        
        // Create context for volume iteration
        struct vol_iter_ctx {
            struct spdk_json_write_ctx *w;
            const char *lvs_name;
            struct {
                char name[256];
                char uuid[UUID_STRING_LEN];
                int type; // 0=regular, 1=snapshot, 2=clone
                bool thin_provisioned;
                uint64_t size_mb;
                char parent[256]; // for snapshots/clones
            } *snapshots;
            int snapshot_count;
        } vol_ctx = {
            .w = w,
            .lvs_name = lvs_name,
        };
        
        // Iterate through all volumes in this LVS
        spdk_lvs_iter_volumes(
            lvs,
            [](void *cb_ctx, struct spdk_lvol *lvol) -> int {
                struct vol_iter_ctx *vol_ctx = cb_ctx;
                struct spdk_json_write_ctx *w = vol_ctx->w;
                
                // Get volume information
                const char *vol_name = spdk_lvol_get_name(lvol);
                uint64_t vol_size_bytes = spdk_lvol_get_size(lvol);
                uint64_t vol_size_mb = vol_size_bytes / (1024 * 1024);
                bool thin_provisioned = spdk_lvol_is_thin_provisioned(lvol);
                
                // Check if it's a snapshot or clone
                bool is_snapshot = spdk_lvol_is_snapshot(lvol);
                bool is_clone = spdk_lvol_is_clone(lvol);
                
                // For regular volumes and clones, write them immediately
                // For snapshots, collect them and write later (after parent volumes)
                if (is_snapshot) {
                    // Save for later processing (after parent volumes)
                    // This would require extending the vol_ctx to store snapshot info
                } else {
                    // Write volume object
                    spdk_json_write_object_begin(w);
                    spdk_json_write_named_string(w, "name", vol_name);
                    spdk_json_write_named_string(w, "lvs_name", vol_ctx->lvs_name);
                    spdk_json_write_named_uint64(w, "size_mb", vol_size_mb);
                    spdk_json_write_named_bool(w, "thin_provision", thin_provisioned);
                    
                    // Write clone source if applicable
                    if (is_clone) {
                        struct spdk_lvol *parent = spdk_lvol_get_parent(lvol);
                        if (parent) {
                            const char *parent_name = spdk_lvol_get_name(parent);
                            spdk_json_write_named_string(w, "clone_source", parent_name);
                        }
                    }
                    
                    spdk_json_write_object_end(w);
                }
                
                return 0; // Continue iteration
            },
            &vol_ctx
        );
    }
    
    // End LVOL volumes array
    spdk_json_write_array_end(w);
    
    // Write snapshots array
    spdk_json_write_named_array_begin(w, "snapshots");
    
    // Iterate through all LVOL stores and their snapshots
    for (int i = 0; i < lvs_count; i++) {
        struct spdk_lvol_store *lvs = lvs_list[i];
        const char *lvs_name = spdk_lvs_get_name(lvs);
        
        // Create context for snapshot iteration
        struct snap_iter_ctx {
            struct spdk_json_write_ctx *w;
            const char *lvs_name;
        } snap_ctx = {
            .w = w,
            .lvs_name = lvs_name
        };
        
        // Iterate through all snapshots in this LVS
        spdk_lvs_iter_volumes(
            lvs,
            [](void *cb_ctx, struct spdk_lvol *lvol) -> int {
                struct snap_iter_ctx *snap_ctx = cb_ctx;
                struct spdk_json_write_ctx *w = snap_ctx->w;
                
                // Skip non-snapshot volumes
                if (!spdk_lvol_is_snapshot(lvol)) {
                    return 0; // Continue iteration
                }
                
                // Write snapshot object
                const char *snap_name = spdk_lvol_get_name(lvol);
                uint64_t snap_size_bytes = spdk_lvol_get_size(lvol);
                uint64_t snap_size_mb = snap_size_bytes / (1024 * 1024);
                
                spdk_json_write_object_begin(w);
                spdk_json_write_named_string(w, "name", snap_name);
                
                // Get parent volume (the one we took snapshot of)
                struct spdk_lvol *parent = spdk_lvol_get_parent(lvol);
                if (parent) {
                    const char *parent_name = spdk_lvol_get_name(parent);
                    spdk_json_write_named_string(w, "source_vol", parent_name);
                }
                
                spdk_json_write_named_string(w, "lvs_name", snap_ctx->lvs_name);
                spdk_json_write_named_uint64(w, "size_mb", snap_size_mb);
                spdk_json_write_object_end(w);
                
                return 0; // Continue iteration
            },
            &snap_ctx
        );
    }
    
    // End snapshots array
    spdk_json_write_array_end(w);
    
    // Write clones array
    spdk_json_write_named_array_begin(w, "clones");
    
    // Iterate through all LVOL stores and their clones
    for (int i = 0; i < lvs_count; i++) {
        struct spdk_lvol_store *lvs = lvs_list[i];
        const char *lvs_name = spdk_lvs_get_name(lvs);
        
        // Create context for clone iteration
        struct clone_iter_ctx {
            struct spdk_json_write_ctx *w;
            const char *lvs_name;
        } clone_ctx = {
            .w = w,
            .lvs_name = lvs_name
        };
        
        // Iterate through all clones in this LVS
        spdk_lvs_iter_volumes(
            lvs,
            [](void *cb_ctx, struct spdk_lvol *lvol) -> int {
                struct clone_iter_ctx *clone_ctx = cb_ctx;
                struct spdk_json_write_ctx *w = clone_ctx->w;
                
                // Skip volumes that are not clones
                if (!spdk_lvol_is_clone(lvol)) {
                    return 0; // Continue iteration
                }
                
                // Write clone object
                const char *clone_name = spdk_lvol_get_name(lvol);
                uint64_t clone_size_bytes = spdk_lvol_get_size(lvol);
                uint64_t clone_size_mb = clone_size_bytes / (1024 * 1024);
                
                spdk_json_write_object_begin(w);
                spdk_json_write_named_string(w, "name", clone_name);
                
                // Get source snapshot
                struct spdk_lvol *source = spdk_lvol_get_parent(lvol);
                if (source) {
                    const char *source_name = spdk_lvol_get_name(source);
                    spdk_json_write_named_string(w, "source_snap", source_name);
                }
                
                spdk_json_write_named_string(w, "lvs_name", clone_ctx->lvs_name);
                spdk_json_write_named_uint64(w, "size_mb", clone_size_mb);
                spdk_json_write_object_end(w);
                
                return 0; // Continue iteration
            },
            &clone_ctx
        );
    }
    
    // End clones array
    spdk_json_write_array_end(w);
    
    // Free resources
    free(lvs_list);
    
    return 0;
}

/**
 * List all LVOL stores
 * 
 * @param lvs_names Array to store LVS names
 * @param max_lvs Maximum number of LVS to return
 * @return Number of LVS found or negative error code
 */
int xbdev_lvs_list(char **lvs_names, int max_lvs)
{
    int lvs_count = 0;
    
    // Validate parameters
    if (!lvs_names || max_lvs <= 0) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // Clear output array
    for (int i = 0; i < max_lvs; i++) {
        lvs_names[i] = NULL;
    }
    
    // Create context for LVS iteration
    struct lvs_list_ctx {
        char **lvs_names;
        int max_lvs;
        int count;
    } ctx = {
        .lvs_names = lvs_names,
        .max_lvs = max_lvs,
        .count = 0
    };
    
    // Iterate through all LVS
    spdk_lvs_iterate(
        [](void *cb_ctx, struct spdk_lvol_store *lvs) -> int {
            struct lvs_list_ctx *ctx = cb_ctx;
            
            // Check if we have space for this LVS
            if (ctx->count >= ctx->max_lvs) {
                return 1; // Stop iteration
            }
            
            // Get LVS name
            const char *lvs_name = spdk_lvs_get_name(lvs);
            ctx->lvs_names[ctx->count] = strdup(lvs_name);
            if (!ctx->lvs_names[ctx->count]) {
                // Memory allocation failed
                // Free already allocated names
                for (int i = 0; i < ctx->count; i++) {
                    free(ctx->lvs_names[i]);
                    ctx->lvs_names[i] = NULL;
                }
                ctx->count = -ENOMEM;
                return 1; // Stop iteration
            }
            
            ctx->count++;
            return 0; // Continue iteration
        },
        &ctx
    );
    
    // Check if iteration failed
    if (ctx.count < 0) {
        return ctx.count; // Return error code
    }
    
    return ctx.count; // Return number of LVS found
}

/**
 * List all LVOL volumes in a store
 * 
 * @param lvs_name LVS name
 * @param lvol_names Array to store LVOL names
 * @param max_lvols Maximum number of LVOLs to return
 * @return Number of LVOLs found or negative error code
 */
int xbdev_lvol_list(const char *lvs_name, char **lvol_names, int max_lvols)
{
    struct spdk_lvol_store *lvs = NULL;
    int lvol_count = 0;
    
    // Validate parameters
    if (!lvs_name || !lvol_names || max_lvols <= 0) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // Clear output array
    for (int i = 0; i < max_lvols; i++) {
        lvol_names[i] = NULL;
    }
    
    // Create context for find operation
    struct find_lvs_ctx {
        struct spdk_lvol_store *lvs;
        const char *name;
        bool found;
    } find_ctx = {
        .lvs = NULL,
        .name = lvs_name,
        .found = false
    };
    
    // Find LVS
    spdk_lvs_iterate(
        [](void *cb_ctx, struct spdk_lvol_store *lvs) -> int {
            struct find_lvs_ctx *ctx = cb_ctx;
            const char *lvs_name = spdk_lvs_get_name(lvs);
            
            if (strcmp(lvs_name, ctx->name) == 0) {
                ctx->lvs = lvs;
                ctx->found = true;
                return 1; // Stop iteration
            }
            
            return 0; // Continue iteration
        },
        &find_ctx
    );
    
    // Check if LVS was found
    if (!find_ctx.found) {
        XBDEV_ERRLOG("LVS not found: %s\n", lvs_name);
        return -ENOENT;
    }
    
    lvs = find_ctx.lvs;
    
    // Create context for LVOL iteration
    struct lvol_list_ctx {
        char **lvol_names;
        int max_lvols;
        int count;
    } ctx = {
        .lvol_names = lvol_names,
        .max_lvols = max_lvols,
        .count = 0
    };
    
    // Iterate through all LVOLs in this LVS
    spdk_lvs_iter_volumes(
        lvs,
        [](void *cb_ctx, struct spdk_lvol *lvol) -> int {
            struct lvol_list_ctx *ctx = cb_ctx;
            
            // Check if we have space for this LVOL
            if (ctx->count >= ctx->max_lvols) {
                return 1; // Stop iteration
            }
            
            // Get LVOL name
            const char *lvol_name = spdk_lvol_get_name(lvol);
            ctx->lvol_names[ctx->count] = strdup(lvol_name);
            if (!ctx->lvol_names[ctx->count]) {
                // Memory allocation failed
                // Free already allocated names
                for (int i = 0; i < ctx->count; i++) {
                    free(ctx->lvol_names[i]);
                    ctx->lvol_names[i] = NULL;
                }
                ctx->count = -ENOMEM;
                return 1; // Stop iteration
            }
            
            ctx->count++;
            return 0; // Continue iteration
        },
        &ctx
    );
    
    // Check if iteration failed
    if (ctx.count < 0) {
        return ctx.count; // Return error code
    }
    
    return ctx.count; // Return number of LVOLs found
}

/**
 * Get LVOL information
 * 
 * @param lvol_name LVOL name (format: lvs_name/lvol_name)
 * @param info Output parameter to store LVOL information
 * @return 0 on success, negative error code on failure
 */
int xbdev_lvol_get_info(const char *lvol_name, xbdev_lvol_info_t *info)
{
    struct spdk_bdev *bdev;
    struct spdk_lvol *lvol;
    
    // Validate parameters
    if (!lvol_name || !info) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // Find LVOL bdev
    bdev = spdk_bdev_get_by_name(lvol_name);
    if (!bdev) {
        XBDEV_ERRLOG("LVOL bdev not found: %s\n", lvol_name);
        return -ENODEV;
    }
    
    // Get LVOL from bdev
    lvol = spdk_lvol_get_from_bdev(bdev);
    if (!lvol) {
        XBDEV_ERRLOG("Could not get LVOL from bdev: %s\n", lvol_name);
        return -EINVAL;
    }
    
    // Fill in information
    memset(info, 0, sizeof(xbdev_lvol_info_t));
    
    // Get LVOL properties
    strncpy(info->name, spdk_lvol_get_name(lvol), sizeof(info->name) - 1);
    
    // Get LVS name
    struct spdk_lvol_store *lvs = spdk_lvol_get_lvstore(lvol);
    if (lvs) {
        strncpy(info->lvs_name, spdk_lvs_get_name(lvs), sizeof(info->lvs_name) - 1);
    }
    
    // Get size
    info->size_bytes = spdk_lvol_get_size(lvol);
    info->size_mb = info->size_bytes / (1024 * 1024);
    
    // Get type and status
    info->thin_provisioned = spdk_lvol_is_thin_provisioned(lvol);
    info->is_snapshot = spdk_lvol_is_snapshot(lvol);
    info->is_clone = spdk_lvol_is_clone(lvol);
    
    // Get parent information
    struct spdk_lvol *parent = spdk_lvol_get_parent(lvol);
    if (parent) {
        info->has_parent = true;
        strncpy(info->parent_name, spdk_lvol_get_name(parent), sizeof(info->parent_name) - 1);
    }
    
    // Get block device information
    info->block_size = spdk_bdev_get_block_size(bdev);
    info->num_blocks = spdk_bdev_get_num_blocks(bdev);
    
    return 0;
}

/**
 * Get list of clones for a snapshot
 * 
 * @param snapshot_name Snapshot name (format: lvs_name/snapshot_name)
 * @param clone_names Array to store clone names
 * @param max_clones Maximum number of clones to return
 * @return Number of clones found or negative error code
 */
int xbdev_snapshot_get_clones(const char *snapshot_name, char **clone_names, int max_clones)
{
    struct spdk_bdev *snap_bdev;
    struct spdk_lvol *snap_lvol;
    
    // Validate parameters
    if (!snapshot_name || !clone_names || max_clones <= 0) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // Clear output array
    for (int i = 0; i < max_clones; i++) {
        clone_names[i] = NULL;
    }
    
    // Find snapshot bdev
    snap_bdev = spdk_bdev_get_by_name(snapshot_name);
    if (!snap_bdev) {
        XBDEV_ERRLOG("Snapshot bdev not found: %s\n", snapshot_name);
        return -ENODEV;
    }
    
    // Get snapshot LVOL
    snap_lvol = spdk_lvol_get_from_bdev(snap_bdev);
    if (!snap_lvol) {
        XBDEV_ERRLOG("Could not get snapshot LVOL from bdev: %s\n", snapshot_name);
        return -EINVAL;
    }
    
    // Check if it's really a snapshot
    if (!spdk_lvol_is_snapshot(snap_lvol)) {
        XBDEV_ERRLOG("Not a snapshot: %s\n", snapshot_name);
        return -EINVAL;
    }
    
    // Create context for clone finding
    struct clone_find_ctx {
        struct spdk_lvol *snap_lvol;
        char **clone_names;
        int max_clones;
        int count;
    } ctx = {
        .snap_lvol = snap_lvol,
        .clone_names = clone_names,
        .max_clones = max_clones,
        .count = 0
    };
    
    // Get LVS
    struct spdk_lvol_store *lvs = spdk_lvol_get_lvstore(snap_lvol);
    if (!lvs) {
        XBDEV_ERRLOG("Could not get LVS for snapshot: %s\n", snapshot_name);
        return -EINVAL;
    }
    
    // Iterate through all LVOLs in this LVS to find clones of this snapshot
    spdk_lvs_iter_volumes(
        lvs,
        [](void *cb_ctx, struct spdk_lvol *lvol) -> int {
            struct clone_find_ctx *ctx = cb_ctx;
            
            // Skip if not a clone
            if (!spdk_lvol_is_clone(lvol)) {
                return 0; // Continue iteration
            }
            
            // Check if this clone's parent is our snapshot
            struct spdk_lvol *parent = spdk_lvol_get_parent(lvol);
            if (parent == ctx->snap_lvol) {
                // This is a clone of our snapshot
                
                // Check if we have space for this clone
                if (ctx->count >= ctx->max_clones) {
                    return 1; // Stop iteration
                }
                
                // Get clone name
                const char *clone_name = spdk_lvol_get_name(lvol);
                ctx->clone_names[ctx->count] = strdup(clone_name);
                if (!ctx->clone_names[ctx->count]) {
                    // Memory allocation failed
                    // Free already allocated names
                    for (int i = 0; i < ctx->count; i++) {
                        free(ctx->clone_names[i]);
                        ctx->clone_names[i] = NULL;
                    }
                    ctx->count = -ENOMEM;
                    return 1; // Stop iteration
                }
                
                ctx->count++;
            }
            
            return 0; // Continue iteration
        },
        &ctx
    );
    
    // Check if iteration failed
    if (ctx.count < 0) {
        return ctx.count; // Return error code
    }
    
    return ctx.count; // Return number of clones found
}

/**
 * Create multiple LVOL volumes at once
 * 
 * @param lvs_name LVOL store name
 * @param volume_specs Array of volume specifications
 * @param num_volumes Number of volumes to create
 * @return 0 on success, negative error code on failure
 */
int xbdev_lvol_create_many(const char *lvs_name, xbdev_lvol_spec_t *volume_specs, int num_volumes)
{
    int rc = 0;
    
    // Validate parameters
    if (!lvs_name || !volume_specs || num_volumes <= 0) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // Create each LVOL one by one
    for (int i = 0; i < num_volumes; i++) {
        xbdev_lvol_spec_t *spec = &volume_specs[i];
        
        rc = xbdev_lvol_create(lvs_name, spec->name, spec->size_mb, spec->thin_provision);
        if (rc != 0) {
            XBDEV_ERRLOG("Failed to create LVOL %s: %d\n", spec->name, rc);
            
            // Clean up already created volumes if requested
            if (spec->rollback_on_error) {
                for (int j = 0; j < i; j++) {
                    char full_name[256];
                    snprintf(full_name, sizeof(full_name), "%s/%s", lvs_name, volume_specs[j].name);
                    xbdev_lvol_destroy(full_name);
                }
            }
            
            return rc;
        }
    }
    
    return 0;
}

/**
 * Create a set of interdependent LVOLs (volumes, snapshots, clones)
 * 
 * @param config LVOL configuration structure
 * @return 0 on success, negative error code on failure
 */
int xbdev_lvol_create_set(xbdev_lvol_set_config_t *config)
{
    int rc = 0;
    
    // Validate parameters
    if (!config || !config->lvs_name) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // First create all volumes
    if (config->volumes) {
        for (int i = 0; i < config->num_volumes; i++) {
            xbdev_lvol_spec_t *spec = &config->volumes[i];
            rc = xbdev_lvol_create(config->lvs_name, spec->name, spec->size_mb, spec->thin_provision);
            if (rc != 0) {
                XBDEV_ERRLOG("Failed to create LVOL %s: %d\n", spec->name, rc);
                return rc;
            }
        }
    }
    
    // Then create snapshots
    if (config->snapshots) {
        for (int i = 0; i < config->num_snapshots; i++) {
            xbdev_snapshot_spec_t *spec = &config->snapshots[i];
            char source_vol_path[256];
            
            // Create full path for source volume
            if (strchr(spec->source_vol, '/')) {
                // Already has full path
                strncpy(source_vol_path, spec->source_vol, sizeof(source_vol_path) - 1);
            } else {
                // Prepend LVS name
                snprintf(source_vol_path, sizeof(source_vol_path), "%s/%s", config->lvs_name, spec->source_vol);
            }
            
            rc = xbdev_snapshot_create(source_vol_path, spec->name);
            if (rc != 0) {
                XBDEV_ERRLOG("Failed to create snapshot %s: %d\n", spec->name, rc);
                return rc;
            }
        }
    }
    
    // Finally create clones
    if (config->clones) {
        for (int i = 0; i < config->num_clones; i++) {
            xbdev_clone_spec_t *spec = &config->clones[i];
            char source_snap_path[256];
            char clone_path[256];
            
            // Create full path for source snapshot
            if (strchr(spec->source_snap, '/')) {
                // Already has full path
                strncpy(source_snap_path, spec->source_snap, sizeof(source_snap_path) - 1);
            } else {
                // Prepend LVS name
                snprintf(source_snap_path, sizeof(source_snap_path), "%s/%s", config->lvs_name, spec->source_snap);
            }
            
            // Create full path for clone
            if (strchr(spec->name, '/')) {
                // Already has full path
                strncpy(clone_path, spec->name, sizeof(clone_path) - 1);
            } else {
                // Prepend LVS name
                snprintf(clone_path, sizeof(clone_path), "%s/%s", config->lvs_name, spec->name);
            }
            
            rc = xbdev_clone_create(source_snap_path, clone_path);
            if (rc != 0) {
                XBDEV_ERRLOG("Failed to create clone %s: %d\n", spec->name, rc);
                return rc;
            }
        }
    }
    
    return 0;
}
