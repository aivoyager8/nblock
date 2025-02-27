/**
 * @file md.c
 * @brief 实现基于SPDK的RAID设备管理功能
 *
 * 该文件实现类似Linux MD设备的RAID管理接口，支持RAID0、RAID1、RAID4、RAID5、RAID6和RAID10。
 * 提供设备的创建、启动、停止、检查以及热备盘管理等功能。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/bdev.h>
#include <spdk/bdev_zone.h>
#include <spdk/util.h>
#include <spdk/string.h>
#include <uuid/uuid.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// RAID类型常量
#define XBDEV_MD_TYPE_LINEAR  "Linear"
#define XBDEV_MD_TYPE_RAID0   "raid0"
#define XBDEV_MD_TYPE_RAID1   "raid1"
#define XBDEV_MD_TYPE_RAID4   "raid4"
#define XBDEV_MD_TYPE_RAID5   "raid5"
#define XBDEV_MD_TYPE_RAID6   "raid6"
#define XBDEV_MD_TYPE_RAID10  "raid10"

// RAID设备限制
#define XBDEV_MD_MAX_DEVICES 32  // 每个RAID最大设备数
#define XBDEV_MD_MAX_RAIDS   16  // 最大支持RAID数量

/**
 * RAID设备状态结构
 */
typedef struct {
    bool active;                     // 设备是否活跃
    char name[64];                   // 设备名称
    int level;                       // RAID级别
    int disk_count;                  // 磁盘数量
    char **disk_names;               // 磁盘名称数组
    bool *disk_state;                // 磁盘状态数组（正常/失效）
    struct spdk_bdev_part_base *base_bdev; // 基础BDEV
    uint64_t chunk_size_kb;          // 条带大小（KB）
    int layout;                      // RAID布局算法
    char uuid_str[37];               // UUID字符串
    bool rebuild_active;             // 是否正在重建
    float rebuild_progress;          // 重建进度（0-1）
    uint32_t rebuild_speed_kb;       // 重建速度（KB/秒）
    int failed_disk_count;           // 故障磁盘数量
    int spare_disk_count;            // 热备盘数量
    char **spare_disks;              // 热备盘名称数组
    bool degraded;                   // 是否处于降级模式
    pthread_mutex_t lock;            // 互斥锁
} xbdev_md_dev_t;

/**
 * 全局RAID设备表
 */
static xbdev_md_dev_t g_md_devices[XBDEV_MD_MAX_RAIDS];
static pthread_mutex_t g_md_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_md_initialized = false;

/**
 * RAID设备初始化
 */
int xbdev_md_module_init(void)
{
    pthread_mutex_lock(&g_md_mutex);
    
    if (g_md_initialized) {
        pthread_mutex_unlock(&g_md_mutex);
        return 0;
    }
    
    // 初始化设备表
    memset(g_md_devices, 0, sizeof(g_md_devices));
    for (int i = 0; i < XBDEV_MD_MAX_RAIDS; i++) {
        pthread_mutex_init(&g_md_devices[i].lock, NULL);
    }
    
    g_md_initialized = true;
    
    pthread_mutex_unlock(&g_md_mutex);
    
    XBDEV_NOTICELOG("RAID管理模块已初始化\n");
    return 0;
}

/**
 * RAID设备模块清理
 */
void xbdev_md_module_fini(void)
{
    pthread_mutex_lock(&g_md_mutex);
    
    if (!g_md_initialized) {
        pthread_mutex_unlock(&g_md_mutex);
        return;
    }
    
    // 停止所有活跃的RAID设备
    for (int i = 0; i < XBDEV_MD_MAX_RAIDS; i++) {
        if (g_md_devices[i].active) {
            // 这里应该调用停止RAID的函数
            // 但我们不直接调用，避免死锁
            XBDEV_WARNLOG("清理时RAID设备仍然活跃: %s\n", g_md_devices[i].name);
        }
        
        // 清理资源
        if (g_md_devices[i].disk_names) {
            for (int j = 0; j < g_md_devices[i].disk_count; j++) {
                if (g_md_devices[i].disk_names[j]) {
                    free(g_md_devices[i].disk_names[j]);
                }
            }
            free(g_md_devices[i].disk_names);
        }
        
        if (g_md_devices[i].disk_state) {
            free(g_md_devices[i].disk_state);
        }
        
        if (g_md_devices[i].spare_disks) {
            for (int j = 0; j < g_md_devices[i].spare_disk_count; j++) {
                if (g_md_devices[i].spare_disks[j]) {
                    free(g_md_devices[i].spare_disks[j]);
                }
            }
            free(g_md_devices[i].spare_disks);
        }
        
        // 销毁互斥锁
        pthread_mutex_destroy(&g_md_devices[i].lock);
    }
    
    g_md_initialized = false;
    
    pthread_mutex_unlock(&g_md_mutex);
    
    XBDEV_NOTICELOG("RAID管理模块已清理\n");
}

/**
 * 查找空闲的RAID设备槽
 */
static int find_free_md_slot(void)
{
    for (int i = 0; i < XBDEV_MD_MAX_RAIDS; i++) {
        if (!g_md_devices[i].active) {
            return i;
        }
    }
    return -1;
}

/**
 * 根据名称查找RAID设备
 */
