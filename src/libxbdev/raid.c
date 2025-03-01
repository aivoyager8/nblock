/**
 * @file raid.c
 * @brief RAID设备管理实现
 *
 * 该文件提供了RAID设备的创建、管理和操作功能，包括RAID0、RAID1和RAID5的实现。
 * 支持RAID设备的组装、停止、监控和故障恢复等高级功能。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/bdev.h>
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/raid.h>
#include <spdk/string.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

/**
 * RAID上下文结构
 */
struct raid_ctx {
    char name[256];                   // RAID设备名称 
    int level;                        // RAID级别
    const char **base_bdevs;          // 基础设备名称数组
    int num_base_bdevs;               // 基础设备数量
    xbdev_md_config_t *config;        // RAID配置
    int rc;                           // 返回值
    bool done;                        // 是否完成
};

/**
 * RAID详细信息上下文
 */
struct raid_detail_ctx {
    const char *md_name;              // RAID设备名称
    xbdev_md_detail_t *detail;        // RAID详细信息
    int rc;                           // 返回码
    bool done;                        // 是否完成
};

/**
 * RAID磁盘管理上下文
 */
struct raid_disk_mgmt_ctx {
    const char *md_name;              // RAID设备名称
    int cmd;                          // 命令类型
    void *arg;                        // 命令参数
    int rc;                           // 返回码
    bool done;                        // 是否完成
};

/**
 * RAID事件回调项
 */
struct raid_event_cb_entry {
    xbdev_md_event_cb cb;             // 回调函数
    void *ctx;                        // 回调上下文
    struct raid_event_cb_entry *next; // 下一项
};

// RAID事件回调链表
static struct raid_event_cb_entry *g_raid_event_cb_list = NULL;
static pthread_mutex_t g_raid_cb_mutex = PTHREAD_MUTEX_INITIALIZER;

// RAID监控定时器
static void *g_raid_monitor_timer = NULL;
static bool g_raid_monitor_active = false;

/**
 * 在SPDK线程上下文中创建RAID设备
 */
