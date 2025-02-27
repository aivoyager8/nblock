/**
 * @file device_list.c
 * @brief Implements device listing and information query functions
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/string.h>
#include <spdk/json.h>
#include <spdk/util.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * @brief Device info structure - used for device enumeration
 */
typedef struct {
    char name[256];          // Device name
    char product_name[256];  // Product name
    uint64_t block_size;     // Block size
    uint64_t num_blocks;     // Number of blocks
    uint64_t size_bytes;     // Total size (bytes)
    bool write_cache;        // Whether write cache is enabled
    char driver[64];         // Driver name
    bool claimed;            // Whether device is claimed
    bool raid;               // Whether it's a RAID device
    bool lvol;               // Whether it's a LVOL device
    const char *base_bdev;   // For LVOLs, the name of base device
} xbdev_device_info_t;

struct device_list_ctx {
    xbdev_device_info_t *devices;  // Device info array
    int num_devices;               // Number of devices
    int capacity;                  // Array capacity
};

/**
 * Device iteration callback
 */
static void _bdev_iter_cb(void *ctx, struct spdk_bdev *bdev)
{
    struct device_list_ctx *list_ctx = ctx;
    
    // If array is full, expand it
    if (list_ctx->num_devices >= list_ctx->capacity) {
        int new_capacity = list_ctx->capacity * 2;
        xbdev_device_info_t *new_devices = realloc(list_ctx->devices, 
                                                  new_capacity * sizeof(xbdev_device_info_t));
        if (!new_devices) {
            XBDEV_ERRLOG("Failed to expand device list memory\n");
            return;
        }
        
        list_ctx->devices = new_devices;
        list_ctx->capacity = new_capacity;
    }
    
    // Save device info
    xbdev_device_info_t *dev_info = &list_ctx->devices[list_ctx->num_devices];
    strncpy(dev_info->name, spdk_bdev_get_name(bdev), sizeof(dev_info->name) - 1);
    strncpy(dev_info->product_name, spdk_bdev_get_product_name(bdev), sizeof(dev_info->product_name) - 1);
    dev_info->block_size = spdk_bdev_get_block_size(bdev);
    dev_info->num_blocks = spdk_bdev_get_num_blocks(bdev);
    dev_info->size_bytes = dev_info->block_size * dev_info->num_blocks;
    dev_info->write_cache = spdk_bdev_has_write_cache(bdev);
    strncpy(dev_info->driver, spdk_bdev_get_module_name(bdev), sizeof(dev_info->driver) - 1);
    dev_info->claimed = spdk_bdev_is_claimed(bdev);
    
    // Check if it's a RAID device
    dev_info->raid = spdk_bdev_is_raid(bdev);
    
    // Check if it's an LVOL device
    const char *alias = spdk_bdev_get_alias(bdev);
    dev_info->lvol = alias && strstr(alias, "/") != NULL;
    dev_info->base_bdev = NULL; // Don't get base device for now
    
    list_ctx->num_devices++;
}

/**
 * 获取设备列表
 */