static int find_md_by_name(const char *name)
{
    if (!name) return -1;
    
    for (int i = 0; i < XBDEV_MD_MAX_RAIDS; i++) {
        if (g_md_devices[i].active && strcmp(g_md_devices[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * 检查名称是否已存在
 */
static bool md_name_exists(const char *name)
{
    return find_md_by_name(name) >= 0;
}

/**
 * RAID设备创建（在SPDK线程中执行）
 */
static void xbdev_md_create_on_thread(void *arg)
{
    struct {
        const char *name;
        int level;
        const char **base_bdevs;
        int num_base_bdevs;
        xbdev_md_config_t *config;
        int rc;
        bool done;
    } *ctx = arg;
    
    struct spdk_bdev *raid_bdev = NULL;
    char uuid_str[37] = {0};
    const char *raid_type;
    
    // 参数检查
    if (ctx->num_base_bdevs < 2 || !ctx->name || !ctx->base_bdevs) {
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取RAID类型字符串
    switch (ctx->level) {
        case XBDEV_MD_LEVEL_LINEAR:
            raid_type = XBDEV_MD_TYPE_LINEAR;
            break;
        case XBDEV_MD_LEVEL_RAID0:
            raid_type = XBDEV_MD_TYPE_RAID0;
            break;
        case XBDEV_MD_LEVEL_RAID1:
            raid_type = XBDEV_MD_TYPE_RAID1;
            break;
        case XBDEV_MD_LEVEL_RAID4:
            raid_type = XBDEV_MD_TYPE_RAID4;
            break;
        case XBDEV_MD_LEVEL_RAID5:
            raid_type = XBDEV_MD_TYPE_RAID5;
            break;
        case XBDEV_MD_LEVEL_RAID6:
            raid_type = XBDEV_MD_TYPE_RAID6;
            break;
        case XBDEV_MD_LEVEL_RAID10:
            raid_type = XBDEV_MD_TYPE_RAID10;
            break;
        default:
            XBDEV_ERRLOG("不支持的RAID级别: %d\n", ctx->level);
            ctx->rc = -EINVAL;
            ctx->done = true;
            return;
    }
    
    // 生成UUID
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
    
    // 查找所有基础设备
    struct spdk_bdev **base_bdevs = calloc(ctx->num_base_bdevs, sizeof(struct spdk_bdev *));
    if (!base_bdevs) {
        ctx->rc = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 计算条带大小（默认为64KB或配置值）
    uint64_t strip_size_kb = ctx->config ? ctx->config->chunk_size_kb : 64;
    if (strip_size_kb == 0) strip_size_kb = 64;
    
    // 定义layout（仅对RAID5/6有效）
    int layout = ctx->config ? ctx->config->layout : 0;
    
    // 为每个基础设备打开BDEV
    for (int i = 0; i < ctx->num_base_bdevs; i++) {
        base_bdevs[i] = spdk_bdev_get_by_name(ctx->base_bdevs[i]);
        if (!base_bdevs[i]) {
            XBDEV_ERRLOG("找不到基础设备: %s\n", ctx->base_bdevs[i]);
            free(base_bdevs);
            ctx->rc = -ENODEV;
            ctx->done = true;
            return;
        }
    }
    
    // 创建RAID BDEV
    if (ctx->level == XBDEV_MD_LEVEL_RAID0) {
        // 创建RAID0
        ctx->rc = spdk_bdev_raid_create_raid0(ctx->name, strip_size_kb * 1024, ctx->num_base_bdevs);
    } else if (ctx->level == XBDEV_MD_LEVEL_RAID1) {
        // 创建RAID1
        ctx->rc = spdk_bdev_raid_create_raid1(ctx->name, ctx->num_base_bdevs);
    } else if (ctx->level == XBDEV_MD_LEVEL_RAID5) {
        // 创建RAID5
        ctx->rc = spdk_bdev_raid_create_raid5(ctx->name, strip_size_kb * 1024, ctx->num_base_bdevs, layout);
    } else if (ctx->level == XBDEV_MD_LEVEL_RAID10) {
        // 创建RAID10（RAID1+0）
        ctx->rc = spdk_bdev_raid_create_raid10(ctx->name, strip_size_kb * 1024, ctx->num_base_bdevs);
    } else {
        // 其他RAID级别暂不支持
        XBDEV_ERRLOG("RAID级别%d尚未实现\n", ctx->level);
        free(base_bdevs);
        ctx->rc = -ENOTSUP;
        ctx->done = true;
        return;
    }
    
    if (ctx->rc != 0) {
        XBDEV_ERRLOG("创建RAID设备失败: %s, rc=%d\n", ctx->name, ctx->rc);
        free(base_bdevs);
        ctx->done = true;
        return;
    }
    
    // 查找创建的RAID设备
    raid_bdev = spdk_bdev_get_by_name(ctx->name);
    if (!raid_bdev) {
        XBDEV_ERRLOG("无法获取刚创建的RAID设备: %s\n", ctx->name);
        free(base_bdevs);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 为每个基础设备添加到RAID
    for (int i = 0; i < ctx->num_base_bdevs; i++) {
        ctx->rc = spdk_bdev_raid_add_base_device(ctx->name, ctx->base_bdevs[i], i);
        if (ctx->rc != 0) {
            XBDEV_ERRLOG("添加基础设备到RAID失败: %s, rc=%d\n", ctx->base_bdevs[i], ctx->rc);
            
            // 清理已添加的设备
            for (int j = 0; j < i; j++) {
                spdk_bdev_raid_remove_base_device(ctx->name, j);
            }
            
            // 删除RAID设备
            spdk_bdev_raid_delete(ctx->name);
            
            free(base_bdevs);
            ctx->done = true;
            return;
        }
    }
    
    // 获取分配的RAID设备槽
    int raid_idx = -1;
    pthread_mutex_lock(&g_md_mutex);
    raid_idx = find_free_md_slot();
    if (raid_idx < 0) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("无可用RAID设备槽\n");
        
        // 清理RAID设备
        for (int i = 0; i < ctx->num_base_bdevs; i++) {
            spdk_bdev_raid_remove_base_device(ctx->name, i);
        }
        spdk_bdev_raid_delete(ctx->name);
        
        free(base_bdevs);
        ctx->rc = -ENOSPC;
        ctx->done = true;
        return;
    }
    
    // 填充RAID设备信息
    xbdev_md_dev_t *md_dev = &g_md_devices[raid_idx];
    pthread_mutex_lock(&md_dev->lock);
    
    md_dev->active = true;
    strncpy(md_dev->name, ctx->name, sizeof(md_dev->name) - 1);
    md_dev->level = ctx->level;
    md_dev->disk_count = ctx->num_base_bdevs;
    md_dev->chunk_size_kb = strip_size_kb;
    md_dev->layout = layout;
    strncpy(md_dev->uuid_str, uuid_str, sizeof(md_dev->uuid_str) - 1);
    md_dev->rebuild_active = false;
    md_dev->rebuild_progress = 0.0;
    md_dev->failed_disk_count = 0;
    md_dev->spare_disk_count = 0;
    md_dev->degraded = false;
    
    // 分配并填充磁盘名称数组
    md_dev->disk_names = calloc(ctx->num_base_bdevs, sizeof(char *));
    md_dev->disk_state = calloc(ctx->num_base_bdevs, sizeof(bool));
    if (!md_dev->disk_names || !md_dev->disk_state) {
        pthread_mutex_unlock(&md_dev->lock);
        pthread_mutex_unlock(&g_md_mutex);
        
        // 清理RAID设备
        for (int i = 0; i < ctx->num_base_bdevs; i++) {
            spdk_bdev_raid_remove_base_device(ctx->name, i);
        }
        spdk_bdev_raid_delete(ctx->name);
        
        if (md_dev->disk_names) free(md_dev->disk_names);
        if (md_dev->disk_state) free(md_dev->disk_state);
        md_dev->disk_names = NULL;
        md_dev->disk_state = NULL;
        md_dev->active = false;
        
        free(base_bdevs);
        ctx->rc = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 复制磁盘名称
    for (int i = 0; i < ctx->num_base_bdevs; i++) {
        md_dev->disk_names[i] = strdup(ctx->base_bdevs[i]);
        md_dev->disk_state[i] = true; // 初始状态为正常
    }
    
    // 初始化热备盘
    md_dev->spare_disks = NULL;
    
    pthread_mutex_unlock(&md_dev->lock);
    pthread_mutex_unlock(&g_md_mutex);
    
    XBDEV_NOTICELOG("成功创建RAID设备: %s (级别: %d, 设备数: %d)\n", 
                  ctx->name, ctx->level, ctx->num_base_bdevs);
    
    free(base_bdevs);
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 创建RAID设备
 * 
 * @param name RAID设备名称
 * @param level RAID级别 (0=Linear, 1=RAID0, 2=RAID1, ...)
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param config RAID配置参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create(const char *name, int level, const char **base_bdevs, int num_base_bdevs, xbdev_md_config_t *config)
{
    xbdev_request_t *req;
    struct {
        const char *name;
        int level;
        const char **base_bdevs;
        int num_base_bdevs;
        xbdev_md_config_t *config;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (!name || !base_bdevs || num_base_bdevs < 2) {
        XBDEV_ERRLOG("无效的RAID参数\n");
        return -EINVAL;
    }
    
    // 检查RAID级别有效性
    if (level != XBDEV_MD_LEVEL_LINEAR && level != XBDEV_MD_LEVEL_RAID0 && 
        level != XBDEV_MD_LEVEL_RAID1 && level != XBDEV_MD_LEVEL_RAID4 && 
        level != XBDEV_MD_LEVEL_RAID5 && level != XBDEV_MD_LEVEL_RAID6 && 
        level != XBDEV_MD_LEVEL_RAID10) {
        XBDEV_ERRLOG("无效的RAID级别: %d\n", level);
        return -EINVAL;
    }
    
    // 检查名称是否已存在
    pthread_mutex_lock(&g_md_mutex);
    if (md_name_exists(name)) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("RAID设备名称已存在: %s\n", name);
        return -EEXIST;
    }
    pthread_mutex_unlock(&g_md_mutex);
    
    // 设置上下文
    ctx.name = name;
    ctx.level = level;
    ctx.base_bdevs = base_bdevs;
    ctx.num_base_bdevs = num_base_bdevs;
    ctx.config = config;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型
    req->type = XBDEV_REQ_RAID_CREATE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("创建RAID设备失败: %s, rc=%d\n", name, rc);
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * 创建RAID0设备（便捷函数）
 */
int xbdev_md_create_raid0(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb)
{
    xbdev_md_config_t config = {
        .chunk_size_kb = chunk_size_kb,
        .assume_clean = false,
        .layout = 0
    };
    
    return xbdev_md_create(name, XBDEV_MD_LEVEL_RAID0, base_bdevs, num_base_bdevs, &config);
}

/**
 * 创建RAID1设备（便捷函数）
 */
int xbdev_md_create_raid1(const char *name, const char **base_bdevs, int num_base_bdevs)
{
    // RAID1不需要条带大小
    xbdev_md_config_t config = {
        .chunk_size_kb = 0,
        .assume_clean = false,
        .layout = 0
    };
    
    return xbdev_md_create(name, XBDEV_MD_LEVEL_RAID1, base_bdevs, num_base_bdevs, &config);
}

/**
 * 创建RAID5设备（便捷函数）
 */
int xbdev_md_create_raid5(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb)
{
    // 对于RAID5，布局默认为左对称（left-symmetric）
    xbdev_md_config_t config = {
        .chunk_size_kb = chunk_size_kb,
        .assume_clean = false,
        .layout = 0 // left-symmetric
    };
    
    return xbdev_md_create(name, XBDEV_MD_LEVEL_RAID5, base_bdevs, num_base_bdevs, &config);
}

/**
 * 创建RAID10设备（便捷函数）
 */
int xbdev_md_create_raid10(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb)
{
    xbdev_md_config_t config = {
        .chunk_size_kb = chunk_size_kb,
        .assume_clean = false,
        .layout = 0
    };
    
    return xbdev_md_create(name, XBDEV_MD_LEVEL_RAID10, base_bdevs, num_base_bdevs, &config);
}

/**
 * 创建线性RAID设备（便捷函数）
 */
int xbdev_md_create_linear(const char *name, const char **base_bdevs, int num_base_bdevs)
{
    xbdev_md_config_t config = {
        .chunk_size_kb = 0,  // 线性RAID不需要条带大小
        .assume_clean = true,
        .layout = 0
    };
    
    return xbdev_md_create(name, XBDEV_MD_LEVEL_LINEAR, base_bdevs, num_base_bdevs, &config);
}

/**
 * RAID设备停止（在SPDK线程中执行）
 */
static void xbdev_md_stop_on_thread(void *arg)
{
    struct {
        const char *name;
        int rc;
        bool done;
    } *ctx = arg;
    
    int raid_idx;
    xbdev_md_dev_t *md_dev = NULL;
    
    // 查找RAID设备
    pthread_mutex_lock(&g_md_mutex);
    raid_idx = find_md_by_name(ctx->name);
    if (raid_idx < 0) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("找不到RAID设备: %s\n", ctx->name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    md_dev = &g_md_devices[raid_idx];
    pthread_mutex_lock(&md_dev->lock);
    pthread_mutex_unlock(&g_md_mutex);
    
    // 删除RAID设备
    ctx->rc = spdk_bdev_raid_delete(ctx->name);
    if (ctx->rc != 0) {
        XBDEV_ERRLOG("删除RAID设备失败: %s, rc=%d\n", ctx->name, ctx->rc);
        pthread_mutex_unlock(&md_dev->lock);
        ctx->done = true;
        return;
    }
    
    // 清理设备资源
    if (md_dev->disk_names) {
        for (int i = 0; i < md_dev->disk_count; i++) {
            if (md_dev->disk_names[i]) {
                free(md_dev->disk_names[i]);
            }
        }
        free(md_dev->disk_names);
        md_dev->disk_names = NULL;
    }
    
    if (md_dev->disk_state) {
        free(md_dev->disk_state);
        md_dev->disk_state = NULL;
    }
    
    // 清理热备盘
    if (md_dev->spare_disks) {
        for (int i = 0; i < md_dev->spare_disk_count; i++) {
            if (md_dev->spare_disks[i]) {
                free(md_dev->spare_disks[i]);
            }
        }
        free(md_dev->spare_disks);
        md_dev->spare_disks = NULL;
    }
    
    // 标记设备为非活跃
    md_dev->active = false;
    
    pthread_mutex_unlock(&md_dev->lock);
    
    XBDEV_NOTICELOG("成功停止RAID设备: %s\n", ctx->name);
    
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 停止RAID设备
 * 
 * @param name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_stop(const char *name)
{
    xbdev_request_t *req;
    struct {
        const char *name;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (!name) {
        XBDEV_ERRLOG("无效的RAID名称\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.name = name;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型
    req->type = XBDEV_REQ_RAID_STOP;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("停止RAID设备失败: %s, rc=%d\n", name, rc);
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * RAID设备组装（从现有的设备重建RAID）
 * 在SPDK线程中执行
 */
static void xbdev_md_assemble_on_thread(void *arg)
{
    struct {
        const char *name;
        int level;
        const char **base_bdevs;
        int num_base_bdevs;
        int rc;
        bool done;
    } *ctx = arg;
    
    char uuid_str[37];
    uuid_t uuid;
    int raid_idx;
    
    // 参数检查
    if (ctx->num_base_bdevs < 2 || !ctx->name || !ctx->base_bdevs) {
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 生成UUID
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
    
    // 检查是否可以创建RAID设备
    for (int i = 0; i < ctx->num_base_bdevs; i++) {
        if (!spdk_bdev_get_by_name(ctx->base_bdevs[i])) {
            XBDEV_ERRLOG("RAID组装失败: 找不到设备 %s\n", ctx->base_bdevs[i]);
            ctx->rc = -ENODEV;
            ctx->done = true;
            return;
        }
    }
    
    // 为RAID创建基本参数
    switch (ctx->level) {
        case XBDEV_MD_LEVEL_RAID0:
            // 暂时使用默认64KB条带大小
            ctx->rc = spdk_bdev_raid_create_raid0(ctx->name, 64 * 1024, ctx->num_base_bdevs);
            break;
            
        case XBDEV_MD_LEVEL_RAID1:
            ctx->rc = spdk_bdev_raid_create_raid1(ctx->name, ctx->num_base_bdevs);
            break;
            
        case XBDEV_MD_LEVEL_RAID5:
            // 暂时使用默认64KB条带大小
            ctx->rc = spdk_bdev_raid_create_raid5(ctx->name, 64 * 1024, ctx->num_base_bdevs, 0);
            break;
            
        case XBDEV_MD_LEVEL_RAID10:
            // 暂时使用默认64KB条带大小
            ctx->rc = spdk_bdev_raid_create_raid10(ctx->name, 64 * 1024, ctx->num_base_bdevs);
            break;
            
        default:
            XBDEV_ERRLOG("RAID组装失败: 不支持的RAID级别 %d\n", ctx->level);
            ctx->rc = -ENOTSUP;
            ctx->done = true;
            return;
    }
    
    if (ctx->rc != 0) {
        XBDEV_ERRLOG("RAID组装失败: 创建RAID设备失败 %s, rc=%d\n", ctx->name, ctx->rc);
        ctx->done = true;
        return;
    }
    
    // 添加基础设备
    for (int i = 0; i < ctx->num_base_bdevs; i++) {
        ctx->rc = spdk_bdev_raid_add_base_device(ctx->name, ctx->base_bdevs[i], i);
        if (ctx->rc != 0) {
            XBDEV_ERRLOG("RAID组装失败: 添加设备失败 %s, rc=%d\n", ctx->base_bdevs[i], ctx->rc);
            
            // 清理已添加的设备
            for (int j = 0; j < i; j++) {
                spdk_bdev_raid_remove_base_device(ctx->name, j);
            }
            
            // 删除RAID设备
            spdk_bdev_raid_delete(ctx->name);
            
            ctx->done = true;
            return;
        }
    }
    
    // 获取分配的RAID设备槽
    pthread_mutex_lock(&g_md_mutex);
    raid_idx = find_free_md_slot();
    if (raid_idx < 0) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("RAID组装失败: 无可用RAID设备槽\n");
        
        // 清理已创建的RAID设备
        for (int i = 0; i < ctx->num_base_bdevs; i++) {
            spdk_bdev_raid_remove_base_device(ctx->name, i);
        }
        spdk_bdev_raid_delete(ctx->name);
        
        ctx->rc = -ENOSPC;
        ctx->done = true;
        return;
    }
    
    // 填充RAID设备信息
    xbdev_md_dev_t *md_dev = &g_md_devices[raid_idx];
    pthread_mutex_lock(&md_dev->lock);
    
    md_dev->active = true;
    strncpy(md_dev->name, ctx->name, sizeof(md_dev->name) - 1);
    md_dev->level = ctx->level;
    md_dev->disk_count = ctx->num_base_bdevs;
    md_dev->chunk_size_kb = 64; // 默认64KB
    md_dev->layout = 0; // 默认布局
    strncpy(md_dev->uuid_str, uuid_str, sizeof(md_dev->uuid_str) - 1);
    md_dev->rebuild_active = false;
    md_dev->rebuild_progress = 0.0;
    md_dev->failed_disk_count = 0;
    md_dev->spare_disk_count = 0;
    md_dev->degraded = false;
    
    // 分配并填充磁盘名称数组
    md_dev->disk_names = calloc(ctx->num_base_bdevs, sizeof(char *));
    md_dev->disk_state = calloc(ctx->num_base_bdevs, sizeof(bool));
    if (!md_dev->disk_names || !md_dev->disk_state) {
        pthread_mutex_unlock(&md_dev->lock);
        pthread_mutex_unlock(&g_md_mutex);
        
        // 清理RAID设备
        for (int i = 0; i < ctx->num_base_bdevs; i++) {
            spdk_bdev_raid_remove_base_device(ctx->name, i);
        }
        spdk_bdev_raid_delete(ctx->name);
        
        if (md_dev->disk_names) free(md_dev->disk_names);
        if (md_dev->disk_state) free(md_dev->disk_state);
        md_dev->disk_names = NULL;
        md_dev->disk_state = NULL;
        md_dev->active = false;
        
        ctx->rc = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 复制磁盘名称
    for (int i = 0; i < ctx->num_base_bdevs; i++) {
        md_dev->disk_names[i] = strdup(ctx->base_bdevs[i]);
        md_dev->disk_state[i] = true; // 初始状态为正常
    }
    
    // 初始化热备盘
    md_dev->spare_disks = NULL;
    
    pthread_mutex_unlock(&md_dev->lock);
    pthread_mutex_unlock(&g_md_mutex);
    
    XBDEV_NOTICELOG("成功组装RAID设备: %s (级别: %d, 设备数: %d)\n", 
                  ctx->name, ctx->level, ctx->num_base_bdevs);
    
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 组装RAID设备
 * 
 * @param name RAID设备名称
 * @param level RAID级别
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_assemble(const char *name, int level, const char **base_bdevs, int num_base_bdevs)
{
    xbdev_request_t *req;
    struct {
        const char *name;
        int level;
        const char **base_bdevs;
        int num_base_bdevs;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (!name || !base_bdevs || num_base_bdevs < 2) {
        XBDEV_ERRLOG("无效的RAID参数\n");
        return -EINVAL;
    }
    
    // 检查RAID级别有效性
    if (level != XBDEV_MD_LEVEL_RAID0 && level != XBDEV_MD_LEVEL_RAID1 && 
        level != XBDEV_MD_LEVEL_RAID5 && level != XBDEV_MD_LEVEL_RAID10) {
        XBDEV_ERRLOG("不支持的RAID级别: %d\n", level);
        return -EINVAL;
    }
    
    // 检查名称是否已存在
    pthread_mutex_lock(&g_md_mutex);
    if (md_name_exists(name)) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("RAID设备名称已存在: %s\n", name);
        return -EEXIST;
    }
    pthread_mutex_unlock(&g_md_mutex);
    
    // 设置上下文
    ctx.name = name;
    ctx.level = level;
    ctx.base_bdevs = base_bdevs;
    ctx.num_base_bdevs = num_base_bdevs;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型
    req->type = XBDEV_REQ_RAID_ASSEMBLE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("组装RAID设备失败: %s, rc=%d\n", name, rc);
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * 运行RAID设备
 * 
 * @param name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_run(const char *name)
{
    // 对于SPDK来说，RAID设备创建或组装后即处于运行状态
    // 因此这个函数主要是为了与MD接口兼容
    pthread_mutex_lock(&g_md_mutex);
    int raid_idx = find_md_by_name(name);
    if (raid_idx < 0) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("找不到RAID设备: %s\n", name);
        return -ENOENT;
    }
    pthread_mutex_unlock(&g_md_mutex);
    
    return 0;
}

/**
 * 检查磁盘是否为RAID成员
 * 在SPDK线程中执行
 */
static void xbdev_md_examine_on_thread(void *arg)
{
    struct {
        const char *bdev_name;
        xbdev_md_info_t *info;
        int rc;
        bool done;
    } *ctx = arg;
    
    struct spdk_bdev *bdev;
    
    // 参数检查
    if (!ctx->bdev_name || !ctx->info) {
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取BDEV
    bdev = spdk_bdev_get_by_name(ctx->bdev_name);
    if (!bdev) {
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 清空信息结构体
    memset(ctx->info, 0, sizeof(xbdev_md_info_t));
    
    // 检查该设备是否为RAID设备
    if (spdk_bdev_is_raid(bdev)) {
        ctx->info->is_raid = true;
        strncpy(ctx->info->raid_name, ctx->bdev_name, sizeof(ctx->info->raid_name) - 1);
        
        // 尝试获取RAID级别和相关信息
        // 注意: SPDK当前API不直接提供RAID级别信息，尝试从名称中推断
        const char *product_name = spdk_bdev_get_product_name(bdev);
        if (strstr(product_name, "RAID0")) {
            ctx->info->level = XBDEV_MD_LEVEL_RAID0;
        } else if (strstr(product_name, "RAID1")) {
            ctx->info->level = XBDEV_MD_LEVEL_RAID1;
        } else if (strstr(product_name, "RAID5")) {
            ctx->info->level = XBDEV_MD_LEVEL_RAID5;
        } else if (strstr(product_name, "RAID10")) {
            ctx->info->level = XBDEV_MD_LEVEL_RAID10;
        } else {
            ctx->info->level = -1; // 未知RAID级别
        }
    } else {
        // 检查该设备是否为某个RAID的成员
        pthread_mutex_lock(&g_md_mutex);
        bool is_member = false;
        
        for (int i = 0; i < XBDEV_MD_MAX_RAIDS; i++) {
            if (!g_md_devices[i].active) continue;
            
            xbdev_md_dev_t *md_dev = &g_md_devices[i];
            pthread_mutex_lock(&md_dev->lock);
            
            for (int j = 0; j < md_dev->disk_count; j++) {
                if (md_dev->disk_names[j] && strcmp(md_dev->disk_names[j], ctx->bdev_name) == 0) {
                    // 找到了成员关系
                    is_member = true;
                    ctx->info->is_raid_member = true;
                    strncpy(ctx->info->raid_name, md_dev->name, sizeof(ctx->info->raid_name) - 1);
                    ctx->info->level = md_dev->level;
                    ctx->info->member_index = j;
                    ctx->info->is_failed = !md_dev->disk_state[j];
                    break;
                }
            }
            
            // 也检查是否为热备盘
            if (!is_member && md_dev->spare_disks) {
                for (int j = 0; j < md_dev->spare_disk_count; j++) {
                    if (md_dev->spare_disks[j] && strcmp(md_dev->spare_disks[j], ctx->bdev_name) == 0) {
                        // 找到了热备盘关系
                        is_member = true;
                        ctx->info->is_raid_member = true;
                        ctx->info->is_spare = true;
                        strncpy(ctx->info->raid_name, md_dev->name, sizeof(ctx->info->raid_name) - 1);
                        ctx->info->level = md_dev->level;
                        break;
                    }
                }
            }
            
            pthread_mutex_unlock(&md_dev->lock);
            if (is_member) break;
        }
        
        pthread_mutex_unlock(&g_md_mutex);
    }
    
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 检查设备的RAID信息
 * 
 * @param bdev_name 设备名称
 * @param info 输出参数，存储设备的RAID信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_examine(const char *bdev_name, xbdev_md_info_t *info)
{
    xbdev_request_t *req;
    struct {
        const char *bdev_name;
        xbdev_md_info_t *info;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (!bdev_name || !info) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.bdev_name = bdev_name;
    ctx.info = info;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型
    req->type = XBDEV_REQ_RAID_EXAMINE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * 获取RAID设备详细信息
 * 在SPDK线程中执行
 */
static void xbdev_md_detail_on_thread(void *arg)
{
    struct {
        const char *md_name;
        xbdev_md_detail_t *detail;
        int rc;
        bool done;
    } *ctx = arg;
    
    struct spdk_bdev *bdev;
    int raid_idx;
    xbdev_md_dev_t *md_dev;
    
    // 参数检查
    if (!ctx->md_name || !ctx->detail) {
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取RAID设备
    bdev = spdk_bdev_get_by_name(ctx->md_name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到RAID设备: %s\n", ctx->md_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    // 查找MD设备记录
    pthread_mutex_lock(&g_md_mutex);
    raid_idx = find_md_by_name(ctx->md_name);
    if (raid_idx < 0) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("找不到RAID设备记录: %s\n", ctx->md_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    md_dev = &g_md_devices[raid_idx];
    pthread_mutex_lock(&md_dev->lock);
    pthread_mutex_unlock(&g_md_mutex);
    
    // 填充详细信息
    ctx->detail->level = md_dev->level;
    ctx->detail->raid_disks = md_dev->disk_count;
    
    // 计算活动磁盘和工作磁盘数量
    ctx->detail->active_disks = 0;
    ctx->detail->working_disks = 0;
    ctx->detail->failed_disks = 0;
    
    for (int i = 0; i < md_dev->disk_count; i++) {
        if (md_dev->disk_state[i]) {
            ctx->detail->active_disks++;
            ctx->detail->working_disks++;
        } else {
            ctx->detail->failed_disks++;
        }
    }
    
    ctx->detail->spare_disks = md_dev->spare_disk_count;
    
    // 获取设备大小
    ctx->detail->size = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev) / 1024;
    ctx->detail->chunk_size = md_dev->chunk_size_kb;
    
    // 获取状态
    if (md_dev->rebuild_active) {
        ctx->detail->state = md_dev->degraded ? 
            XBDEV_MD_STATE_RESYNCING : XBDEV_MD_STATE_RECOVERING;
            
        snprintf(ctx->detail->resync_status, sizeof(ctx->detail->resync_status),
                 "resyncing");
        ctx->detail->resync_progress = md_dev->rebuild_progress * 100.0;
        ctx->detail->resync_speed = md_dev->rebuild_speed_kb;
    } else if (md_dev->degraded) {
        ctx->detail->state = XBDEV_MD_STATE_DEGRADED;
    } else {
        ctx->detail->state = XBDEV_MD_STATE_ACTIVE;
    }
    
    // 填充UUID
    strncpy(ctx->detail->uuid, md_dev->uuid_str, sizeof(ctx->detail->uuid) - 1);
    ctx->detail->degraded = md_dev->degraded;
    
    // 填充磁盘信息
    for (int i = 0; i < md_dev->disk_count && i < 32; i++) {
        ctx->detail->disks[i].number = i;
        strncpy(ctx->detail->disks[i].name, md_dev->disk_names[i], 
                sizeof(ctx->detail->disks[i].name) - 1);
        
        ctx->detail->disks[i].state = md_dev->disk_state[i] ? 
            XBDEV_MD_DISK_ACTIVE : XBDEV_MD_DISK_FAULTY;
            
        // 获取磁盘大小
        struct spdk_bdev *disk_bdev = spdk_bdev_get_by_name(md_dev->disk_names[i]);
        if (disk_bdev) {
            ctx->detail->disks[i].size = 
                spdk_bdev_get_num_blocks(disk_bdev) * spdk_bdev_get_block_size(disk_bdev) / 1024;
        }
        
        // 设置角色
        if (md_dev->level == XBDEV_MD_LEVEL_RAID5 && i == md_dev->disk_count - 1) {
            strcpy(ctx->detail->disks[i].role, "parity");
        } else if (md_dev->level == XBDEV_MD_LEVEL_RAID6 && 
                 (i == md_dev->disk_count - 1 || i == md_dev->disk_count - 2)) {
            strcpy(ctx->detail->disks[i].role, "parity");
        } else {
            strcpy(ctx->detail->disks[i].role, "data");
        }
    }
    
    pthread_mutex_unlock(&md_dev->lock);
    
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 获取RAID设备详细信息
 * 
 * @param md_name RAID设备名称
 * @param detail 输出参数，存储RAID详细信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_detail(const char *md_name, xbdev_md_detail_t *detail)
{
    xbdev_request_t *req;
    struct {
        const char *md_name;
        xbdev_md_detail_t *detail;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (!md_name || !detail) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.md_name = md_name;
    ctx.detail = detail;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型
    req->type = XBDEV_REQ_RAID_DETAIL;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("获取RAID详细信息失败: %s, rc=%d\n", md_name, rc);
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * RAID设备管理
 * 在SPDK线程中执行
 */
static void xbdev_md_manage_on_thread(void *arg)
{
    struct {
        const char *md_name;
        int cmd;
        void *cmd_arg;
        int rc;
        bool done;
    } *ctx = arg;
    
    int raid_idx;
    xbdev_md_dev_t *md_dev;
    
    // 参数检查
    if (!ctx->md_name) {
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 查找RAID设备
    pthread_mutex_lock(&g_md_mutex);
    raid_idx = find_md_by_name(ctx->md_name);
    if (raid_idx < 0) {
        pthread_mutex_unlock(&g_md_mutex);
        XBDEV_ERRLOG("找不到RAID设备: %s\n", ctx->md_name);
        ctx->rc = -ENOENT;
        ctx->done = true;
        return;
    }
    
    md_dev = &g_md_devices[raid_idx];
    pthread_mutex_lock(&md_dev->lock);
    pthread_mutex_unlock(&g_md_mutex);
    
    // 根据命令类型执行不同操作
    switch (ctx->cmd) {
        case XBDEV_MD_ADD_SPARE: {
            // 添加热备盘
            const char *disk_name = (const char *)ctx->cmd_arg;
            if (!disk_name) {
                ctx->rc = -EINVAL;
                break;
            }
            
            // 检查设备是否存在
            if (!spdk_bdev_get_by_name(disk_name)) {
                ctx->rc = -ENODEV;
                XBDEV_ERRLOG("添加热备盘失败: 找不到设备 %s\n", disk_name);
                break;
            }
            
            // 检查设备是否已经是该RAID的成员或热备盘
            bool already_member = false;
            for (int i = 0; i < md_dev->disk_count; i++) {
                if (md_dev->disk_names[i] && strcmp(md_dev->disk_names[i], disk_name) == 0) {
                    already_member = true;
                    break;
                }
            }
            
            if (already_member) {
                ctx->rc = -EEXIST;
                XBDEV_ERRLOG("添加热备盘失败: 设备已经是RAID成员 %s\n", disk_name);
                break;
            }
            
            for (int i = 0; i < md_dev->spare_disk_count; i++) {
                if (md_dev->spare_disks[i] && strcmp(md_dev->spare_disks[i], disk_name) == 0) {
                    already_member = true;
                    break;
                }
            }
            
            if (already_member) {
                ctx->rc = -EEXIST;
                XBDEV_ERRLOG("添加热备盘失败: 设备已经是热备盘 %s\n", disk_name);
                break;
            }
            
            // 添加热备盘
            char **new_spare_disks = realloc(md_dev->spare_disks, 
                                             (md_dev->spare_disk_count + 1) * sizeof(char *));
            if (!new_spare_disks) {
                ctx->rc = -ENOMEM;
                break;
            }
            
            md_dev->spare_disks = new_spare_disks;
            md_dev->spare_disks[md_dev->spare_disk_count] = strdup(disk_name);
            if (!md_dev->spare_disks[md_dev->spare_disk_count]) {
                ctx->rc = -ENOMEM;
                break;
            }
            
            md_dev->spare_disk_count++;
            ctx->rc = 0;
            break;
        }
        
        case XBDEV_MD_REMOVE_SPARE: {
            // 移除热备盘
            const char *disk_name = (const char *)ctx->cmd_arg;
            if (!disk_name) {
                ctx->rc = -EINVAL;
                break;
            }
            
            // 查找热备盘
            int spare_idx = -1;
            for (int i = 0; i < md_dev->spare_disk_count; i++) {
                if (md_dev->spare_disks[i] && strcmp(md_dev->spare_disks[i], disk_name) == 0) {
                    spare_idx = i;
                    break;
                }
            }
            
            if (spare_idx < 0) {
                ctx->rc = -ENOENT;
                XBDEV_ERRLOG("移除热备盘失败: 找不到热备盘 %s\n", disk_name);
                break;
            }
            
            // 移除热备盘
            free(md_dev->spare_disks[spare_idx]);
            for (int i = spare_idx; i < md_dev->spare_disk_count - 1; i++) {
                md_dev->spare_disks[i] = md_dev->spare_disks[i + 1];
            }
            
            md_dev->spare_disk_count--;
            if (md_dev->spare_disk_count == 0) {
                free(md_dev->spare_disks);
                md_dev->spare_disks = NULL;
            } else {
                char **new_spare_disks = realloc(md_dev->spare_disks, 
                                                 md_dev->spare_disk_count * sizeof(char *));
                if (new_spare_disks) {
                    md_dev->spare_disks = new_spare_disks;
                }
            }
            
            ctx->rc = 0;
            break;
        }
        
        case XBDEV_MD_FAIL_DISK: {
            // 标记磁盘为故障
            int disk_idx = *(int *)ctx->cmd_arg;
            if (disk_idx < 0 || disk_idx >= md_dev->disk_count) {
                ctx->rc = -EINVAL;
                break;
            }
            
            if (!md_dev->disk_state[disk_idx]) {
                ctx->rc = -EALREADY;
                break;
            }
            
            md_dev->disk_state[disk_idx] = false;
            md_dev->failed_disk_count++;
            md_dev->degraded = true;
            ctx->rc = 0;
            break;
        }
        
        case XBDEV_MD_REBUILD: {
            // 启动重建
            if (md_dev->rebuild_active) {
                ctx->rc = -EALREADY;
                break;
            }
            
            // 查找热备盘
            if (md_dev->spare_disk_count == 0) {
                ctx->rc = -ENOSPC;
                break;
            }
            
            // 启动重建过程
            md_dev->rebuild_active = true;
            md_dev->rebuild_progress = 0.0;
            md_dev->rebuild_speed_kb = 0;
            ctx->rc = 0;
            break;
        }
        
        default:
            ctx->rc = -EINVAL;
            break;
    }
    
    pthread_mutex_unlock(&md_dev->lock);
    ctx->done = true;
}

/**
 * RAID设备管理
 * 
 * @param md_name RAID设备名称
 * @param cmd 管理命令
 * @param cmd_arg 命令参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_manage(const char *md_name, int cmd, void *cmd_arg)
{
    xbdev_request_t *req;
    struct {
        const char *md_name;
        int cmd;
        void *cmd_arg;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (!md_name) {
        XBDEV_ERRLOG("无效的RAID名称\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.md_name = md_name;
    ctx.cmd = cmd;
    ctx.cmd_arg = cmd_arg;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型
    req->type = XBDEV_REQ_RAID_MANAGE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("RAID设备管理失败: %s, rc=%d\n", md_name, rc);
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}