static void raid_create_on_thread(void *arg)
{
    struct raid_ctx *ctx = arg;
    struct spdk_bdev **base_bdevs = NULL;
    struct spdk_bdev_raid_opts raid_opts = {};
    int i, rc;
    
    // 分配基础设备数组
    base_bdevs = calloc(ctx->num_base_bdevs, sizeof(struct spdk_bdev *));
    if (!base_bdevs) {
        ctx->rc = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 查找所有基础设备
    for (i = 0; i < ctx->num_base_bdevs; i++) {
        base_bdevs[i] = spdk_bdev_get_by_name(ctx->base_bdevs[i]);
        if (!base_bdevs[i]) {
            XBDEV_ERRLOG("找不到基础设备: %s\n", ctx->base_bdevs[i]);
            free(base_bdevs);
            ctx->rc = -ENODEV;
            ctx->done = true;
            return;
        }
    }
    
    // 设置RAID选项
    raid_opts.name = ctx->name;
    
    // 根据RAID级别设置相应的参数
    switch (ctx->level) {
        case XBDEV_MD_LEVEL_LINEAR:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_CONCAT;
            raid_opts.strip_size_kb = 0;  // 不适用于线性RAID
            break;
            
        case XBDEV_MD_LEVEL_RAID0:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_0;
            raid_opts.strip_size_kb = ctx->config ? ctx->config->chunk_size_kb : 64;
            break;
            
        case XBDEV_MD_LEVEL_RAID1:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_1;
            raid_opts.strip_size_kb = 0;  // 不适用于RAID1
            break;
            
        case XBDEV_MD_LEVEL_RAID5:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_5;
            raid_opts.strip_size_kb = ctx->config ? ctx->config->chunk_size_kb : 64;
            break;
            
        default:
            XBDEV_ERRLOG("不支持的RAID级别: %d\n", ctx->level);
            free(base_bdevs);
            ctx->rc = -EINVAL;
            ctx->done = true;
            return;
    }
    
    // 设置对齐信息
    raid_opts.alignment = 4096;
    raid_opts.metadata_size = 0;
    
    // 构造回调参数
    struct {
        int *rc;
        bool *done;
    } cb_arg = {
        .rc = &ctx->rc,
        .done = &ctx->done
    };
    
    // 创建RAID设备
    XBDEV_NOTICELOG("创建RAID设备: %s, 级别=%d, 设备数=%d, 块大小=%lu KB\n",
                  ctx->name, ctx->level, ctx->num_base_bdevs, raid_opts.strip_size_kb);
    
    rc = spdk_bdev_raid_create(&raid_opts, base_bdevs, ctx->num_base_bdevs,
                             raid_create_complete, &cb_arg);
    if (rc != 0) {
        XBDEV_ERRLOG("无法创建RAID设备: %s, rc=%d\n", ctx->name, rc);
        free(base_bdevs);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    // 等待完成回调
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
    
    free(base_bdevs);
}

/**
 * RAID创建回调函数
 */
static void raid_create_complete(struct spdk_bdev *bdev, void *_ctx, int status)
{
    struct {
        int *rc;
        bool *done;
    } *ctx = _ctx;
    
    // 设置返回值和完成标志
    *ctx->rc = status;
    *ctx->done = true;
    
    if (status != 0) {
        XBDEV_ERRLOG("RAID设备创建失败，rc=%d\n", status);
    } else {
        XBDEV_NOTICELOG("RAID设备创建成功: %s\n", spdk_bdev_get_name(bdev));
    }
}

/**
 * 在SPDK线程上下文中组装已有的RAID设备
 */
static void raid_assemble_on_thread(void *arg)
{
    struct raid_ctx *ctx = arg;
    struct spdk_bdev **base_bdevs = NULL;
    struct spdk_bdev_raid_opts raid_opts = {};
    int i, rc;
    
    // 分配基础设备数组
    base_bdevs = calloc(ctx->num_base_bdevs, sizeof(struct spdk_bdev *));
    if (!base_bdevs) {
        ctx->rc = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 查找所有基础设备
    for (i = 0; i < ctx->num_base_bdevs; i++) {
        base_bdevs[i] = spdk_bdev_get_by_name(ctx->base_bdevs[i]);
        if (!base_bdevs[i]) {
            XBDEV_ERRLOG("找不到基础设备: %s\n", ctx->base_bdevs[i]);
            free(base_bdevs);
            ctx->rc = -ENODEV;
            ctx->done = true;
            return;
        }
    }
    
    // 设置RAID选项
    raid_opts.name = ctx->name;
    
    // 根据RAID级别设置相应的参数
    switch (ctx->level) {
        case XBDEV_MD_LEVEL_LINEAR:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_CONCAT;
            break;
            
        case XBDEV_MD_LEVEL_RAID0:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_0;
            break;
            
        case XBDEV_MD_LEVEL_RAID1:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_1;
            break;
            
        case XBDEV_MD_LEVEL_RAID5:
            raid_opts.level = SPDK_BDEV_RAID_LEVEL_5;
            break;
            
        default:
            XBDEV_ERRLOG("不支持的RAID级别: %d\n", ctx->level);
            free(base_bdevs);
            ctx->rc = -EINVAL;
            ctx->done = true;
            return;
    }
    
    // 设置RAID额外选项
    raid_opts.alignment = 4096;
    raid_opts.metadata_size = 0;
    raid_opts.assume_clean = ctx->config ? ctx->config->assume_clean : false;
    
    // 设置RAID布局 (主要用于RAID5/6)
    if (ctx->level == XBDEV_MD_LEVEL_RAID5 && ctx->config) {
        raid_opts.raid5.layout = ctx->config->layout;
    }
    
    // 构造回调参数
    struct {
        int *rc;
        bool *done;
    } cb_arg = {
        .rc = &ctx->rc,
        .done = &ctx->done
    };
    
    // 组装RAID设备
    XBDEV_NOTICELOG("组装RAID设备: %s, 级别=%d, 设备数=%d\n",
                  ctx->name, ctx->level, ctx->num_base_bdevs);
    
    rc = spdk_bdev_raid_create(&raid_opts, base_bdevs, ctx->num_base_bdevs,
                             raid_create_complete, &cb_arg);
    if (rc != 0) {
        XBDEV_ERRLOG("无法组装RAID设备: %s, rc=%d\n", ctx->name, rc);
        free(base_bdevs);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    // 等待完成回调
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
    
    free(base_bdevs);
}

/**
 * 在SPDK线程上下文中停止RAID设备
 */
static void raid_stop_on_thread(void *arg)
{
    struct {
        const char *name;
        int rc;
        bool done;
    } *ctx = arg;
    
    struct spdk_bdev *bdev;
    int rc;
    
    // 查找RAID设备
    bdev = spdk_bdev_get_by_name(ctx->name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到RAID设备: %s\n", ctx->name);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 检查是否为RAID设备
    if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") != 0) {
        XBDEV_ERRLOG("%s 不是RAID设备\n", ctx->name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        int *rc;
        bool *done;
    } cb_arg = {
        .rc = &ctx->rc,
        .done = &ctx->done
    };
    
    // 停止RAID设备
    XBDEV_NOTICELOG("停止RAID设备: %s\n", ctx->name);
    
    rc = spdk_bdev_raid_remove(ctx->name, raid_remove_cb, &cb_arg);
    if (rc != 0) {
        XBDEV_ERRLOG("无法停止RAID设备: %s, rc=%d\n", ctx->name, rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    // 等待完成回调
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * RAID设备移除回调
 */
static void raid_remove_cb(void *cb_arg, int status)
{
    struct {
        int *rc;
        bool *done;
    } *ctx = cb_arg;
    
    // 设置返回值和完成标志
    *ctx->rc = status;
    *ctx->done = true;
    
    if (status != 0) {
        XBDEV_ERRLOG("RAID设备停止失败，rc=%d\n", status);
    } else {
        XBDEV_NOTICELOG("RAID设备停止成功\n");
    }
}

/**
 * 在SPDK线程上下文中获取RAID设备详细信息
 */
static void raid_detail_on_thread(void *arg)
{
    struct raid_detail_ctx *ctx = arg;
    struct spdk_bdev *bdev;
    struct spdk_bdev_raid_bdev *raid_bdev;
    
    // 查找RAID设备
    bdev = spdk_bdev_get_by_name(ctx->md_name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到RAID设备: %s\n", ctx->md_name);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 检查是否为RAID设备
    if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") != 0) {
        XBDEV_ERRLOG("%s 不是RAID设备\n", ctx->md_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取RAID设备指针
    raid_bdev = spdk_bdev_get_raid_bdev(bdev);
    if (!raid_bdev) {
        XBDEV_ERRLOG("无法获取RAID元数据: %s\n", ctx->md_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 初始化详细信息结构
    memset(ctx->detail, 0, sizeof(xbdev_md_detail_t));
    
    // 获取RAID级别
    int raid_level = spdk_bdev_raid_get_level(raid_bdev);
    switch (raid_level) {
        case SPDK_BDEV_RAID_LEVEL_0:
            ctx->detail->level = XBDEV_MD_LEVEL_RAID0;
            break;
            
        case SPDK_BDEV_RAID_LEVEL_1:
            ctx->detail->level = XBDEV_MD_LEVEL_RAID1;
            break;
            
        case SPDK_BDEV_RAID_LEVEL_5:
            ctx->detail->level = XBDEV_MD_LEVEL_RAID5;
            break;
            
        case SPDK_BDEV_RAID_LEVEL_CONCAT:
            ctx->detail->level = XBDEV_MD_LEVEL_LINEAR;
            break;
            
        default:
            ctx->detail->level = raid_level;
    }
    
    // 获取RAID基本信息
    ctx->detail->raid_disks = spdk_bdev_raid_get_num_base_bdevs(raid_bdev);
    ctx->detail->active_disks = 0; // 将在下面统计
    ctx->detail->working_disks = 0; // 将在下面统计
    ctx->detail->failed_disks = 0; // 将在下面统计
    ctx->detail->spare_disks = 0; // 将在下面统计
    
    // 获取RAID设备大小
    ctx->detail->size = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev) / 1024;
    
    // 获取条带大小
    ctx->detail->chunk_size = spdk_bdev_raid_get_strip_size(raid_bdev) / 1024;
    
    // 获取RAID状态
    bool degraded = spdk_bdev_raid_is_degraded(raid_bdev);
    ctx->detail->degraded = degraded;
    
    if (degraded) {
        ctx->detail->state = XBDEV_MD_STATE_DEGRADED;
    } else {
        ctx->detail->state = XBDEV_MD_STATE_ACTIVE;
    }
    
    // 获取同步状态和进度（简化处理）
    if (spdk_bdev_raid_is_resynchronizing(raid_bdev)) {
        ctx->detail->state = XBDEV_MD_STATE_RESYNCING;
        strcpy(ctx->detail->resync_status, "resyncing");
        ctx->detail->resync_progress = spdk_bdev_raid_get_resync_percent(raid_bdev);
        ctx->detail->resync_speed = spdk_bdev_raid_get_resync_speed(raid_bdev) / 1024;
    } else {
        strcpy(ctx->detail->resync_status, "idle");
        ctx->detail->resync_progress = 100.0;
        ctx->detail->resync_speed = 0;
    }
    
    // 生成UUID（简化处理，实际应从元数据获取）
    snprintf(ctx->detail->uuid, sizeof(ctx->detail->uuid), 
             "raid-%s-%08x-%04x-%04x", ctx->md_name, 
             (unsigned int)time(NULL), rand() % 0xFFFF, rand() % 0xFFFF);
    
    // 获取成员磁盘信息
    int max_disks = ctx->detail->raid_disks;
    if (max_disks > 32) max_disks = 32; // 限制最大磁盘数
    
    for (int i = 0; i < max_disks; i++) {
        struct spdk_bdev *base_bdev = spdk_bdev_raid_get_base_bdev(raid_bdev, i);
        if (!base_bdev) {
            ctx->detail->disks[i].state = XBDEV_MD_DISK_REMOVED;
            continue;
        }
        
        // 填充磁盘信息
        ctx->detail->disks[i].number = i;
        strncpy(ctx->detail->disks[i].name, spdk_bdev_get_name(base_bdev), 
                sizeof(ctx->detail->disks[i].name) - 1);
        
        // 获取磁盘状态
        bool disk_online = spdk_bdev_raid_base_bdev_is_online(raid_bdev, i);
        if (disk_online) {
            ctx->detail->disks[i].state = XBDEV_MD_DISK_ACTIVE;
            ctx->detail->active_disks++;
            ctx->detail->working_disks++;
            strncpy(ctx->detail->disks[i].role, "active", sizeof(ctx->detail->disks[i].role) - 1);
        } else {
            ctx->detail->disks[i].state = XBDEV_MD_DISK_FAULTY;
            ctx->detail->failed_disks++;
            strncpy(ctx->detail->disks[i].role, "faulty", sizeof(ctx->detail->disks[i].role) - 1);
        }
        
        // 获取磁盘大小
        ctx->detail->disks[i].size = spdk_bdev_get_num_blocks(base_bdev) * 
                                     spdk_bdev_get_block_size(base_bdev) / 1024;
    }
    
    // 设置完成标志
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 在SPDK线程上下文中执行RAID磁盘管理命令
 */
static void raid_disk_mgmt_on_thread(void *arg)
{
    struct raid_disk_mgmt_ctx *ctx = arg;
    struct spdk_bdev *bdev;
    struct spdk_bdev_raid_bdev *raid_bdev;
    int rc = 0;
    
    // 查找RAID设备
    bdev = spdk_bdev_get_by_name(ctx->md_name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到RAID设备: %s\n", ctx->md_name);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 检查是否为RAID设备
    if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") != 0) {
        XBDEV_ERRLOG("%s 不是RAID设备\n", ctx->md_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取RAID设备指针
    raid_bdev = spdk_bdev_get_raid_bdev(bdev);
    if (!raid_bdev) {
        XBDEV_ERRLOG("无法获取RAID元数据: %s\n", ctx->md_name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 根据命令类型执行相应操作
    switch (ctx->cmd) {
        case XBDEV_MD_ADD_DISK: {
            const char *disk_name = (const char *)ctx->arg;
            struct spdk_bdev *disk_bdev = spdk_bdev_get_by_name(disk_name);
            if (!disk_bdev) {
                XBDEV_ERRLOG("找不到磁盘: %s\n", disk_name);
                ctx->rc = -ENODEV;
                break;
            }
            
            // 构造回调参数
            struct {
                int *rc;
                bool *done;
            } cb_arg = {
                .rc = &ctx->rc,
                .done = &ctx->done
            };
            
            // 添加磁盘
            rc = spdk_bdev_raid_add_base_device(raid_bdev, disk_bdev, raid_disk_op_cb, &cb_arg);
            if (rc != 0) {
                XBDEV_ERRLOG("添加磁盘失败: %s, rc=%d\n", disk_name, rc);
                ctx->rc = rc;
                ctx->done = true;
            }
            // 如果成功，等待回调
            break;
        }
        
        case XBDEV_MD_REMOVE_DISK: {
            const char *disk_name = (const char *)ctx->arg;
            int disk_idx = -1;
            
            // 查找磁盘索引
            int num_base_bdevs = spdk_bdev_raid_get_num_base_bdevs(raid_bdev);
            for (int i = 0; i < num_base_bdevs; i++) {
                struct spdk_bdev *base_bdev = spdk_bdev_raid_get_base_bdev(raid_bdev, i);
                if (base_bdev && strcmp(spdk_bdev_get_name(base_bdev), disk_name) == 0) {
                    disk_idx = i;
                    break;
                }
            }
            
            if (disk_idx < 0) {
                XBDEV_ERRLOG("在RAID设备 %s 中找不到磁盘 %s\n", ctx->md_name, disk_name);
                ctx->rc = -ENODEV;
                ctx->done = true;
                break;
            }
            
            // 构造回调参数
            struct {
                int *rc;
                bool *done;
            } cb_arg = {
                .rc = &ctx->rc,
                .done = &ctx->done
            };
            
            // 移除磁盘
            rc = spdk_bdev_raid_remove_base_device(raid_bdev, disk_idx, raid_disk_op_cb, &cb_arg);
            if (rc != 0) {
                XBDEV_ERRLOG("移除磁盘失败: %s (索引 %d), rc=%d\n", disk_name, disk_idx, rc);
                ctx->rc = rc;
                ctx->done = true;
            }
            // 如果成功，等待回调
            break;
        }
        
        case XBDEV_MD_SET_FAULTY: {
            const char *disk_name = (const char *)ctx->arg;
            int disk_idx = -1;
            
            // 查找磁盘索引
            int num_base_bdevs = spdk_bdev_raid_get_num_base_bdevs(raid_bdev);
            for (int i = 0; i < num_base_bdevs; i++) {
                struct spdk_bdev *base_bdev = spdk_bdev_raid_get_base_bdev(raid_bdev, i);
                if (base_bdev && strcmp(spdk_bdev_get_name(base_bdev), disk_name) == 0) {
                    disk_idx = i;
                    break;
                }
            }
            
            if (disk_idx < 0) {
                XBDEV_ERRLOG("在RAID设备 %s 中找不到磁盘 %s\n", ctx->md_name, disk_name);
                ctx->rc = -ENODEV;
                ctx->done = true;
                break;
            }
            
            // 构造回调参数
            struct {
                int *rc;
                bool *done;
            } cb_arg = {
                .rc = &ctx->rc,
                .done = &ctx->done
            };
            
            // 将磁盘设置为故障状态
            rc = spdk_bdev_raid_set_base_device_state(raid_bdev, disk_idx, false, 
                                                    raid_disk_op_cb, &cb_arg);
            if (rc != 0) {
                XBDEV_ERRLOG("将磁盘设置为故障状态失败: %s (索引 %d), rc=%d\n", 
                           disk_name, disk_idx, rc);
                ctx->rc = rc;
                ctx->done = true;
            }
            // 如果成功，等待回调
            break;
        }
        
        case XBDEV_MD_REPLACE_DISK: {
            struct {
                const char *old_disk;
                const char *new_disk;
            } *replace_info = ctx->arg;
            
            int disk_idx = -1;
            
            // 查找旧磁盘索引
            int num_base_bdevs = spdk_bdev_raid_get_num_base_bdevs(raid_bdev);
            for (int i = 0; i < num_base_bdevs; i++) {
                struct spdk_bdev *base_bdev = spdk_bdev_raid_get_base_bdev(raid_bdev, i);
                if (base_bdev && strcmp(spdk_bdev_get_name(base_bdev), replace_info->old_disk) == 0) {
                    disk_idx = i;
                    break;
                }
            }
            
            if (disk_idx < 0) {
                XBDEV_ERRLOG("在RAID设备 %s 中找不到旧磁盘 %s\n", 
                           ctx->md_name, replace_info->old_disk);
                ctx->rc = -ENODEV;
                ctx->done = true;
                break;
            }
            
            // 获取新磁盘
            struct spdk_bdev *new_bdev = spdk_bdev_get_by_name(replace_info->new_disk);
            if (!new_bdev) {
                XBDEV_ERRLOG("找不到新磁盘: %s\n", replace_info->new_disk);
                ctx->rc = -ENODEV;
                ctx->done = true;
                break;
            }
            
            // 构造回调参数
            struct {
                int *rc;
                bool *done;
            } cb_arg = {
                .rc = &ctx->rc,
                .done = &ctx->done
            };
            
            // 替换磁盘
            rc = spdk_bdev_raid_replace_base_device(raid_bdev, disk_idx, new_bdev, 
                                                  raid_disk_op_cb, &cb_arg);
            if (rc != 0) {
                XBDEV_ERRLOG("替换磁盘失败: %s -> %s, rc=%d\n", 
                    replace_info->old_disk, replace_info->new_disk, rc);
                ctx->rc = rc;
                ctx->done = true;
                break;
            }
            // 如果成功，等待回调
            break;
        }
        
        case XBDEV_MD_ADD_SPARE: {
            // SPDK RAID当前不支持热备盘概念，我们需要模拟实现
            // 这里使用内部数据结构记录热备盘信息
            const char *disk_name = (const char *)ctx->arg;
            struct spdk_bdev *disk_bdev = spdk_bdev_get_by_name(disk_name);
            if (!disk_bdev) {
                XBDEV_ERRLOG("找不到热备盘: %s\n", disk_name);
                ctx->rc = -ENODEV;
                ctx->done = true;
                break;
            }
            
            // 在内部数据结构中记录热备盘信息
            // 这需要额外的热备盘管理功能
            ctx->rc = _xbdev_raid_add_hot_spare(ctx->md_name, disk_name);
            ctx->done = true;
            break;
        }
        
        case XBDEV_MD_SET_SYNC_SPEED: {
            int *speed_kb_per_sec = (int *)ctx->arg;
            
            // 设置同步速度
            rc = spdk_bdev_raid_set_resync_speed(raid_bdev, *speed_kb_per_sec);
            if (rc != 0) {
                XBDEV_ERRLOG("设置同步速度失败: %d KB/s, rc=%d\n", *speed_kb_per_sec, rc);
            } else {
                XBDEV_NOTICELOG("成功设置同步速度: %d KB/s\n", *speed_kb_per_sec);
            }
            
            ctx->rc = rc;
            ctx->done = true;
            break;
        }
        
        case XBDEV_MD_START_REBUILD: {
            // SPDK RAID会自动处理重建，此处仅提供手动触发功能
            rc = spdk_bdev_raid_start_rebuild(raid_bdev);
            if (rc != 0) {
                XBDEV_ERRLOG("启动重建失败: rc=%d\n", rc);
            } else {
                XBDEV_NOTICELOG("已启动RAID设备重建\n");
            }
            
            ctx->rc = rc;
            ctx->done = true;
            break;
        }
        
        case XBDEV_MD_STOP_REBUILD: {
            rc = spdk_bdev_raid_stop_rebuild(raid_bdev);
            if (rc != 0) {
                XBDEV_ERRLOG("停止重建失败: rc=%d\n", rc);
            } else {
                XBDEV_NOTICELOG("已停止RAID设备重建\n");
            }
            
            ctx->rc = rc;
            ctx->done = true;
            break;
        }
        
        default:
            XBDEV_ERRLOG("未知RAID管理命令: %d\n", ctx->cmd);
            ctx->rc = -EINVAL;
            ctx->done = true;
            break;
    }
}

/**
 * RAID磁盘操作回调
 */
static void raid_disk_op_cb(void *cb_arg, int status)
{
    struct {
        int *rc;
        bool *done;
    } *ctx = cb_arg;
    
    *ctx->rc = status;
    *ctx->done = true;
    
    if (status != 0) {
        XBDEV_ERRLOG("RAID磁盘操作失败: %d\n", status);
    } else {
        XBDEV_NOTICELOG("RAID磁盘操作成功完成\n");
    }
}

/**
 * 创建RAID设备
 *
 * @param name 设备名称
 * @param level RAID级别
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param config RAID配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create(const char *name, int level, const char **base_bdevs, int num_base_bdevs, 
                  xbdev_md_config_t *config)
{
    xbdev_request_t *req;
    struct raid_ctx ctx = {0};
    int rc;
    
    if (!name || !base_bdevs || num_base_bdevs <= 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 验证RAID级别
    switch (level) {
        case XBDEV_MD_LEVEL_LINEAR:
        case XBDEV_MD_LEVEL_RAID0:
        case XBDEV_MD_LEVEL_RAID1:
        case XBDEV_MD_LEVEL_RAID5:
            // 支持的RAID级别
            break;
        default:
            XBDEV_ERRLOG("不支持的RAID级别: %d\n", level);
            return -EINVAL;
    }
    
    // 验证磁盘数量是否满足RAID级别要求
    if (level == XBDEV_MD_LEVEL_RAID1 && num_base_bdevs < 2) {
        XBDEV_ERRLOG("RAID1至少需要2个磁盘\n");
        return -EINVAL;
    }
    
    if (level == XBDEV_MD_LEVEL_RAID5 && num_base_bdevs < 3) {
        XBDEV_ERRLOG("RAID5至少需要3个磁盘\n");
        return -EINVAL;
    }
    
    // 设置上下文
    strncpy(ctx.name, name, sizeof(ctx.name) - 1);
    ctx.level = level;
    ctx.base_bdevs = base_bdevs;
    ctx.num_base_bdevs = num_base_bdevs;
    ctx.config = config;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功创建RAID设备: %s, 级别=%d, 设备数=%d\n",
                  name, level, num_base_bdevs);
    
    return 0;
}

/**
 * 创建RAID0设备
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小(KB)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid0(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb)
{
    xbdev_md_config_t config = {0};
    config.chunk_size_kb = chunk_size_kb;
    
    return xbdev_md_create(name, XBDEV_MD_LEVEL_RAID0, base_bdevs, num_base_bdevs, &config);
}

/**
 * 创建RAID1设备
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid1(const char *name, const char **base_bdevs, int num_base_bdevs)
{
    // RAID1不需要块大小参数
    return xbdev_md_create(name, XBDEV_MD_LEVEL_RAID1, base_bdevs, num_base_bdevs, NULL);
}

/**
 * 创建RAID5设备
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小(KB)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid5(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb)
{
    xbdev_md_config_t config = {0};
    config.chunk_size_kb = chunk_size_kb;
    // 默认使用left-symmetric布局
    config.layout = SPDK_RAID5_LEFT_SYMMETRIC;
    
    return xbdev_md_create(name, XBDEV_MD_LEVEL_RAID5, base_bdevs, num_base_bdevs, &config);
}

/**
 * 组装已有的RAID设备
 *
 * @param name 设备名称
 * @param level RAID级别
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_assemble(const char *name, int level, const char **base_bdevs, int num_base_bdevs)
{
    xbdev_request_t *req;
    struct raid_ctx ctx = {0};
    int rc;
    
    if (!name || !base_bdevs || num_base_bdevs <= 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    strncpy(ctx.name, name, sizeof(ctx.name) - 1);
    ctx.level = level;
    ctx.base_bdevs = base_bdevs;
    ctx.num_base_bdevs = num_base_bdevs;
    ctx.done = false;
    
    // 创建配置
    xbdev_md_config_t config = {0};
    config.assume_clean = true;  // 组装时假定设备是干净的
    ctx.config = &config;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功组装RAID设备: %s, 级别=%d, 设备数=%d\n",
                  name, level, num_base_bdevs);
    
    return 0;
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
    
    if (!name) {
        XBDEV_ERRLOG("无效的RAID设备名称\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.name = name;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回操作结果
    if (ctx.rc != 0) {
        return ctx.rc;
    }
    
    XBDEV_NOTICELOG("成功停止RAID设备: %s\n", name);
    
    return 0;
}

/**
 * 获取RAID设备详细信息
 *
 * @param md_name RAID设备名称
 * @param detail RAID详细信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_detail(const char *md_name, xbdev_md_detail_t *detail)
{
    xbdev_request_t *req;
    struct raid_detail_ctx ctx = {0};
    int rc;
    
    if (!md_name || !detail) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.md_name = md_name;
    ctx.detail = detail;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return ctx.rc;
}

/**
 * 对RAID设备执行管理命令
 *
 * @param md_name RAID设备名称
 * @param cmd 命令类型
 * @param arg 命令参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_manage(const char *md_name, int cmd, void *arg)
{
    xbdev_request_t *req;
    struct raid_disk_mgmt_ctx ctx = {0};
    int rc;
    
    if (!md_name) {
        XBDEV_ERRLOG("无效的RAID设备名称\n");
        return -EINVAL;
    }
    
    // 某些命令需要额外检查参数
    if ((cmd == XBDEV_MD_ADD_DISK || cmd == XBDEV_MD_REMOVE_DISK || 
        cmd == XBDEV_MD_ADD_SPARE || cmd == XBDEV_MD_REMOVE_SPARE || 
        cmd == XBDEV_MD_SET_FAULTY) && !arg) {
        XBDEV_ERRLOG("磁盘操作命令需要提供磁盘名称\n");
        return -EINVAL;
    }
    
    if (cmd == XBDEV_MD_REPLACE_DISK && !arg) {
        XBDEV_ERRLOG("替换磁盘命令需要提供新旧磁盘信息\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.md_name = md_name;
    ctx.cmd = cmd;
    ctx.arg = arg;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return ctx.rc;
}

/**
 * 注册RAID事件回调
 *
 * @param cb 回调函数
 * @param ctx 回调上下文
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_register_event_cb(xbdev_md_event_cb cb, void *ctx)
{
    struct raid_event_cb_entry *entry;
    
    if (!cb) {
        XBDEV_ERRLOG("无效的回调函数\n");
        return -EINVAL;
    }
    
    entry = malloc(sizeof(*entry));
    if (!entry) {
        XBDEV_ERRLOG("无法分配内存\n");
        return -ENOMEM;
    }
    
    entry->cb = cb;
    entry->ctx = ctx;
    
    pthread_mutex_lock(&g_raid_cb_mutex);
    
    // 添加到链表头部
    entry->next = g_raid_event_cb_list;
    g_raid_event_cb_list = entry;
    
    pthread_mutex_unlock(&g_raid_cb_mutex);
    
    return 0;
}

/**
 * 取消注册RAID事件回调
 *
 * @param cb 回调函数
 * @param ctx 回调上下文
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_unregister_event_cb(xbdev_md_event_cb cb, void *ctx)
{
    struct raid_event_cb_entry *entry, *prev = NULL;
    
    if (!cb) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_raid_cb_mutex);
    
    // 查找并移除回调
    entry = g_raid_event_cb_list;
    while (entry) {
        if (entry->cb == cb && entry->ctx == ctx) {
            if (prev) {
                prev->next = entry->next;
            } else {
                g_raid_event_cb_list = entry->next;
            }
            
            free(entry);
            pthread_mutex_unlock(&g_raid_cb_mutex);
            return 0;
        }
        
        prev = entry;
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_raid_cb_mutex);
    
    // 未找到回调
    return -ENOENT;
}

/**
 * 触发RAID事件通知
 * 
 * @param event_type 事件类型
 * @param md_name RAID设备名称
 * @param disk_name 磁盘名称（可选）
 */
static void _xbdev_raid_event_notify(int event_type, const char *md_name, const char *disk_name)
{
    struct raid_event_cb_entry *entry;
    
    // 创建事件数据
    xbdev_md_event_t event = {
        .type = event_type,
        .timestamp = time(NULL)
    };
    
    if (md_name) {
        strncpy(event.md_name, md_name, sizeof(event.md_name) - 1);
    }
    
    if (disk_name) {
        strncpy(event.disk_name, disk_name, sizeof(event.disk_name) - 1);
    }
    
    // 调用所有注册的回调
    pthread_mutex_lock(&g_raid_cb_mutex);
    
    entry = g_raid_event_cb_list;
    while (entry) {
        entry->cb(&event, entry->ctx);
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_raid_cb_mutex);
}

/**
 * RAID监控定时器回调
 */
static void raid_monitor_timer_cb(void *arg)
{
    if (!g_raid_monitor_active) {
        return;
    }
    
    // 扫描所有RAID设备状态
    // 此部分需要根据实际应用场景补充
    
    // 重新注册定时器
    g_raid_monitor_timer = spdk_poller_register(raid_monitor_timer_cb, NULL, 10000000); // 10秒
}

/**
 * 启动RAID监控
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_monitor_start(void)
{
    if (g_raid_monitor_active) {
        return 0; // 已经启动
    }
    
    g_raid_monitor_active = true;
    g_raid_monitor_timer = spdk_poller_register(raid_monitor_timer_cb, NULL, 10000000); // 10秒
    
    if (!g_raid_monitor_timer) {
        g_raid_monitor_active = false;
        return -ENOMEM;
    }
    
    return 0;
}

/**
 * 停止RAID监控
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_monitor_stop(void)
{
    if (!g_raid_monitor_active) {
        return 0; // 已经停止
    }
    
    if (g_raid_monitor_timer) {
        spdk_poller_unregister(&g_raid_monitor_timer);
        g_raid_monitor_timer = NULL;
    }
    
    g_raid_monitor_active = false;
    
    return 0;
}

/**
 * 处理SPDK线程上下文中的RAID相关请求
 *
 * @param req 请求
 */
void xbdev_md_dispatch_request(xbdev_request_t *req)
{
    switch (req->type) {
        case XBDEV_REQ_MD_CREATE:
            raid_create_on_thread(req->ctx);
            break;
            
        case XBDEV_REQ_MD_ASSEMBLE:
            raid_assemble_on_thread(req->ctx);
            break;
            
        case XBDEV_REQ_MD_STOP:
            raid_stop_on_thread(req->ctx);
            break;
            
        case XBDEV_REQ_MD_DETAIL:
            raid_detail_on_thread(req->ctx);
            break;
            
        case XBDEV_REQ_MD_MANAGE:
            raid_disk_mgmt_on_thread(req->ctx);
            break;
            
        default:
            // 未知请求类型，标记为错误
            struct {
                int rc;
                bool done;
            } *ctx = req->ctx;
            
            if (ctx) {
                ctx->rc = -EINVAL;
                ctx->done = true;
            }
            break;
    }
}

/**
 * 热备盘记录结构
 */
struct hot_spare_entry {
    char md_name[256];           // RAID设备名称，为空表示全局热备盘
    char disk_name[256];         // 磁盘名称
    bool in_use;                 // 是否已使用
    struct hot_spare_entry *next;// 下一项
};

// 热备盘链表
static struct hot_spare_entry *g_hot_spare_list = NULL;
static pthread_mutex_t g_hot_spare_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * 添加热备盘（内部函数）
 *
 * @param md_name RAID设备名称，NULL表示全局热备盘
 * @param disk_name 磁盘名称
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_raid_add_hot_spare(const char *md_name, const char *disk_name)
{
    struct hot_spare_entry *entry;
    
    if (!disk_name) {
        return -EINVAL;
    }
    
    // 检查磁盘是否存在
    struct spdk_bdev *disk_bdev = spdk_bdev_get_by_name(disk_name);
    if (!disk_bdev) {
        XBDEV_ERRLOG("找不到磁盘: %s\n", disk_name);
        return -ENODEV;
    }
    
    // 分配热备盘记录
    entry = malloc(sizeof(*entry));
    if (!entry) {
        return -ENOMEM;
    }
    
    memset(entry, 0, sizeof(*entry));
    
    if (md_name) {
        strncpy(entry->md_name, md_name, sizeof(entry->md_name) - 1);
    }
    
    strncpy(entry->disk_name, disk_name, sizeof(entry->disk_name) - 1);
    entry->in_use = false;
    
    // 添加到链表
    pthread_mutex_lock(&g_hot_spare_mutex);
    
    entry->next = g_hot_spare_list;
    g_hot_spare_list = entry;
    
    pthread_mutex_unlock(&g_hot_spare_mutex);
    
    XBDEV_NOTICELOG("添加%s热备盘: %s\n", 
                  md_name ? "专用" : "全局", disk_name);
    
    return 0;
}

/**
 * 查找匹配的热备盘（内部函数）
 *
 * @param md_name RAID设备名称
 * @param spare_disk 输出参数，匹配的热备盘名称
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_raid_find_hot_spare(const char *md_name, char *spare_disk, size_t max_len)
{
    struct hot_spare_entry *entry, *prev = NULL;
    int rc = -ENOENT;
    
    if (!md_name || !spare_disk || max_len == 0) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_hot_spare_mutex);
    
    // 首先尝试找专用热备盘
    entry = g_hot_spare_list;
    while (entry) {
        if (!entry->in_use && strcmp(entry->md_name, md_name) == 0) {
            // 找到匹配的专用热备盘
            strncpy(spare_disk, entry->disk_name, max_len - 1);
            entry->in_use = true;
            rc = 0;
            break;
        }
        
        entry = entry->next;
    }
    
    // 如果没找到专用热备盘，尝试找全局热备盘
    if (rc != 0) {
        entry = g_hot_spare_list;
        while (entry) {
            if (!entry->in_use && entry->md_name[0] == '\0') {
                // 找到可用的全局热备盘
                strncpy(spare_disk, entry->disk_name, max_len - 1);
                entry->in_use = true;
                rc = 0;
                break;
            }
            
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_hot_spare_mutex);
    
    return rc;
}

/**
 * 标记热备盘为可用（内部函数）
 *
 * @param disk_name 磁盘名称
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_raid_release_hot_spare(const char *disk_name)
{
    struct hot_spare_entry *entry;
    
    if (!disk_name) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_hot_spare_mutex);
    
    entry = g_hot_spare_list;
    while (entry) {
        if (strcmp(entry->disk_name, disk_name) == 0) {
            entry->in_use = false;
            pthread_mutex_unlock(&g_hot_spare_mutex);
            return 0;
        }
        
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_hot_spare_mutex);
    
    return -ENOENT;
}

/**
 * 移除热备盘（内部函数）
 *
 * @param md_name RAID设备名称，NULL表示移除全局热备盘
 * @param disk_name 磁盘名称
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_raid_remove_hot_spare(const char *md_name, const char *disk_name)
{
    struct hot_spare_entry *entry, *prev = NULL;
    int rc = -ENOENT;
    
    if (!disk_name) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_hot_spare_mutex);
    
    entry = g_hot_spare_list;
    while (entry) {
        if (strcmp(entry->disk_name, disk_name) == 0) {
            // 如果提供了RAID设备名称，则检查它是否匹配
            if (md_name && entry->md_name[0] != '\0' && strcmp(entry->md_name, md_name) != 0) {
                // 跳过不匹配的专用热备盘
                prev = entry;
                entry = entry->next;
                continue;
            }
            
            // 从链表中移除
            if (prev) {
                prev->next = entry->next;
            } else {
                g_hot_spare_list = entry->next;
            }
            
            XBDEV_NOTICELOG("移除%s热备盘: %s\n", 
                          entry->md_name[0] ? "专用" : "全局", disk_name);
            
            free(entry);
            rc = 0;
            break;
        }
        
        prev = entry;
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_hot_spare_mutex);
    
    return rc;
}

/**
 * 获取热备盘列表
 *
 * @param md_name RAID设备名称，NULL表示获取全局热备盘
 * @param disks 输出参数，热备盘名称数组
 * @param max_disks 数组大小
 * @return 成功返回热备盘数量，失败返回错误码
 */
int xbdev_raid_get_hot_spares(const char *md_name, char **disks, int max_disks)
{
    struct hot_spare_entry *entry;
    int count = 0;
    
    if (!disks || max_disks <= 0) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_hot_spare_mutex);
    
    entry = g_hot_spare_list;
    while (entry && count < max_disks) {
        // 如果指定了RAID名称，只返回该RAID的专用热备盘和全局热备盘
        if (!md_name || entry->md_name[0] == '\0' || strcmp(entry->md_name, md_name) == 0) {
            disks[count++] = strdup(entry->disk_name);
        }
        
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_hot_spare_mutex);
    
    return count;
}

/**
 * 清理热备盘管理系统
 */
void _xbdev_raid_cleanup_hot_spares(void)
{
    struct hot_spare_entry *entry, *next;
    
    pthread_mutex_lock(&g_hot_spare_mutex);
    
    entry = g_hot_spare_list;
    while (entry) {
        next = entry->next;
        free(entry);
        entry = next;
    }
    
    g_hot_spare_list = NULL;
    
    pthread_mutex_unlock(&g_hot_spare_mutex);
}

/**
 * 处理磁盘故障事件
 *
 * @param md_name RAID设备名称
 * @param disk_name 故障磁盘名称
 */
void _xbdev_raid_handle_disk_failure(const char *md_name, const char *disk_name)
{
    char spare_disk[256] = {0};
    int rc;
    
    // 记录故障事件
    XBDEV_WARNLOG("检测到磁盘故障: %s 在RAID %s中\n", disk_name, md_name);
    
    // 触发事件通知
    _xbdev_raid_event_notify(XBDEV_MD_EVENT_DISK_FAILURE, md_name, disk_name);
    
    // 尝试查找可用的热备盘
    rc = _xbdev_raid_find_hot_spare(md_name, spare_disk, sizeof(spare_disk));
    if (rc == 0 && spare_disk[0] != '\0') {
        // 找到可用的热备盘，自动替换故障磁盘
        XBDEV_NOTICELOG("使用热备盘 %s 自动替换故障磁盘 %s\n", spare_disk, disk_name);
        
        // 构造替换参数
        struct {
            const char *old_disk;
            const char *new_disk;
        } replace_info = {
            .old_disk = disk_name,
            .new_disk = spare_disk
        };
        
        // 执行替换操作
        xbdev_md_manage(md_name, XBDEV_MD_REPLACE_DISK, &replace_info);
        
        // 触发事件通知
        _xbdev_raid_event_notify(XBDEV_MD_EVENT_HOT_SPARE_ACTIVATED, md_name, spare_disk);
    } else {
        // 没有可用的热备盘，向系统报告RAID降级状态
        XBDEV_ERRLOG("没有可用的热备盘替换故障磁盘 %s，RAID %s已降级\n", disk_name, md_name);
        
        // 触发事件通知
        _xbdev_raid_event_notify(XBDEV_MD_EVENT_RAID_DEGRADED, md_name, NULL);
    }
}

/**
 * 检测并处理RAID状态变化
 * 
 * @param md_name RAID设备名称
 */
void _xbdev_raid_check_status(const char *md_name)
{
    static struct {
        char md_name[256];
        int last_state;
        int failed_disks;
        float resync_progress;
    } raid_states[32] = {0};  // 最多追踪32个RAID设备
    
    xbdev_md_detail_t detail;
    int rc, i, found = -1;
    
    // 检查参数
    if (!md_name) {
        return;
    }
    
    // 获取RAID详细信息
    memset(&detail, 0, sizeof(detail));
    rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        XBDEV_WARNLOG("无法获取RAID设备 %s 的状态: %d\n", md_name, rc);
        return;
    }
    
    // 查找或创建RAID状态记录
    for (i = 0; i < 32; i++) {
        if (raid_states[i].md_name[0] == '\0' || strcmp(raid_states[i].md_name, md_name) == 0) {
            found = i;
            break;
        }
    }
    
    if (found < 0) {
        // 没有找到空槽位跟踪该RAID
        XBDEV_WARNLOG("无法跟踪RAID设备 %s 的状态：超过最大跟踪设备数\n", md_name);
        return;
    }
    
    // 第一次见到该RAID，初始化记录
    if (raid_states[found].md_name[0] == '\0') {
        strncpy(raid_states[found].md_name, md_name, sizeof(raid_states[found].md_name) - 1);
        raid_states[found].last_state = detail.state;
        raid_states[found].failed_disks = detail.failed_disks;
        raid_states[found].resync_progress = detail.resync_progress;
        return;  // 首次记录不需要检测状态变化
    }
    
    // 检测状态变化并生成相应事件
    
    // 1. 检测RAID状态变化
    if (raid_states[found].last_state != detail.state) {
        // 状态已改变
        if (detail.state == XBDEV_MD_STATE_DEGRADED && 
            raid_states[found].last_state != XBDEV_MD_STATE_DEGRADED) {
            // RAID新降级
            _xbdev_raid_event_notify(XBDEV_MD_EVENT_RAID_DEGRADED, md_name, NULL);
            XBDEV_WARNLOG("RAID设备 %s 已降级\n", md_name);
        } 
        else if (detail.state == XBDEV_MD_STATE_RESYNCING && 
                raid_states[found].last_state != XBDEV_MD_STATE_RESYNCING) {
            // RAID开始重同步
            _xbdev_raid_event_notify(XBDEV_MD_EVENT_RAID_RESYNCING, md_name, NULL);
            XBDEV_NOTICELOG("RAID设备 %s 开始重同步\n", md_name);
        }
        else if (detail.state == XBDEV_MD_STATE_ACTIVE && 
                raid_states[found].last_state == XBDEV_MD_STATE_RESYNCING) {
            // 重同步完成
            _xbdev_raid_event_notify(XBDEV_MD_EVENT_RAID_RECOVERED, md_name, NULL);
            XBDEV_NOTICELOG("RAID设备 %s 已恢复正常状态\n", md_name);
        }
        else if (detail.state == XBDEV_MD_STATE_FAILED) {
            // RAID完全失败
            _xbdev_raid_event_notify(XBDEV_MD_EVENT_RAID_FAILED, md_name, NULL);
            XBDEV_ERRLOG("RAID设备 %s 已完全失败\n", md_name);
        }
        
        raid_states[found].last_state = detail.state;
    }
    
    // 2. 检测故障磁盘数量变化
    if (detail.failed_disks > raid_states[found].failed_disks) {
        // 有新的磁盘故障
        for (i = 0; i < detail.raid_disks; i++) {
            if (detail.disks[i].state == XBDEV_MD_DISK_FAULTY) {
                _xbdev_raid_handle_disk_failure(md_name, detail.disks[i].name);
            }
        }
    }
    
    // 3. 跟踪重同步进度变化
    if (detail.state == XBDEV_MD_STATE_RESYNCING) {
        // 如果进度每增加10%，发送一次通知
        int old_progress = (int)(raid_states[found].resync_progress / 10) * 10;
        int new_progress = (int)(detail.resync_progress / 10) * 10;
        
        if (new_progress > old_progress && new_progress % 10 == 0) {
            XBDEV_NOTICELOG("RAID设备 %s 重同步进度: %d%%\n", md_name, new_progress);
            
            // 可以添加进度事件通知
        }
    }
    
    // 更新记录
    raid_states[found].failed_disks = detail.failed_disks;
    raid_states[found].resync_progress = detail.resync_progress;
}

/**
 * RAID健康检查定时器回调
 */
static void raid_health_check_timer_cb(void *arg)
{
    if (!g_raid_monitor_active) {
        return;
    }
    
    // 获取所有RAID设备列表
    char *raid_devices[32];
    int num_raids = 0;
    struct spdk_bdev *bdev;
    
    // 扫描所有RAID类型的BDEV
    bdev = spdk_bdev_first();
    while (bdev && num_raids < 32) {
        // 检查是否为RAID设备
        if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") == 0) {
            raid_devices[num_raids++] = strdup(spdk_bdev_get_name(bdev));
        }
        bdev = spdk_bdev_next(bdev);
    }
    
    // 检查每个RAID设备的状态
    for (int i = 0; i < num_raids; i++) {
        _xbdev_raid_check_status(raid_devices[i]);
        free(raid_devices[i]);
    }
    
    // 重新注册定时器
    g_raid_monitor_timer = spdk_poller_register(raid_health_check_timer_cb, NULL, 10000000); // 10秒
}

/**
 * 设置RAID设备为只读模式
 *
 * @param md_name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_set_readonly(const char *md_name, bool readonly)
{
    struct spdk_bdev *bdev;
    int rc;
    
    // 在SPDK线程上下文中执行
    xbdev_request_t *req;
    struct {
        const char *name;
        bool readonly;
        int rc;
        bool done;
    } ctx = {
        .name = md_name,
        .readonly = readonly,
        .rc = 0,
        .done = false
    };
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return ctx.rc;
}

/**
 * 在SPDK线程上下文中设置RAID设备只读模式
 */
static void raid_set_readonly_on_thread(void *arg)
{
    struct {
        const char *name;
        bool readonly;
        int rc;
        bool done;
    } *ctx = arg;
    
    struct spdk_bdev *bdev;
    
    // 查找RAID设备
    bdev = spdk_bdev_get_by_name(ctx->name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到RAID设备: %s\n", ctx->name);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 检查是否为RAID设备
    if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") != 0) {
        XBDEV_ERRLOG("%s 不是RAID设备\n", ctx->name);
        ctx->rc = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 设置只读模式
    ctx->rc = spdk_bdev_raid_set_readonly(bdev, ctx->readonly);
    if (ctx->rc != 0) {
        XBDEV_ERRLOG("设置RAID设备 %s 的只读模式失败: %d\n", ctx->name, ctx->rc);
    } else {
        XBDEV_NOTICELOG("已设置RAID设备 %s 为%s模式\n", 
                      ctx->name, ctx->readonly ? "只读" : "读写");
    }
    
    ctx->done = true;
}

/**
 * 强制重建RAID设备
 *
 * @param md_name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_force_rebuild(const char *md_name)
{
    return xbdev_md_manage(md_name, XBDEV_MD_START_REBUILD, NULL);
}

/**
 * 检查RAID设备是否处于降级状态
 *
 * @param md_name RAID设备名称
 * @param is_degraded 输出参数，设置为降级状态
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_is_degraded(const char *md_name, bool *is_degraded)
{
    xbdev_md_detail_t detail;
    int rc;
    
    if (!md_name || !is_degraded) {
        return -EINVAL;
    }
    
    // 获取RAID详细信息
    rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }
    
    *is_degraded = detail.degraded;
    
    return 0;
}

/**
 * 获取RAID设备重建进度
 *
 * @param md_name RAID设备名称
 * @param rebuild_active 输出参数，是否正在重建
 * @param progress 输出参数，重建进度(0-100)
 * @param est_remaining_time 输出参数，估计剩余时间(秒)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_get_rebuild_progress(const char *md_name, bool *rebuild_active, 
                                float *progress, uint32_t *est_remaining_time)
{
    xbdev_md_detail_t detail;
    int rc;
    
    if (!md_name || !rebuild_active || !progress) {
        return -EINVAL;
    }
    
    // 获取RAID详细信息
    rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }
    
    // 设置是否正在重建
    *rebuild_active = (detail.state == XBDEV_MD_STATE_RESYNCING);
    
    // 设置重建进度
    *progress = detail.resync_progress;
    
    // 计算剩余时间（如果正在重建且速度不为0）
    if (est_remaining_time) {
        if (*rebuild_active && detail.resync_speed > 0) {
            // 剩余数据量(KB) / 速度(KB/s) = 剩余时间(s)
            uint64_t total_size = detail.size; // KB
            uint64_t remaining_size = total_size * (100.0 - detail.resync_progress) / 100.0;
            *est_remaining_time = (uint32_t)(remaining_size / detail.resync_speed);
        } else {
            *est_remaining_time = 0;
        }
    }
    
    return 0;
}

/**
 * RAID模块初始化
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_raid_module_init(void)
{
    // 初始化热备盘管理
    g_hot_spare_list = NULL;
    pthread_mutex_init(&g_hot_spare_mutex, NULL);
    
    // 初始化RAID事件回调链表
    g_raid_event_cb_list = NULL;
    pthread_mutex_init(&g_raid_cb_mutex, NULL);
    
    // 初始化RAID监控系统
    g_raid_monitor_active = false;
    g_raid_monitor_timer = NULL;
    
    XBDEV_NOTICELOG("RAID模块初始化成功\n");
    
    return 0;
}

/**
 * RAID模块清理
 */
void xbdev_raid_module_fini(void)
{
    // 停止RAID监控
    xbdev_md_monitor_stop();
    
    // 清理热备盘管理
    _xbdev_raid_cleanup_hot_spares();
    pthread_mutex_destroy(&g_hot_spare_mutex);
    
    // 清理事件回调链表
    struct raid_event_cb_entry *entry = g_raid_event_cb_list;
    struct raid_event_cb_entry *next;
    
    while (entry) {
        next = entry->next;
        free(entry);
        entry = next;
    }
    
    g_raid_event_cb_list = NULL;
    pthread_mutex_destroy(&g_raid_cb_mutex);
    
    XBDEV_NOTICELOG("RAID模块清理完成\n");
}

/**
 * 创建线性RAID设备(RAID级别0，无条带化)
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_linear(const char *name, const char **base_bdevs, int num_base_bdevs)
{
    // 线性RAID不需要特殊配置
    return xbdev_md_create(name, XBDEV_MD_LEVEL_LINEAR, base_bdevs, num_base_bdevs, NULL);
}

/**
 * 获取RAID设备的所有成员磁盘
 *
 * @param md_name RAID设备名称
 * @param disks 输出参数，磁盘名称数组
 * @param max_disks 数组大小
 * @return 成功返回磁盘数量，失败返回错误码
 */
int xbdev_md_get_member_disks(const char *md_name, char **disks, int max_disks)
{
    xbdev_md_detail_t detail;
    int rc, count = 0;
    
    if (!md_name || !disks || max_disks <= 0) {
        return -EINVAL;
    }
    
    // 获取RAID详细信息
    rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }
    
    // 复制磁盘名称
    for (int i = 0; i < detail.raid_disks && count < max_disks; i++) {
        if (detail.disks[i].state != XBDEV_MD_DISK_REMOVED) {
            disks[count++] = strdup(detail.disks[i].name);
        }
    }
    
    return count;
}

/**
 * 打开RAID设备
 *
 * @param md_name RAID设备名称
 * @return 成功返回文件描述符，失败返回错误码
 */
int xbdev_md_open(const char *md_name)
{
    // 实际上只是调用通用的bdev打开函数
    return xbdev_open(md_name);
}

/**
 * 关闭RAID设备
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_close(int fd)
{
    // 实际上只是调用通用的bdev关闭函数
    return xbdev_close(fd);
}

/**
 * 创建RAID10设备(RAID级别10，条带化镜像)
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小(KB)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid10(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb)
{
    // 检查参数
    if (!name || !base_bdevs || num_base_bdevs < 4 || (num_base_bdevs % 2) != 0) {
        XBDEV_ERRLOG("无效的RAID10参数: num_base_bdevs=%d (必须是大于等于4的偶数)\n", num_base_bdevs);
        return -EINVAL;
    }
    
    // 当前SPDK不直接支持RAID10，我们需要手动创建RAID1+0结构
    // 第一步：创建n/2个RAID1设备
    int i, rc;
    char **raid1_names = calloc(num_base_bdevs / 2, sizeof(char *));
    if (!raid1_names) {
        return -ENOMEM;
    }
    
    // 创建RAID1设备
    for (i = 0; i < num_base_bdevs / 2; i++) {
        const char *raid1_disks[2] = { base_bdevs[i*2], base_bdevs[i*2+1] };
        char raid1_name[256];
        snprintf(raid1_name, sizeof(raid1_name), "%s_raid1_%d", name, i);
        
        rc = xbdev_md_create_raid1(raid1_name, raid1_disks, 2);
        if (rc != 0) {
            // 清理已创建的RAID1设备
            for (int j = 0; j < i; j++) {
                xbdev_md_stop(raid1_names[j]);
                free(raid1_names[j]);
            }
            free(raid1_names);
            return rc;
        }
        
        raid1_names[i] = strdup(raid1_name);
    }
    
    // 第二步：基于RAID1设备创建RAID0
    const char **raid0_disks = (const char **)raid1_names;
    rc = xbdev_md_create_raid0(name, raid0_disks, num_base_bdevs / 2, chunk_size_kb);
    
    // 清理
    for (i = 0; i < num_base_bdevs / 2; i++) {
        free(raid1_names[i]);
    }
    free(raid1_names);
    
    if (rc != 0) {
        // 如果RAID0创建失败，需要清理RAID1设备
        for (i = 0; i < num_base_bdevs / 2; i++) {
            char raid1_name[256];
            snprintf(raid1_name, sizeof(raid1_name), "%s_raid1_%d", name, i);
            xbdev_md_stop(raid1_name);
        }
        return rc;
    }
    
    return 0;
}

/**
 * 扫描并识别已有的RAID设备
 *
 * @return 成功返回找到的RAID设备数量，失败返回错误码
 */
int xbdev_md_scan(void)
{
    // 这个函数应该扫描所有基础设备，寻找并组装已有的RAID设备
    // 在当前SPDK的实现中，很难自动识别以前创建的RAID设备
    // 这里提供一个简化的实现
    
    XBDEV_NOTICELOG("扫描RAID设备...\n");
    
    // 获取所有BDEV设备
    struct spdk_bdev *bdev;
    int count = 0;
    
    bdev = spdk_bdev_first();
    while (bdev) {
        // 检查是否已经是RAID设备
        if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") == 0) {
            count++;
        }
        
        bdev = spdk_bdev_next(bdev);
    }
    
    XBDEV_NOTICELOG("扫描完成，发现%d个RAID设备\n", count);
    
    return count;
}

/**
 * 处理RAID相关的配置文件项
 *
 * @param config 配置对象
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_apply_raid_devices(xbdev_config_t *config)
{
    if (!config || !config->raid_devices) {
        return 0;  // 没有RAID配置，直接成功返回
    }
    
    // 遍历RAID配置
    struct spdk_json_val *val = config->raid_devices + 1;  // 跳过数组开始标记
    int rc;
    
    for (uint32_t i = 0; i < config->raid_devices->len; i++) {
        // 解析RAID配置项
        char *method = NULL;
        struct spdk_json_val *params = NULL;
        
        struct spdk_json_object_decoder decoders[] = {
            {"method", offsetof(struct { char *method; }, method), spdk_json_decode_string},
            {"params", offsetof(struct { struct spdk_json_val *params; }, params), spdk_json_decode_object}
        };
        
        if (spdk_json_decode_object(val, decoders, SPDK_COUNTOF(decoders), &method, &params) != 0) {
            XBDEV_ERRLOG("无法解析RAID配置项\n");
            return -EINVAL;
        }
        
        // 根据方法类型进行处理
        if (strcmp(method, "raid0") == 0) {
            // 解析RAID0配置
            char *name = NULL;
            char **base_bdevs = NULL;
            uint64_t strip_size_kb = 0;
            int rc;
            
            struct spdk_json_object_decoder raid0_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"base_bdevs", offsetof(struct { char **base_bdevs; }, base_bdevs), spdk_json_decode_string_array},
                {"strip_size_kb", offsetof(struct { uint64_t strip_size_kb; }, strip_size_kb), spdk_json_decode_uint64, true}
            };
            
            if (spdk_json_decode_object(params, raid0_decoders, SPDK_COUNTOF(raid0_decoders), &name, &base_bdevs, &strip_size_kb) != 0) {
                XBDEV_ERRLOG("无法解析RAID0配置参数\n");
                free(method);
                return -EINVAL;
            }
            
            // 使用默认块大小(如果未指定)
            if (strip_size_kb == 0) {
                strip_size_kb = 64;
            }
            
            // 获取基础设备数量
            int num_base_bdevs = 0;
            while (base_bdevs[num_base_bdevs] != NULL) {
                num_base_bdevs++;
            }
            
            // 创建RAID0设备
            rc = xbdev_md_create_raid0(name, (const char**)base_bdevs, num_base_bdevs, strip_size_kb);
            
            // 释放解析分配的内存
            free(name);
            for (int j = 0; j < num_base_bdevs; j++) {
                free(base_bdevs[j]);
            }
            free(base_bdevs);
            
            if (rc != 0) {
                XBDEV_ERRLOG("无法创建RAID0设备: %d\n", rc);
                free(method);
                return rc;
            }
        } 
        else if (strcmp(method, "raid1") == 0) {
            // 解析RAID1配置
            char *name = NULL;
            char **base_bdevs = NULL;
            int rc;
            
            struct spdk_json_object_decoder raid1_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"base_bdevs", offsetof(struct { char **base_bdevs; }, base_bdevs), spdk_json_decode_string_array}
            };
            
            if (spdk_json_decode_object(params, raid1_decoders, SPDK_COUNTOF(raid1_decoders), &name, &base_bdevs) != 0) {
                XBDEV_ERRLOG("无法解析RAID1配置参数\n");
                free(method);
                return -EINVAL;
            }
            
            // 获取基础设备数量
            int num_base_bdevs = 0;
            while (base_bdevs[num_base_bdevs] != NULL) {
                num_base_bdevs++;
            }
            
            // 创建RAID1设备
            rc = xbdev_md_create_raid1(name, (const char**)base_bdevs, num_base_bdevs);
            
            // 释放解析分配的内存
            free(name);
            for (int j = 0; j < num_base_bdevs; j++) {
                free(base_bdevs[j]);
            }
            free(base_bdevs);
            
            if (rc != 0) {
                XBDEV_ERRLOG("无法创建RAID1设备: %d\n", rc);
                free(method);
                return rc;
            }
        } 
        else if (strcmp(method, "raid5") == 0) {
            // 解析RAID5配置
            char *name = NULL;
            char **base_bdevs = NULL;
            uint64_t strip_size_kb = 0;
            int rc;
            
            struct spdk_json_object_decoder raid5_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"base_bdevs", offsetof(struct { char **base_bdevs; }, base_bdevs), spdk_json_decode_string_array},
                {"strip_size_kb", offsetof(struct { uint64_t strip_size_kb; }, strip_size_kb), spdk_json_decode_uint64, true}
            };
            
            if (spdk_json_decode_object(params, raid5_decoders, SPDK_COUNTOF(raid5_decoders), &name, &base_bdevs, &strip_size_kb) != 0) {
                XBDEV_ERRLOG("无法解析RAID5配置参数\n");
                free(method);
                return -EINVAL;
            }
            
            // 使用默认块大小(如果未指定)
            if (strip_size_kb == 0) {
                strip_size_kb = 64;
            }
            
            // 获取基础设备数量
            int num_base_bdevs = 0;
            while (base_bdevs[num_base_bdevs] != NULL) {
                num_base_bdevs++;
            }
            
            // 创建RAID5设备
            rc = xbdev_md_create_raid5(name, (const char**)base_bdevs, num_base_bdevs, strip_size_kb);
            
            // 释放解析分配的内存
            free(name);
            for (int j = 0; j < num_base_bdevs; j++) {
                free(base_bdevs[j]);
            }
            free(base_bdevs);
            
            if (rc != 0) {
                XBDEV_ERRLOG("无法创建RAID5设备: %d\n", rc);
                free(method);
                return rc;
            }
        } 
        else if (strcmp(method, "raid10") == 0) {
            // 解析RAID10配置
            char *name = NULL;
            char **base_bdevs = NULL;
            uint64_t strip_size_kb = 0;
            int rc;
            
            struct spdk_json_object_decoder raid10_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"base_bdevs", offsetof(struct { char **base_bdevs; }, base_bdevs), spdk_json_decode_string_array},
                {"strip_size_kb", offsetof(struct { uint64_t strip_size_kb; }, strip_size_kb), spdk_json_decode_uint64, true}
            };
            
            if (spdk_json_decode_object(params, raid10_decoders, SPDK_COUNTOF(raid10_decoders), &name, &base_bdevs, &strip_size_kb) != 0) {
                XBDEV_ERRLOG("无法解析RAID10配置参数\n");
                free(method);
                return -EINVAL;
            }
            
            // 使用默认块大小(如果未指定)
            if (strip_size_kb == 0) {
                strip_size_kb = 64;
            }
            
            // 获取基础设备数量
            int num_base_bdevs = 0;
            while (base_bdevs[num_base_bdevs] != NULL) {
                num_base_bdevs++;
            }
            
            // 创建RAID10设备
            rc = xbdev_md_create_raid10(name, (const char**)base_bdevs, num_base_bdevs, strip_size_kb);
            
            // 释放解析分配的内存
            free(name);
            for (int j = 0; j < num_base_bdevs; j++) {
                free(base_bdevs[j]);
            }
            free(base_bdevs);
            
            if (rc != 0) {
                XBDEV_ERRLOG("无法创建RAID10设备: %d\n", rc);
                free(method);
                return rc;
            }
        } 
        else if (strcmp(method, "linear") == 0) {
            // 解析线性RAID配置
            char *name = NULL;
            char **base_bdevs = NULL;
            int rc;
            
            struct spdk_json_object_decoder linear_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"base_bdevs", offsetof(struct { char **base_bdevs; }, base_bdevs), spdk_json_decode_string_array}
            };
            
            if (spdk_json_decode_object(params, linear_decoders, SPDK_COUNTOF(linear_decoders), &name, &base_bdevs) != 0) {
                XBDEV_ERRLOG("无法解析Linear RAID配置参数\n");
                free(method);
                return -EINVAL;
            }
            
            // 获取基础设备数量
            int num_base_bdevs = 0;
            while (base_bdevs[num_base_bdevs] != NULL) {
                num_base_bdevs++;
            }
            
            // 创建线性RAID设备
            rc = xbdev_md_create_linear(name, (const char**)base_bdevs, num_base_bdevs);
            
            // 释放解析分配的内存
            free(name);
            for (int j = 0; j < num_base_bdevs; j++) {
                free(base_bdevs[j]);
            }
            free(base_bdevs);
            
            if (rc != 0) {
                XBDEV_ERRLOG("无法创建Linear RAID设备: %d\n", rc);
                free(method);
                return rc;
            }
        } 
        else {
            XBDEV_ERRLOG("未知的RAID类型: %s\n", method);
            free(method);
            return -EINVAL;
        }
        
        free(method);
        
        // 移动到下一个RAID配置项
        val = spdk_json_next(val);
    }
    
    return 0;
}

/**
 * 收集现有的RAID设备配置
 *
 * @param raid_array JSON数组值
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_collect_raid_config(struct spdk_json_write_ctx *w)
{
    struct spdk_bdev *bdev;
    int rc = 0;
    
    // 开始RAID设备数组
    spdk_json_write_array_begin(w);
    
    bdev = spdk_bdev_first();
    while (bdev) {
        // 检查是否为RAID设备
        if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") == 0) {
            struct spdk_bdev_raid_bdev *raid_bdev = spdk_bdev_get_raid_bdev(bdev);
            if (!raid_bdev) {
                XBDEV_WARNLOG("无法获取RAID设备 %s 信息\n", spdk_bdev_get_name(bdev));
                bdev = spdk_bdev_next(bdev);
                continue;
            }
            
            // 开始这个RAID设备的对象
            spdk_json_write_object_begin(w);
            
            // 确定RAID类型
            const char *raid_type = NULL;
            int raid_level = spdk_bdev_raid_get_level(raid_bdev);
            
            switch (raid_level) {
                case SPDK_BDEV_RAID_LEVEL_0:
                    raid_type = "raid0";
                    break;
                case SPDK_BDEV_RAID_LEVEL_1:
                    raid_type = "raid1";
                    break;
                case SPDK_BDEV_RAID_LEVEL_5:
                    raid_type = "raid5";
                    break;
                case SPDK_BDEV_RAID_LEVEL_CONCAT:
                    raid_type = "linear";
                    break;
                default:
                    raid_type = "unknown";
            }
            
            // 写入RAID方法类型
            spdk_json_write_named_string(w, "method", raid_type);
            
            // 开始参数对象
            spdk_json_write_named_object_begin(w, "params");
            
            // 写入名称
            spdk_json_write_named_string(w, "name", spdk_bdev_get_name(bdev));
            
            // 收集并写入基础设备列表
            spdk_json_write_named_array_begin(w, "base_bdevs");
            
            int num_base_bdevs = spdk_bdev_raid_get_num_base_bdevs(raid_bdev);
            for (int i = 0; i < num_base_bdevs; i++) {
                struct spdk_bdev *base_bdev = spdk_bdev_raid_get_base_bdev(raid_bdev, i);
                if (base_bdev) {
                    spdk_json_write_string(w, spdk_bdev_get_name(base_bdev));
                }
            }
            
            // 结束基础设备数组
            spdk_json_write_array_end(w);
            
            // 如果是RAID0、RAID5或RAID10，写入条带大小
            if (raid_level == SPDK_BDEV_RAID_LEVEL_0 || 
                raid_level == SPDK_BDEV_RAID_LEVEL_5) {
                uint64_t strip_size_kb = spdk_bdev_raid_get_strip_size(raid_bdev) / 1024;
                if (strip_size_kb > 0) {
                    spdk_json_write_named_uint64(w, "strip_size_kb", strip_size_kb);
                }
            }
            
            // 结束参数对象
            spdk_json_write_object_end(w);
            
            // 结束这个RAID设备的对象
            spdk_json_write_object_end(w);
        }
        
        // 移动到下一个设备
        bdev = spdk_bdev_next(bdev);
    }
    
    // 结束RAID设备数组
    spdk_json_write_array_end(w);
    
    return rc;
}

/**
 * RAID监控线程入口点
 */
static void *raid_monitor_thread_fn(void *arg)
{
    XBDEV_NOTICELOG("RAID监控线程已启动\n");
    
    while (g_raid_monitor_active) {
        // 扫描所有RAID设备并检查状态
        struct spdk_bdev *bdev;
        
        bdev = spdk_bdev_first();
        while (bdev) {
            if (strcmp(spdk_bdev_get_product_name(bdev), "RAID Volume") == 0) {
                const char *raid_name = spdk_bdev_get_name(bdev);
                _xbdev_raid_check_status(raid_name);
            }
            bdev = spdk_bdev_next(bdev);
        }
        
        // 休眠一段时间
        sleep(10);  // 每10秒检查一次
    }
    
    XBDEV_NOTICELOG("RAID监控线程已退出\n");
    return NULL;
}

/**
 * 后台启动独立的RAID监控线程
 */
static pthread_t g_raid_monitor_thread_id;

/**
 * 启动独立RAID监控线程
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_monitor_start_thread(void)
{
    int rc;
    
    if (g_raid_monitor_active) {
        return 0;  // 已经启动
    }
    
    g_raid_monitor_active = true;
    
    // 创建监控线程
    rc = pthread_create(&g_raid_monitor_thread_id, NULL, raid_monitor_thread_fn, NULL);
    if (rc != 0) {
        XBDEV_ERRLOG("创建RAID监控线程失败: %s\n", strerror(rc));
        g_raid_monitor_active = false;
        return -rc;
    }
    
    return 0;
}

/**
 * 停止独立RAID监控线程
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_monitor_stop_thread(void)
{
    void *retval;
    
    if (!g_raid_monitor_active) {
        return 0;  // 已经停止
    }
    
    // 置位停止标志
    g_raid_monitor_active = false;
    
    // 等待线程退出
    int rc = pthread_join(g_raid_monitor_thread_id, &retval);
    if (rc != 0) {
        XBDEV_ERRLOG("等待RAID监控线程退出失败: %s\n", strerror(rc));
        return -rc;
    }
    
    return 0;
}

/**
 * 更新RAID重建进度通知
 */
static void _xbdev_raid_update_rebuild_progress(const char *md_name)
{
    static struct {
        char md_name[256];
        int last_percent;
    } rebuild_progress[32] = {0};  // 最多追踪32个RAID设备的重建进度
    
    xbdev_md_detail_t detail;
    bool rebuild_active;
    float progress;
    int i, found = -1, percent;
    
    // 获取重建进度
    if (xbdev_md_get_rebuild_progress(md_name, &rebuild_active, &progress, NULL) != 0) {
        return;
    }
    
    if (!rebuild_active) {
        // 重建不活跃，清除可能存在的进度记录
        for (i = 0; i < 32; i++) {
            if (rebuild_progress[i].md_name[0] != '\0' && 
                strcmp(rebuild_progress[i].md_name, md_name) == 0) {
                memset(&rebuild_progress[i], 0, sizeof(rebuild_progress[i]));
                break;
            }
        }
        return;
    }
    
    // 查找或创建进度记录
    for (i = 0; i < 32; i++) {
        if (rebuild_progress[i].md_name[0] == '\0' || strcmp(rebuild_progress[i].md_name, md_name) == 0) {
            found = i;
            break;
        }
    }
    
    if (found < 0) {
        // 没有找到空槽位跟踪该RAID
        return;
    }
    
    // 第一次记录该RAID的重建进度
    if (rebuild_progress[found].md_name[0] == '\0') {
        strncpy(rebuild_progress[found].md_name, md_name, sizeof(rebuild_progress[found].md_name) - 1);
        rebuild_progress[found].last_percent = -1;  // 强制首次更新
    }
    
    // 计算百分比并检查是否有显著变化
    percent = (int)(progress);
    
    // 每增长5%或达到100%时通知
    if (percent >= 100 || 
        (rebuild_progress[found].last_percent < 0) ||
        (percent / 5 > rebuild_progress[found].last_percent / 5)) {
        
        // 生成进度事件
        XBDEV_NOTICELOG("RAID设备 %s 重建进度: %d%%\n", md_name, percent);
        
        // 创建进度事件数据
        struct {
            float progress;
            uint32_t remaining_time_sec;
        } progress_data = {
            .progress = progress,
            .remaining_time_sec = 0
        };
        
        // 获取剩余时间估计（如果可用）
        xbdev_md_get_rebuild_progress(md_name, NULL, NULL, &progress_data.remaining_time_sec);
        
        // 触发进度事件
        _xbdev_raid_event_notify(XBDEV_MD_EVENT_REBUILD_PROGRESS, md_name, &progress_data);
        
        // 更新上次通知的进度
        rebuild_progress[found].last_percent = percent;
        
        // 如果达到100%，触发重建完成事件
        if (percent >= 100) {
            _xbdev_raid_event_notify(XBDEV_MD_EVENT_RAID_RECOVERED, md_name, NULL);
            // 清除进度记录
            memset(&rebuild_progress[found], 0, sizeof(rebuild_progress[found]));
        }
    }
}

/**
 * 扩展RAID磁盘数量
 *
 * @param md_name RAID设备名称
 * @param new_disks 新增磁盘名称数组
 * @param num_new_disks 新增磁盘数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_grow(const char *md_name, const char **new_disks, int num_new_disks)
{
    if (!md_name || !new_disks || num_new_disks <= 0) {
        return -EINVAL;
    }
    
    // 获取当前RAID信息
    xbdev_md_detail_t detail;
    int rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }
    
    // 根据RAID级别执行不同的扩容流程
    switch (detail.level) {
        case XBDEV_MD_LEVEL_LINEAR:
            // 线性RAID可以直接添加设备
            for (int i = 0; i < num_new_disks; i++) {
                rc = xbdev_md_manage(md_name, XBDEV_MD_ADD_DISK, (void*)new_disks[i]);
                if (rc != 0) {
                    XBDEV_ERRLOG("无法将磁盘 %s 添加到LINEAR RAID %s, rc=%d\n",
                               new_disks[i], md_name, rc);
                    return rc;
                }
            }
            break;
            
        case XBDEV_MD_LEVEL_RAID0:
            // RAID0扩容比较复杂，需要重建整个RAID
            {
                // 收集当前所有磁盘
                int total_disks = detail.raid_disks + num_new_disks;
                const char **all_disks = malloc(total_disks * sizeof(char*));
                if (!all_disks) {
                    return -ENOMEM;
                }
                
                // 复制现有磁盘
                int disk_idx = 0;
                for (int i = 0; i < detail.raid_disks; i++) {
                    if (detail.disks[i].state != XBDEV_MD_DISK_REMOVED) {
                        all_disks[disk_idx++] = strdup(detail.disks[i].name);
                    }
                }
                
                // 添加新磁盘
                for (int i = 0; i < num_new_disks; i++) {
                    all_disks[disk_idx++] = strdup(new_disks[i]);
                }
                
                // 停止现有RAID
                rc = xbdev_md_stop(md_name);
                if (rc != 0) {
                    XBDEV_ERRLOG("无法停止RAID0 %s 进行扩容, rc=%d\n", md_name, rc);
                    for (int i = 0; i < disk_idx; i++) {
                        free((void*)all_disks[i]);
                    }
                    free(all_disks);
                    return rc;
                }
                
                // 使用相同的条带大小创建新RAID0
                rc = xbdev_md_create_raid0(md_name, all_disks, disk_idx, detail.chunk_size);
                
                // 释放临时内存
                for (int i = 0; i < disk_idx; i++) {
                    free((void*)all_disks[i]);
                }
                free(all_disks);
                
                if (rc != 0) {
                    XBDEV_ERRLOG("重建扩容的RAID0 %s 失败, rc=%d\n", md_name, rc);
                    return rc;
                }
            }
            break;
            
        case XBDEV_MD_LEVEL_RAID1:
            // RAID1可以直接添加镜像设备
            for (int i = 0; i < num_new_disks; i++) {
                rc = xbdev_md_manage(md_name, XBDEV_MD_ADD_DISK, (void*)new_disks[i]);
                if (rc != 0) {
                    XBDEV_ERRLOG("无法将镜像磁盘 %s 添加到RAID1 %s, rc=%d\n",
                               new_disks[i], md_name, rc);
                    return rc;
                }
                
                // 等待同步完成
                bool rebuild_active;
                float progress;
                do {
                    sleep(1);
                    rc = xbdev_md_get_rebuild_progress(md_name, &rebuild_active, &progress, NULL);
                    if (rc != 0) {
                        XBDEV_WARNLOG("无法获取RAID1 %s 的同步进度, rc=%d\n", md_name, rc);
                        break;
                    }
                } while (rebuild_active);
            }
            break;
            
        case XBDEV_MD_LEVEL_RAID5:
            // RAID5扩容当前不支持
            XBDEV_ERRLOG("当前不支持RAID5扩容\n");
            return -ENOTSUP;
            
        default:
            XBDEV_ERRLOG("未知RAID级别: %d\n", detail.level);
            return -EINVAL;
    }
    
    return 0;
}

/**
 * 收缩RAID设备（移除磁盘）
 *
 * @param md_name RAID设备名称
 * @param remove_disks 要移除的磁盘名称数组
 * @param num_remove_disks 要移除的磁盘数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_shrink(const char *md_name, const char **remove_disks, int num_remove_disks)
{
    if (!md_name || !remove_disks || num_remove_disks <= 0) {
        return -EINVAL;
    }
    
    // 获取当前RAID信息
    xbdev_md_detail_t detail;
    int rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }
    
    // 检查移除磁盘后是否还能维持RAID级别的最小需求
    int remaining_disks = detail.raid_disks - num_remove_disks;
    switch (detail.level) {
        case XBDEV_MD_LEVEL_RAID1:
            if (remaining_disks < 2) {
                XBDEV_ERRLOG("RAID1设备 %s 至少需要2个磁盘，移除后将只剩 %d 个\n", 
                           md_name, remaining_disks);
                return -EINVAL;
            }
            break;
            
        case XBDEV_MD_LEVEL_RAID5:
            if (remaining_disks < 3) {
                XBDEV_ERRLOG("RAID5设备 %s 至少需要3个磁盘，移除后将只剩 %d 个\n", 
                           md_name, remaining_disks);
                return -EINVAL;
            }
            break;
            
        case XBDEV_MD_LEVEL_RAID0:
            if (remaining_disks < 2) {
                XBDEV_ERRLOG("RAID0设备 %s 至少需要2个磁盘，移除后将只剩 %d 个\n", 
                           md_name, remaining_disks);
                return -EINVAL;
            }
            // RAID0收缩当前不支持
            XBDEV_ERRLOG("当前不支持RAID0收缩\n");
            return -ENOTSUP;
    }
    
    // 根据RAID级别执行不同的收缩流程
    switch (detail.level) {
        case XBDEV_MD_LEVEL_LINEAR:
            // 线性RAID可以直接移除设备
            for (int i = 0; i < num_remove_disks; i++) {
                rc = xbdev_md_manage(md_name, XBDEV_MD_REMOVE_DISK, (void*)remove_disks[i]);
                if (rc != 0) {
                    XBDEV_ERRLOG("无法将磁盘 %s 从LINEAR RAID %s 中移除, rc=%d\n",
                               remove_disks[i], md_name, rc);
                    return rc;
                }
            }
            break;
            
        case XBDEV_MD_LEVEL_RAID1:
            // RAID1可以直接移除镜像设备
            for (int i = 0; i < num_remove_disks; i++) {
                rc = xbdev_md_manage(md_name, XBDEV_MD_REMOVE_DISK, (void*)remove_disks[i]);
                if (rc != 0) {
                    XBDEV_ERRLOG("无法将镜像磁盘 %s 从RAID1 %s 中移除, rc=%d\n",
                               remove_disks[i], md_name, rc);
                    return rc;
                }
            }
            break;
            
        case XBDEV_MD_LEVEL_RAID5:
            // RAID5收缩当前不支持
            XBDEV_ERRLOG("当前不支持RAID5收缩\n");
            return -ENOTSUP;
            
        default:
            XBDEV_ERRLOG("未知RAID级别: %d\n", detail.level);
            return -EINVAL;
    }
    
    return 0;
}

/**
 * 设置RAID数据校验频率
 *
 * @param md_name RAID设备名称
 * @param frequency_hours 校验频率（小时，0表示禁用）
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_set_check_frequency(const char *md_name, int frequency_hours)
{
    // 获取当前RAID信息
    xbdev_md_detail_t detail;
    int rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }

    // 确保RAID类型支持数据校验
    if (detail.level != XBDEV_MD_LEVEL_RAID1 && 
        detail.level != XBDEV_MD_LEVEL_RAID5) {
        XBDEV_ERRLOG("RAID级别 %d 不支持数据校验\n", detail.level);
        return -EINVAL;
    }

    // 设置校验频率（在内部记录中）
    rc = spdk_bdev_raid_set_check_frequency(md_name, frequency_hours);
    if (rc != 0) {
        XBDEV_ERRLOG("设置RAID校验频率失败: %d\n", rc);
        return rc;
    }

    XBDEV_NOTICELOG("已设置RAID设备 %s 的校验频率为 %d 小时\n", 
                  md_name, frequency_hours);

    return 0;
}

/**
 * 强制执行RAID数据校验操作
 *
 * @param md_name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_force_check(const char *md_name)
{
    // 获取当前RAID信息
    xbdev_md_detail_t detail;
    int rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }

    // 确保RAID类型支持数据校验
    if (detail.level != XBDEV_MD_LEVEL_RAID1 && 
        detail.level != XBDEV_MD_LEVEL_RAID5) {
        XBDEV_ERRLOG("RAID级别 %d 不支持数据校验\n", detail.level);
        return -EINVAL;
    }

    // 启动强制校验
    rc = spdk_bdev_raid_start_check(md_name);
    if (rc != 0) {
        XBDEV_ERRLOG("启动RAID校验失败: %d\n", rc);
        return rc;
    }

    XBDEV_NOTICELOG("已启动RAID设备 %s 的数据校验\n", md_name);
    return 0;
}

/**
 * 获取RAID设备预计校验完成时间
 *
 * @param md_name RAID设备名称
 * @param check_active 输出参数，校验是否正在进行
 * @param progress 输出参数，校验进度(0-100)
 * @param est_remaining_time 输出参数，估计剩余时间(秒)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_get_check_progress(const char *md_name, bool *check_active,
                             float *progress, uint32_t *est_remaining_time)
{
    // 获取校验状态
    int rc = spdk_bdev_raid_get_check_status(md_name, check_active, progress);
    if (rc != 0) {
        XBDEV_ERRLOG("获取RAID校验状态失败: %d\n", rc);
        return rc;
    }

    // 计算剩余时间
    if (est_remaining_time != NULL && *check_active) {
        uint64_t check_speed, total_size, remaining;
        
        // 获取校验速度和设备大小
        rc = spdk_bdev_raid_get_check_speed(md_name, &check_speed);
        if (rc != 0) {
            *est_remaining_time = 0;
        } else {
            xbdev_md_detail_t detail;
            rc = xbdev_md_detail(md_name, &detail);
            
            if (rc != 0 || check_speed == 0) {
                *est_remaining_time = 0;
            } else {
                total_size = detail.size * 1024; // 转为字节
                remaining = total_size * (100.0 - *progress) / 100.0;
                *est_remaining_time = (uint32_t)(remaining / check_speed);
            }
        }
    } else if (est_remaining_time != NULL) {
        *est_remaining_time = 0;
    }

    return 0;
}

/**
 * 获取RAID设备的性能指标
 *
 * @param md_name RAID设备名称
 * @param stats 输出参数，性能统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_get_stats(const char *md_name, xbdev_md_stats_t *stats)
{
    if (!md_name || !stats) {
        return -EINVAL;
    }

    memset(stats, 0, sizeof(xbdev_md_stats_t));
    
    // 获取RAID详细信息（用于判断RAID级别等基本信息）
    xbdev_md_detail_t detail;
    int rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }
    
    // 填充基本信息
    stats->level = detail.level;
    stats->raid_disks = detail.raid_disks;
    
    // 获取性能统计数据
    rc = spdk_bdev_raid_get_performance_stats(md_name, &stats->read_iops, 
                                            &stats->write_iops,
                                            &stats->read_mbytes_per_sec,
                                            &stats->write_mbytes_per_sec,
                                            &stats->avg_read_latency_us,
                                            &stats->avg_write_latency_us);
                                            
    if (rc != 0) {
        XBDEV_ERRLOG("获取RAID性能统计信息失败: %d\n", rc);
        return rc;
    }
    
    // 获取各磁盘的负载均衡情况
    for (int i = 0; i < detail.raid_disks && i < XBDEV_MD_MAX_DISKS; i++) {
        if (detail.disks[i].state == XBDEV_MD_DISK_ACTIVE) {
            uint64_t disk_iops, disk_mbytes;
            
            rc = spdk_bdev_raid_get_disk_stats(md_name, i, 
                                              &disk_iops, 
                                              &disk_mbytes);
            
            if (rc == 0) {
                stats->disk_stats[i].disk_number = i;
                stats->disk_stats[i].iops = disk_iops;
                stats->disk_stats[i].mbytes_per_sec = disk_mbytes;
            }
        }
    }
    
    return 0;
}

/**
 * 重置RAID设备性能统计信息
 *
 * @param md_name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_reset_stats(const char *md_name)
{
    if (!md_name) {
        return -EINVAL;
    }
    
    int rc = spdk_bdev_raid_reset_performance_stats(md_name);
    if (rc != 0) {
        XBDEV_ERRLOG("重置RAID性能统计信息失败: %d\n", rc);
        return rc;
    }
    
    XBDEV_NOTICELOG("已重置RAID设备 %s 的性能统计信息\n", md_name);
    return 0;
}

/**
 * 自动调整RAID设备参数以优化性能
 *
 * @param md_name RAID设备名称
 * @param auto_tune 是否启用自动调优
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_auto_tune(const char *md_name, bool auto_tune)
{
    if (!md_name) {
        return -EINVAL;
    }
    
    // 获取RAID详细信息
    xbdev_md_detail_t detail;
    int rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        return rc;
    }
    
    // 配置自动调优参数
    xbdev_md_tune_params_t params = {0};
    
    if (auto_tune) {
        // 设置默认调优参数
        switch (detail.level) {
            case XBDEV_MD_LEVEL_RAID0:
                params.io_boundary_kb = detail.chunk_size * 2;
                params.max_io_size_kb = detail.chunk_size * detail.raid_disks;
                params.queue_depth_per_disk = 32;
                break;
                
            case XBDEV_MD_LEVEL_RAID1:
                params.io_boundary_kb = 64;  // 小IO边界更适合RAID1
                params.max_io_size_kb = 1024;
                params.queue_depth_per_disk = 16;
                params.read_balance_policy = XBDEV_MD_READ_BALANCE_ROUND_ROBIN;
                break;
                
            case XBDEV_MD_LEVEL_RAID5:
                params.io_boundary_kb = detail.chunk_size;
                params.max_io_size_kb = detail.chunk_size * (detail.raid_disks - 1);
                params.queue_depth_per_disk = 16;
                params.write_cache_policy = XBDEV_MD_WRITE_CACHE_WRITE_BACK;
                params.cache_flush_interval_ms = 5000; // 5秒
                break;
                
            default:
                params.io_boundary_kb = 64;
                params.max_io_size_kb = 1024;
                params.queue_depth_per_disk = 16;
        }
    }
    
    // 应用参数
    rc = spdk_bdev_raid_set_tune_params(md_name, auto_tune, &params);
    if (rc != 0) {
        XBDEV_ERRLOG("设置RAID调优参数失败: %d\n", rc);
        return rc;
    }
    
    XBDEV_NOTICELOG("%s RAID设备 %s 的自动调优功能\n", 
                  auto_tune ? "启用" : "禁用", md_name);
    return 0;
}

/**
 * 设置RAID设备的IO策略
 *
 * @param md_name RAID设备名称
 * @param policy_type 策略类型
 * @param policy_value 策略值
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_set_io_policy(const char *md_name, xbdev_md_policy_type_t policy_type, int policy_value)
{
    if (!md_name) {
        return -EINVAL;
    }
    
    // 根据策略类型设置参数
    int rc = 0;
    switch (policy_type) {
        case XBDEV_MD_POLICY_READ_BALANCE:
            rc = spdk_bdev_raid_set_read_policy(md_name, policy_value);
            if (rc == 0) {
                XBDEV_NOTICELOG("已设置RAID设备 %s 的读取平衡策略为 %d\n", 
                              md_name, policy_value);
            }
            break;
            
        case XBDEV_MD_POLICY_WRITE_CACHE:
            rc = spdk_bdev_raid_set_write_cache_policy(md_name, policy_value);
            if (rc == 0) {
                XBDEV_NOTICELOG("已设置RAID设备 %s 的写入缓存策略为 %d\n", 
                              md_name, policy_value);
            }
            break;
            
        case XBDEV_MD_POLICY_CACHE_SIZE:
            rc = spdk_bdev_raid_set_cache_size(md_name, policy_value);
            if (rc == 0) {
                XBDEV_NOTICELOG("已设置RAID设备 %s 的缓存大小为 %d MB\n", 
                              md_name, policy_value);
            }
            break;
            
        default:
            XBDEV_ERRLOG("未知的RAID IO策略类型: %d\n", policy_type);
            return -EINVAL;
    }
    
    if (rc != 0) {
        XBDEV_ERRLOG("设置RAID IO策略失败: %d\n", rc);
    }
    
    return rc;
}