static int _get_device_list(xbdev_device_info_t **devices, int *num_devices)
{
    struct device_list_ctx list_ctx;
    int initial_capacity = 32;
    
    // 初始化上下文
    list_ctx.devices = malloc(initial_capacity * sizeof(xbdev_device_info_t));
    if (!list_ctx.devices) {
        XBDEV_ERRLOG("无法分配设备列表内存\n");
        return -ENOMEM;
    }
    
    list_ctx.num_devices = 0;
    list_ctx.capacity = initial_capacity;
    
    // 在SPDK线程上下文中执行设备枚举
    xbdev_request_t *req = xbdev_sync_request_alloc();
    if (!req) {
        free(list_ctx.devices);
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型(暂用LVS_CREATE类型)
    req->type = XBDEV_REQ_LVS_CREATE; // FIXME: 应定义专门的请求类型
    req->ctx = &list_ctx;
    
    // 提交请求并等待完成
    int rc = xbdev_sync_request_execute(req);
    xbdev_sync_request_free(req);
    
    if (rc != 0) {
        free(list_ctx.devices);
        XBDEV_ERRLOG("设备枚举请求执行失败: %d\n", rc);
        return rc;
    }
    
    // 设置返回结果
    *devices = list_ctx.devices;
    *num_devices = list_ctx.num_devices;
    
    return 0;
}

/**
 * SPDK线程上下文中执行设备枚举
 */
void xbdev_list_devices_on_thread(void *ctx)
{
    struct device_list_ctx *list_ctx = ctx;
    
    // 遍历所有块设备
    spdk_for_each_bdev(list_ctx, _bdev_iter_cb);
}

/**
 * 获取单个设备的信息
 */
static int _get_device_info(const char *device_name, xbdev_device_info_t *device_info)
{
    // 在SPDK线程上下文中执行设备查询
    struct {
        const char *name;
        xbdev_device_info_t *info;
        int rc;
        bool found;
    } args = {
        .name = device_name,
        .info = device_info,
        .rc = 0,
        .found = false
    };
    
    xbdev_request_t *req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型(暂用LVS_CREATE类型)
    req->type = XBDEV_REQ_LVS_CREATE; // FIXME: 应定义专门的请求类型
    req->ctx = &args;
    
    // 提交请求并等待完成
    int rc = xbdev_sync_request_execute(req);
    xbdev_sync_request_free(req);
    
    if (rc != 0) {
        XBDEV_ERRLOG("设备信息查询请求执行失败: %d\n", rc);
        return rc;
    }
    
    if (!args.found) {
        XBDEV_ERRLOG("找不到设备: %s\n", device_name);
        return -ENOENT;
    }
    
    return 0;
}

/**
 * SPDK线程上下文中执行设备信息查询
 */
void xbdev_get_device_info_on_thread(void *ctx)
{
    struct {
        const char *name;
        xbdev_device_info_t *info;
        int rc;
        bool found;
    } *args = ctx;
    
    // 查找指定设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(args->name);
    if (!bdev) {
        args->found = false;
        return;
    }
    
    // 填充设备信息
    strncpy(args->info->name, spdk_bdev_get_name(bdev), sizeof(args->info->name) - 1);
    strncpy(args->info->product_name, spdk_bdev_get_product_name(bdev), sizeof(args->info->product_name) - 1);
    args->info->block_size = spdk_bdev_get_block_size(bdev);
    args->info->num_blocks = spdk_bdev_get_num_blocks(bdev);
    args->info->size_bytes = args->info->block_size * args->info->num_blocks;
    args->info->write_cache = spdk_bdev_has_write_cache(bdev);
    strncpy(args->info->driver, spdk_bdev_get_module_name(bdev), sizeof(args->info->driver) - 1);
    args->info->claimed = spdk_bdev_is_claimed(bdev);
    
    // 判断是否为RAID设备
    args->info->raid = spdk_bdev_is_raid(bdev);
    
    // 判断是否为LVOL设备
    const char *alias = spdk_bdev_get_alias(bdev);
    args->info->lvol = alias && strstr(alias, "/") != NULL;
    args->info->base_bdev = NULL;
    
    args->found = true;
}

/**
 * 获取设备列表JSON表示
 * 
 * 此函数会获取系统中所有可用的块设备列表，并以JSON格式返回
 *
 * @param json_response 输出参数，存储JSON格式的设备列表
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_get_device_list(char *json_response, size_t response_size)
{
    xbdev_device_info_t *devices = NULL;
    int num_devices = 0;
    int rc;
    
    // 获取设备列表
    rc = _get_device_list(&devices, &num_devices);
    if (rc != 0) {
        snprintf(json_response, response_size, "{\"error\":\"获取设备列表失败: %d\"}", rc);
        return rc;
    }
    
    // 将设备列表转换为JSON
    FILE *f = open_memstream(&json_response, &response_size);
    if (!f) {
        free(devices);
        return -ENOMEM;
    }
    
    struct spdk_json_write_ctx *w = spdk_json_write_begin(f, 0, false);
    if (!w) {
        fclose(f);
        free(devices);
        return -ENOMEM;
    }
    
    // 开始构造JSON
    spdk_json_write_object_begin(w);
    
    // 写入设备数组
    spdk_json_write_named_array_begin(w, "devices");
    
    for (int i = 0; i < num_devices; i++) {
        xbdev_device_info_t *dev = &devices[i];
        
        spdk_json_write_object_begin(w);
        
        spdk_json_write_named_string(w, "name", dev->name);
        spdk_json_write_named_string(w, "product_name", dev->product_name);
        spdk_json_write_named_uint64(w, "block_size", dev->block_size);
        spdk_json_write_named_uint64(w, "num_blocks", dev->num_blocks);
        spdk_json_write_named_uint64(w, "size_bytes", dev->size_bytes);
        spdk_json_write_named_uint64(w, "size_mb", dev->size_bytes / (1024 * 1024));
        spdk_json_write_named_bool(w, "write_cache", dev->write_cache);
        spdk_json_write_named_string(w, "driver", dev->driver);
        spdk_json_write_named_bool(w, "claimed", dev->claimed);
        spdk_json_write_named_bool(w, "raid", dev->raid);
        spdk_json_write_named_bool(w, "lvol", dev->lvol);
        
        if (dev->base_bdev) {
            spdk_json_write_named_string(w, "base_bdev", dev->base_bdev);
        }
        
        spdk_json_write_object_end(w);
    }
    
    // 结束设备数组
    spdk_json_write_array_end(w);
    
    // 写入总数
    spdk_json_write_named_int32(w, "total", num_devices);
    
    // 结束JSON对象
    spdk_json_write_object_end(w);
    
    // 完成JSON写入
    spdk_json_write_end(w);
    fclose(f);
    
    // 释放设备列表内存
    free(devices);
    
    return 0;
}

/**
 * 获取设备信息JSON表示
 * 
 * 此函数获取指定设备的详细信息，并以JSON格式返回
 *
 * @param device_name 设备名称
 * @param json_response 输出参数，存储JSON格式的设备信息
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_get_device_info(const char *device_name, char *json_response, size_t response_size)
{
    xbdev_device_info_t device_info = {0};
    int rc;
    
    // 获取设备信息
    rc = _get_device_info(device_name, &device_info);
    if (rc != 0) {
        snprintf(json_response, response_size, "{\"error\":\"获取设备信息失败: %d\",\"name\":\"%s\"}", 
                rc, device_name);
        return rc;
    }
    
    // 将设备信息转换为JSON
    FILE *f = open_memstream(&json_response, &response_size);
    if (!f) {
        return -ENOMEM;
    }
    
    struct spdk_json_write_ctx *w = spdk_json_write_begin(f, 0, false);
    if (!w) {
        fclose(f);
        return -ENOMEM;
    }
    
    // 写入设备信息
    spdk_json_write_object_begin(w);
    
    spdk_json_write_named_string(w, "name", device_info.name);
    spdk_json_write_named_string(w, "product_name", device_info.product_name);
    spdk_json_write_named_uint64(w, "block_size", device_info.block_size);
    spdk_json_write_named_uint64(w, "num_blocks", device_info.num_blocks);
    spdk_json_write_named_uint64(w, "size_bytes", device_info.size_bytes);
    spdk_json_write_named_uint64(w, "size_mb", device_info.size_bytes / (1024 * 1024));
    spdk_json_write_named_bool(w, "write_cache", device_info.write_cache);
    spdk_json_write_named_string(w, "driver", device_info.driver);
    spdk_json_write_named_bool(w, "claimed", device_info.claimed);
    spdk_json_write_named_bool(w, "raid", device_info.raid);
    spdk_json_write_named_bool(w, "lvol", device_info.lvol);
    
    if (device_info.base_bdev) {
        spdk_json_write_named_string(w, "base_bdev", device_info.base_bdev);
    }
    
    // 如果是RAID设备，获取RAID详细信息
    if (device_info.raid) {
        xbdev_md_detail_t raid_detail = {0};
        
        // 尝试获取RAID信息
        if (xbdev_md_detail(device_info.name, &raid_detail) == 0) {
            spdk_json_write_named_object_begin(w, "raid_info");
            
            spdk_json_write_named_int32(w, "level", raid_detail.level);
            spdk_json_write_named_int32(w, "raid_disks", raid_detail.raid_disks);
            spdk_json_write_named_int32(w, "active_disks", raid_detail.active_disks);
            spdk_json_write_named_int32(w, "working_disks", raid_detail.working_disks);
            spdk_json_write_named_int32(w, "failed_disks", raid_detail.failed_disks);
            spdk_json_write_named_int32(w, "spare_disks", raid_detail.spare_disks);
            spdk_json_write_named_uint64(w, "size_kb", raid_detail.size);
            spdk_json_write_named_uint64(w, "chunk_size_kb", raid_detail.chunk_size);
            spdk_json_write_named_int32(w, "state", raid_detail.state);
            spdk_json_write_named_string(w, "uuid", raid_detail.uuid);
            spdk_json_write_named_bool(w, "degraded", raid_detail.degraded);
            
            if (raid_detail.state == XBDEV_MD_STATE_RESYNCING || 
                raid_detail.state == XBDEV_MD_STATE_RECOVERING) {
                spdk_json_write_named_string(w, "resync_status", raid_detail.resync_status);
                spdk_json_write_named_double(w, "resync_progress", raid_detail.resync_progress);
                spdk_json_write_named_int32(w, "resync_speed", raid_detail.resync_speed);
            }
            
            // 写入磁盘信息
            spdk_json_write_named_array_begin(w, "disks");
            for (int i = 0; i < raid_detail.raid_disks; i++) {
                spdk_json_write_object_begin(w);
                spdk_json_write_named_int32(w, "number", raid_detail.disks[i].number);
                spdk_json_write_named_string(w, "name", raid_detail.disks[i].name);
                spdk_json_write_named_int32(w, "state", raid_detail.disks[i].state);
                spdk_json_write_named_uint64(w, "size_kb", raid_detail.disks[i].size);
                spdk_json_write_named_string(w, "role", raid_detail.disks[i].role);
                spdk_json_write_object_end(w);
            }
            spdk_json_write_array_end(w);
            
            spdk_json_write_object_end(w);
        }
    }
    
    // 如果是LVOL设备，获取LVOL详细信息
    if (device_info.lvol) {
        // LVOL详细信息获取暂未实现
        // TODO: 实现LVOL详细信息获取
        spdk_json_write_named_object_begin(w, "lvol_info");
        
        // 尝试从名称中提取LVOL相关信息
        const char *lvs_name = NULL;
        const char *lvol_name = NULL;
        char *slash_pos = strstr(device_info.name, "/");
        
        if (slash_pos) {
            char lvs_buf[256] = {0};
            size_t lvs_len = slash_pos - device_info.name;
            if (lvs_len < sizeof(lvs_buf)) {
                strncpy(lvs_buf, device_info.name, lvs_len);
                lvs_buf[lvs_len] = '\0';
                lvs_name = lvs_buf;
                lvol_name = slash_pos + 1;
            }
        }
        
        if (lvs_name && lvol_name) {
            spdk_json_write_named_string(w, "lvs_name", lvs_name);
            spdk_json_write_named_string(w, "lvol_name", lvol_name);
        }
        
        // 暂时只写入基本信息，后续可扩展
        spdk_json_write_named_bool(w, "thin_provisioned", true);  // 假设为精简配置
        
        spdk_json_write_object_end(w);
    }
    
    // 结束JSON对象
    spdk_json_write_object_end(w);
    
    // 完成JSON写入
    spdk_json_write_end(w);
    fclose(f);
    
    return 0;
}

/**
 * RAID设备命令处理 - 创建RAID
 * 
 * @param params 命令参数
 * @param json_response JSON响应缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_cmd_create_raid(const struct spdk_json_val *params, char *json_response, size_t response_size)
{
    struct {
        char *name;
        int level;
        struct spdk_json_val *devices;
        uint64_t chunk_size_kb;
        int layout;
    } args = {
        .level = -1,
        .chunk_size_kb = 64,  // 默认64KB块大小
        .layout = 0
    };
    
    // 解码参数
    struct spdk_json_object_decoder decoders[] = {
        {"name", offsetof(typeof(args), name), spdk_json_decode_string},
        {"level", offsetof(typeof(args), level), spdk_json_decode_int32},
        {"devices", offsetof(typeof(args), devices), spdk_json_decode_array},
        {"chunk_size_kb", offsetof(typeof(args), chunk_size_kb), spdk_json_decode_uint64, true},
        {"layout", offsetof(typeof(args), layout), spdk_json_decode_int32, true}
    };
    
    if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &args)) {
        snprintf(json_response, response_size, "{\"error\":\"无效的RAID创建参数\"}");
        return -EINVAL;
    }
    
    // 检查必要参数
    if (!args.name || args.level < 0 || !args.devices) {
        snprintf(json_response, response_size, "{\"error\":\"缺少必要的RAID参数\"}");
        if (args.name) free(args.name);
        return -EINVAL;
    }
    
    // 提取设备数组
    char **device_names = NULL;
    int num_devices = 0;
    
    struct spdk_json_val *dev_val = args.devices + 1;  // 跳过数组头
    for (uint32_t i = 0; i < args.devices->len; i++, dev_val = spdk_json_next(dev_val)) {
        char *device_name = NULL;
        if (spdk_json_decode_string(dev_val, &device_name) == 0) {
            char **new_names = realloc(device_names, sizeof(char*) * (num_devices + 1));
            if (!new_names) {
                snprintf(json_response, response_size, "{\"error\":\"内存分配失败\"}");
                for (int j = 0; j < num_devices; j++) free(device_names[j]);
                free(device_names);
                free(args.name);
                return -ENOMEM;
            }
            
            device_names = new_names;
            device_names[num_devices++] = device_name;
        }
    }
    
    // 检查设备数量
    if (num_devices < 2) {
        snprintf(json_response, response_size, "{\"error\":\"RAID设备数量不足，至少需要2个设备\"}");
        for (int i = 0; i < num_devices; i++) free(device_names[i]);
        free(device_names);
        free(args.name);
        return -EINVAL;
    }
    
    // 准备RAID配置
    xbdev_md_config_t config = {
        .chunk_size_kb = args.chunk_size_kb,
        .assume_clean = false,
        .layout = args.layout
    };
    
    // 准备设备名称数组
    const char **base_bdevs = malloc(sizeof(const char*) * num_devices);
    if (!base_bdevs) {
        snprintf(json_response, response_size, "{\"error\":\"内存分配失败\"}");
        for (int i = 0; i < num_devices; i++) free(device_names[i]);
        free(device_names);
        free(args.name);
        return -ENOMEM;
    }
    
    for (int i = 0; i < num_devices; i++) {
        base_bdevs[i] = device_names[i];
    }
    
    // 创建RAID设备
    int rc = xbdev_md_create(args.name, args.level, base_bdevs, num_devices, &config);
    
    // 构建响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
                 "{\"status\":\"success\",\"name\":\"%s\",\"level\":%d,\"num_devices\":%d}",
                 args.name, args.level, num_devices);
    } else {
        snprintf(json_response, response_size, 
                 "{\"status\":\"error\",\"error_code\":%d,\"error_msg\":\"创建RAID失败\"}",
                 rc);
    }
    
    // 清理资源
    for (int i = 0; i < num_devices; i++) free(device_names[i]);
    free(device_names);
    free(base_bdevs);
    free(args.name);
    
    return rc;
}

/**
 * LVOL设备命令处理 - 创建LVOL
 * 
 * @param params 命令参数
 * @param json_response JSON响应缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_cmd_create_lvol(const struct spdk_json_val *params, char *json_response, size_t response_size)
{
    struct {
        char *lvs_name;
        char *lvol_name;
        uint64_t size_mb;
        bool thin_provision;
    } args = {
        .size_mb = 0,
        .thin_provision = true  // 默认精简配置
    };
    
    // 解码参数
    struct spdk_json_object_decoder decoders[] = {
        {"lvs_name", offsetof(typeof(args), lvs_name), spdk_json_decode_string},
        {"lvol_name", offsetof(typeof(args), lvol_name), spdk_json_decode_string},
        {"size_mb", offsetof(typeof(args), size_mb), spdk_json_decode_uint64},
        {"thin_provision", offsetof(typeof(args), thin_provision), spdk_json_decode_bool, true}
    };
    
    if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &args)) {
        snprintf(json_response, response_size, "{\"error\":\"无效的LVOL创建参数\"}");
        return -EINVAL;
    }
    
    // 检查必要参数
    if (!args.lvs_name || !args.lvol_name || args.size_mb == 0) {
        snprintf(json_response, response_size, "{\"error\":\"缺少必要的LVOL参数\"}");
        if (args.lvs_name) free(args.lvs_name);
        if (args.lvol_name) free(args.lvol_name);
        return -EINVAL;
    }
    
    // 创建LVOL卷
    int rc = xbdev_lvol_create(args.lvs_name, args.lvol_name, args.size_mb, args.thin_provision);
    
    // 构建响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
                 "{\"status\":\"success\",\"lvs_name\":\"%s\",\"lvol_name\":\"%s\",\"size_mb\":%" PRIu64 ",\"thin_provision\":%s}",
                 args.lvs_name, args.lvol_name, args.size_mb, args.thin_provision ? "true" : "false");
    } else {
        snprintf(json_response, response_size, 
                 "{\"status\":\"error\",\"error_code\":%d,\"error_msg\":\"创建LVOL失败\"}",
                 rc);
    }
    
    // 清理资源
    free(args.lvs_name);
    free(args.lvol_name);
    
    return rc;
}

/**
 * 快照命令处理 - 创建快照
 * 
 * @param params 命令参数
 * @param json_response JSON响应缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_cmd_create_snapshot(const struct spdk_json_val *params, char *json_response, size_t response_size)
{
    struct {
        char *lvol_name;
        char *snapshot_name;
    } args = {0};
    
    // 解码参数
    struct spdk_json_object_decoder decoders[] = {
        {"lvol_name", offsetof(typeof(args), lvol_name), spdk_json_decode_string},
        {"snapshot_name", offsetof(typeof(args), snapshot_name), spdk_json_decode_string}
    };
    
    if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &args)) {
        snprintf(json_response, response_size, "{\"error\":\"无效的快照创建参数\"}");
        return -EINVAL;
    }
    
    // 检查必要参数
    if (!args.lvol_name || !args.snapshot_name) {
        snprintf(json_response, response_size, "{\"error\":\"缺少必要的快照参数\"}");
        if (args.lvol_name) free(args.lvol_name);
        if (args.snapshot_name) free(args.snapshot_name);
        return -EINVAL;
    }
    
    // 创建快照
    int rc = xbdev_snapshot_create(args.lvol_name, args.snapshot_name);
    
    // 构建响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
                 "{\"status\":\"success\",\"lvol_name\":\"%s\",\"snapshot_name\":\"%s\"}",
                 args.lvol_name, args.snapshot_name);
    } else {
        snprintf(json_response, response_size, 
                 "{\"status\":\"error\",\"error_code\":%d,\"error_msg\":\"创建快照失败\"}",
                 rc);
    }
    
    // 清理资源
    free(args.lvol_name);
    free(args.snapshot_name);
    
    return rc;
}

/**
 * 调整逻辑卷大小命令
 * 
 * @param params 命令参数
 * @param json_response JSON响应缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_cmd_resize_lvol(const struct spdk_json_val *params, char *json_response, size_t response_size)
{
    // TODO: 实现调整LVOL大小功能
    snprintf(json_response, response_size, "{\"status\":\"not_implemented\",\"error_msg\":\"调整LVOL大小功能暂未实现\"}");
    return -ENOTSUP;
}

/**
 * 删除设备命令
 * 
 * @param params 命令参数
 * @param json_response JSON响应缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_cmd_delete_device(const struct spdk_json_val *params, char *json_response, size_t response_size)
{
    struct {
        char *name;
        char *type;
    } args = {0};
    
    // 解码参数
    struct spdk_json_object_decoder decoders[] = {
        {"name", offsetof(typeof(args), name), spdk_json_decode_string},
        {"type", offsetof(typeof(args), type), spdk_json_decode_string, true}
    };
    
    if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &args)) {
        snprintf(json_response, response_size, "{\"error\":\"无效的删除设备参数\"}");
        return -EINVAL;
    }
    
    // 检查必要参数
    if (!args.name) {
        snprintf(json_response, response_size, "{\"error\":\"缺少必要的设备名称参数\"}");
        return -EINVAL;
    }
    
    int rc = -EINVAL;
    
    // 根据设备类型执行不同的删除操作
    if (args.type) {
        if (strcmp(args.type, "raid") == 0) {
            rc = xbdev_md_stop(args.name);
        } else if (strstr(args.name, "/")) {
            // 假设这是LVOL设备
            // TODO: 实现LVOL删除功能
            rc = -ENOTSUP;
        } else {
            // 未知类型或基础设备
            rc = -EINVAL;
        }
    } else {
        // 尝试自动检测类型
        if (strstr(args.name, "/")) {
            // 可能是LVOL设备
            // TODO: 实现LVOL删除功能
            rc = -ENOTSUP;
        } else {
            // 假设是RAID设备
            rc = xbdev_md_stop(args.name);
            if (rc != 0) {
                // 可能不是RAID设备
                rc = -EINVAL;
            }
        }
    }
    
    // 构建响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
                 "{\"status\":\"success\",\"name\":\"%s\"}",
                 args.name);
    } else if (rc == -ENOTSUP) {
        snprintf(json_response, response_size, 
                 "{\"status\":\"error\",\"error_code\":%d,\"error_msg\":\"功能暂未实现\"}",
                 rc);
    } else {
        snprintf(json_response, response_size, 
                 "{\"status\":\"error\",\"error_code\":%d,\"error_msg\":\"删除设备失败\"}",
                 rc);
    }
    
    // 清理资源
    free(args.name);
    if (args.type) free(args.type);
    
    return rc;
}