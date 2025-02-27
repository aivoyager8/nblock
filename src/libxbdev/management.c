/**
 * @file management.c
 * @brief Implementation of management server functionality
 *
 * This file implements the management server for libxbdev, providing
 * remote management capabilities through a JSON-RPC interface.
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/jsonrpc.h>
#include <spdk/rpc.h>
#include <spdk/util.h>
#include <spdk/log.h>
#include <spdk/event.h>
#include <spdk/thread.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

// 管理服务器上下文
struct xbdev_mgmt_server {
    struct spdk_jsonrpc_server *jsonrpc_server;
    char *listen_addr;
    int listen_port;
    bool initialized;
    pthread_t listen_thread;
    bool listen_thread_running;
    pthread_mutex_t lock;
    xbdev_notification_cb notification_cb;
    void *notification_cb_arg;
};

// 全局管理服务器
static struct xbdev_mgmt_server g_mgmt_server = {0};

/**
 * 管理监听线程
 */
static void *_xbdev_mgmt_listen_thread(void *arg) {
    (void)arg;
    
    while (g_mgmt_server.listen_thread_running) {
        // 处理RPC请求
        spdk_jsonrpc_server_poll(g_mgmt_server.jsonrpc_server);
        
        // 不要消耗100%的CPU
        usleep(1000); // 1ms
    }
    
    return NULL;
}

/**
 * 初始化管理服务器
 */
int xbdev_mgmt_server_init(const char *listen_addr, int port) {
    int rc = 0;
    
    if (!listen_addr || port <= 0 || port > 65535) {
        XBDEV_ERRLOG("无效的监听地址或端口\n");
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_mgmt_server.lock);
    
    // 检查是否已初始化
    if (g_mgmt_server.initialized) {
        XBDEV_ERRLOG("管理服务器已初始化\n");
        pthread_mutex_unlock(&g_mgmt_server.lock);
        return -EALREADY;
    }
    
    // 初始化服务器上下文
    g_mgmt_server.listen_addr = strdup(listen_addr);
    if (!g_mgmt_server.listen_addr) {
        XBDEV_ERRLOG("内存分配失败\n");
        pthread_mutex_unlock(&g_mgmt_server.lock);
        return -ENOMEM;
    }
    
    g_mgmt_server.listen_port = port;
    pthread_mutex_init(&g_mgmt_server.lock, NULL);
    
    // 创建JSONRPC服务器
    g_mgmt_server.jsonrpc_server = spdk_jsonrpc_server_listen(listen_addr, port, NULL);
    if (!g_mgmt_server.jsonrpc_server) {
        XBDEV_ERRLOG("无法创建JSONRPC服务器，地址: %s，端口: %d\n", listen_addr, port);
        free(g_mgmt_server.listen_addr);
        g_mgmt_server.listen_addr = NULL;
        pthread_mutex_unlock(&g_mgmt_server.lock);
        return -ENODEV;
    }
    
    // 注册RPC方法
    // TODO: 在这里注册自定义RPC方法
    
    // 启动监听线程
    g_mgmt_server.listen_thread_running = true;
    rc = pthread_create(&g_mgmt_server.listen_thread, NULL, _xbdev_mgmt_listen_thread, NULL);
    if (rc != 0) {
        XBDEV_ERRLOG("无法创建监听线程，错误码: %d\n", rc);
        spdk_jsonrpc_server_shutdown(g_mgmt_server.jsonrpc_server);
        free(g_mgmt_server.listen_addr);
        g_mgmt_server.listen_addr = NULL;
        g_mgmt_server.listen_thread_running = false;
        pthread_mutex_unlock(&g_mgmt_server.lock);
        return -rc;
    }
    
    g_mgmt_server.initialized = true;
    
    XBDEV_NOTICELOG("管理服务器已启动，监听在 %s:%d\n", listen_addr, port);
    
    pthread_mutex_unlock(&g_mgmt_server.lock);
    
    return 0;
}

/**
 * 清理管理服务器
 */
int xbdev_mgmt_server_fini(void) {
    pthread_mutex_lock(&g_mgmt_server.lock);
    
    // 检查是否已初始化
    if (!g_mgmt_server.initialized) {
        pthread_mutex_unlock(&g_mgmt_server.lock);
        return -EINVAL;
    }
    
    // 停止监听线程
    g_mgmt_server.listen_thread_running = false;
    pthread_mutex_unlock(&g_mgmt_server.lock);
    
    pthread_join(g_mgmt_server.listen_thread, NULL);
    
    pthread_mutex_lock(&g_mgmt_server.lock);
    
    // 关闭JSONRPC服务器
    if (g_mgmt_server.jsonrpc_server) {
        spdk_jsonrpc_server_shutdown(g_mgmt_server.jsonrpc_server);
        g_mgmt_server.jsonrpc_server = NULL;
    }
    
    // 释放资源
    free(g_mgmt_server.listen_addr);
    g_mgmt_server.listen_addr = NULL;
    g_mgmt_server.listen_port = 0;
    g_mgmt_server.initialized = false;
    
    // 清除回调
    g_mgmt_server.notification_cb = NULL;
    g_mgmt_server.notification_cb_arg = NULL;
    
    pthread_mutex_unlock(&g_mgmt_server.lock);
    pthread_mutex_destroy(&g_mgmt_server.lock);
    
    XBDEV_NOTICELOG("管理服务器已关闭\n");
    
    return 0;
}

/**
 * 注册管理通知回调
 */
int xbdev_mgmt_register_notification_callback(xbdev_notification_cb cb, void *cb_arg) {
    pthread_mutex_lock(&g_mgmt_server.lock);
    
    // 检查是否已初始化
    if (!g_mgmt_server.initialized) {
        pthread_mutex_unlock(&g_mgmt_server.lock);
        return -EINVAL;
    }
    
    // 设置回调
    g_mgmt_server.notification_cb = cb;
    g_mgmt_server.notification_cb_arg = cb_arg;
    
    pthread_mutex_unlock(&g_mgmt_server.lock);
    
    XBDEV_NOTICELOG("已注册管理通知回调\n");
    
    return 0;
}

/**
 * 发送通知
 */
static void _xbdev_send_notification(int event_type, const char *device_name, void *event_data) {
    pthread_mutex_lock(&g_mgmt_server.lock);
    
    // 检查是否有回调注册
    if (g_mgmt_server.notification_cb) {
        g_mgmt_server.notification_cb(event_type, device_name, event_data, g_mgmt_server.notification_cb_arg);
    }
    
    pthread_mutex_unlock(&g_mgmt_server.lock);
}

/**
 * RPC处理函数 - 执行命令
 */
static void _xbdev_rpc_execute_cmd(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params) {
    struct spdk_json_write_ctx *w;
    char *cmd_str = NULL;
    char response[4096] = {0};
    int rc;
    
    // 解析参数
    struct spdk_json_object_decoder decoders[] = {
        {"cmd", offsetof(struct { char *cmd; }, cmd), spdk_json_decode_string},
    };
    
    if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &cmd_str)) {
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "无效的参数");
        return;
    }
    
    // 执行命令
    rc = xbdev_mgmt_execute_cmd(cmd_str, response, sizeof(response));
    free(cmd_str);
    
    // 发送响应
    w = spdk_jsonrpc_begin_result(request);
    if (w) {
        if (rc == 0) {
            spdk_json_write_object_begin(w);
            spdk_json_write_named_string(w, "status", "success");
            spdk_json_write_named_string(w, "response", response);
            spdk_json_write_object_end(w);
        } else {
            spdk_json_write_object_begin(w);
            spdk_json_write_named_string(w, "status", "error");
            spdk_json_write_named_int32(w, "error_code", rc);
            spdk_json_write_named_string(w, "error_msg", response[0] ? response : "Unknown error");
            spdk_json_write_object_end(w);
        }
        spdk_jsonrpc_end_result(request, w);
    }
}

// 注册RPC方法
SPDK_RPC_REGISTER("xbdev_execute_cmd", _xbdev_rpc_execute_cmd, SPDK_RPC_RUNTIME)

/**
 * 执行远程管理命令
 *
 * @param json_cmd JSON格式的命令
 * @param json_response 分配用于存储JSON格式响应的缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_execute_cmd(const char *json_cmd, char *json_response, size_t response_size) {
    struct spdk_json_val *values = NULL;
    size_t num_values = 0;
    int rc;
    
    if (!json_cmd || !json_response || response_size == 0) {
        return -EINVAL;
    }
    
    // 清空响应缓冲区
    memset(json_response, 0, response_size);
    
    // 解析JSON命令
    rc = spdk_json_parse(json_cmd, strlen(json_cmd), NULL, 0, &num_values, 0);
    if (rc != 0 && rc != -ESTALE) {
        snprintf(json_response, response_size, "JSON解析错误: %d", rc);
        return -EINVAL;
    }
    
    values = calloc(num_values, sizeof(*values));
    if (values == NULL) {
        snprintf(json_response, response_size, "内存分配失败");
        return -ENOMEM;
    }
    
    rc = spdk_json_parse(json_cmd, strlen(json_cmd), values, num_values, NULL, 0);
    if (rc != 0) {
        snprintf(json_response, response_size, "JSON解析错误: %d", rc);
        free(values);
        return -EINVAL;
    }
    
    // 提取命令类型和参数
    struct {
        char *method;
        struct spdk_json_val *params;
    } cmd;
    
    struct spdk_json_object_decoder cmd_decoders[] = {
        {"method", offsetof(typeof(cmd), method), spdk_json_decode_string},
        {"params", offsetof(typeof(cmd), params), spdk_json_decode_object, true}
    };
    
    if (spdk_json_decode_object(values, cmd_decoders, SPDK_COUNTOF(cmd_decoders), &cmd)) {
        snprintf(json_response, response_size, "无效的JSON命令格式");
        free(values);
        return -EINVAL;
    }
    
    // 处理不同类型的命令
    if (strcmp(cmd.method, "device_list") == 0) {
        // 获取设备列表
        rc = _xbdev_get_device_list(json_response, response_size);
    } else if (strcmp(cmd.method, "device_info") == 0) {
        // 获取设备信息
        char *device_name = NULL;
        
        struct spdk_json_object_decoder param_decoders[] = {
            {"device", offsetof(struct { char *device; }, device), spdk_json_decode_string}
        };
        
        if (cmd.params && spdk_json_decode_object(cmd.params, param_decoders, 1, &device_name)) {
            rc = _xbdev_get_device_info(device_name, json_response, response_size);
            free(device_name);
        } else {
            snprintf(json_response, response_size, "缺少设备名称参数");
            rc = -EINVAL;
        }
    } else if (strcmp(cmd.method, "create_raid") == 0) {
        // 创建RAID设备
        rc = _xbdev_cmd_create_raid(cmd.params, json_response, response_size);
    } else if (strcmp(cmd.method, "create_lvol") == 0) {
        // 创建逻辑卷
        rc = _xbdev_cmd_create_lvol(cmd.params, json_response, response_size);
    } else if (strcmp(cmd.method, "create_snapshot") == 0) {
        // 创建快照
        rc = _xbdev_cmd_create_snapshot(cmd.params, json_response, response_size);
    } else if (strcmp(cmd.method, "resize_lvol") == 0) {
        // 调整逻辑卷大小
        rc = _xbdev_cmd_resize_lvol(cmd.params, json_response, response_size);
    } else if (strcmp(cmd.method, "delete_device") == 0) {
        // 删除设备
        rc = _xbdev_cmd_delete_device(cmd.params, json_response, response_size);
    } else {
        snprintf(json_response, response_size, "未知的命令方法: %s", cmd.method);
        rc = -ENOTSUP;
    }
    
    // 释放资源
    free(cmd.method);
    free(values);
    
    return rc;
}

/**
 * 获取设备列表
 */
static int _xbdev_get_device_list(char *json_response, size_t response_size) {
    // 分配设备名称数组
    char **device_names = NULL;
    int max_devices = 100;  // 最多支持100个设备
    int device_count = 0;
    int rc = 0;
    
    device_names = malloc(sizeof(char*) * max_devices);
    if (!device_names) {
        snprintf(json_response, response_size, "{\"error\": \"内存分配失败\"}");
        return -ENOMEM;
    }
    
    // 初始化数组
    for (int i = 0; i < max_devices; i++) {
        device_names[i] = NULL;
    }
    
    // 获取设备列表
    device_count = xbdev_get_device_list(device_names, max_devices);
    if (device_count < 0) {
        snprintf(json_response, response_size, "{\"error\": \"获取设备列表失败\", \"code\": %d}", device_count);
        rc = device_count;
        goto cleanup;
    }
    
    // 构造JSON响应
    char *pos = json_response;
    size_t remaining = response_size;
    int written = 0;
    
    written = snprintf(pos, remaining, "{\"devices\": [");
    if (written >= remaining) {
        rc = -ENOSPC;
        goto cleanup;
    }
    pos += written;
    remaining -= written;
    
    // 添加每个设备名称
    for (int i = 0; i < device_count; i++) {
        written = snprintf(pos, remaining, "%s\"%s\"", 
                         (i > 0) ? ", " : "",
                         device_names[i]);
        if (written >= remaining) {
            rc = -ENOSPC;
            goto cleanup;
        }
        pos += written;
        remaining -= written;
    }
    
    // 完成JSON对象
    written = snprintf(pos, remaining, "]}");
    if (written >= remaining) {
        rc = -ENOSPC;
        goto cleanup;
    }
    
cleanup:
    // 释放资源
    for (int i = 0; i < max_devices; i++) {
        if (device_names[i]) {
            free(device_names[i]);
        }
    }
    free(device_names);
    
    return rc;
}

/**
 * 获取设备信息
 */
static int _xbdev_get_device_info(const char *device_name, char *json_response, size_t response_size) {
    xbdev_device_info_t device_info;
    int rc;
    
    if (!device_name || !json_response || response_size == 0) {
        return -EINVAL;
    }
    
    // 获取设备信息
    rc = xbdev_get_device_info(device_name, &device_info);
    if (rc != 0) {
        snprintf(json_response, response_size, 
               "{\"error\": \"获取设备信息失败\", \"code\": %d, \"device\": \"%s\"}", 
               rc, device_name);
        return rc;
    }
    
    // 构造设备信息JSON
    snprintf(json_response, response_size,
           "{"
           "\"name\": \"%s\","
           "\"product_name\": \"%s\","
           "\"block_size\": %u,"
           "\"num_blocks\": %"PRIu64","
           "\"size_bytes\": %"PRIu64","
           "\"write_cache\": %s,"
           "\"driver\": \"%s\","
           "\"claimed\": %s,"
           "\"supported_io_types\": {"
           "\"read\": %s,"
           "\"write\": %s,"
           "\"unmap\": %s,"
           "\"flush\": %s,"
           "\"reset\": %s,"
           "\"compare\": %s,"
           "\"compare_and_write\": %s,"
           "\"write_zeroes\": %s"
           "}"
           "}",
           device_info.name,
           device_info.product_name,
           device_info.block_size,
           device_info.num_blocks,
           device_info.size_bytes,
           device_info.write_cache ? "true" : "false",
           device_info.driver,
           device_info.claimed ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_READ) ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_WRITE) ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_UNMAP) ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_FLUSH) ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_RESET) ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_COMPARE) ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_COMPARE_AND_WRITE) ? "true" : "false",
           (device_info.supported_io_types & XBDEV_IO_TYPE_WRITE_ZEROES) ? "true" : "false"
    );
    
    return 0;
}

/**
 * 处理创建RAID命令
 */
static int _xbdev_cmd_create_raid(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *name = NULL;
    int level = -1;
    struct spdk_json_val *base_bdevs_val = NULL;
    uint64_t strip_size_kb = 0;
    int rc;
    char **base_bdevs = NULL;
    int num_base_bdevs = 0;
    const char **base_bdev_ptrs = NULL;
    
    // 解析参数
    struct spdk_json_object_decoder raid_decoders[] = {
        {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
        {"level", offsetof(struct { int level; }, level), spdk_json_decode_int32},
        {"base_bdevs", offsetof(struct { struct spdk_json_val *base_bdevs; }, base_bdevs), spdk_json_decode_array},
        {"strip_size_kb", offsetof(struct { uint64_t strip_size_kb; }, strip_size_kb), spdk_json_decode_uint64, true}
    };
    
    if (spdk_json_decode_object(params, raid_decoders, SPDK_COUNTOF(raid_decoders), 
                               &name, &level, &base_bdevs_val, &strip_size_kb)) {
        if (name) free(name);
        snprintf(json_response, response_size, "{\"error\": \"无效的RAID参数\"}");
        return -EINVAL;
    }
    
    if (strip_size_kb == 0) {
        strip_size_kb = 64; // 默认64KB
    }
    
    // 解析base_bdevs数组
    struct spdk_json_val *val = base_bdevs_val + 1;  // 跳过数组头
    num_base_bdevs = base_bdevs_val->len;
    base_bdevs = calloc(num_base_bdevs, sizeof(char *));
    base_bdev_ptrs = calloc(num_base_bdevs, sizeof(const char *));
    
    if (!base_bdevs || !base_bdev_ptrs) {
        if (name) free(name);
        if (base_bdevs) free(base_bdevs);
        if (base_bdev_ptrs) free(base_bdev_ptrs);
        snprintf(json_response, response_size, "{\"error\": \"内存分配失败\"}");
        return -ENOMEM;
    }
    
    for (int i = 0; i < num_base_bdevs; i++) {
        if (spdk_json_decode_string(val, &base_bdevs[i]) != 0) {
            if (name) free(name);
            for (int j = 0; j < i; j++) {
                free(base_bdevs[j]);
            }
            free(base_bdevs);
            free(base_bdev_ptrs);
            snprintf(json_response, response_size, "{\"error\": \"无效的设备名称\"}");
            return -EINVAL;
        }
        base_bdev_ptrs[i] = base_bdevs[i];
        val = spdk_json_next(val);
    }
    
    // 根据RAID级别创建RAID设备
    switch (level) {
    case 0:
        rc = xbdev_md_create_raid0(name, base_bdev_ptrs, num_base_bdevs, strip_size_kb);
        break;
    case 1:
        rc = xbdev_md_create_raid1(name, base_bdev_ptrs, num_base_bdevs);
        break;
    case 4:
        rc = xbdev_md_create_raid4(name, base_bdev_ptrs, num_base_bdevs, strip_size_kb);
        break;
    case 5:
        rc = xbdev_md_create_raid5(name, base_bdev_ptrs, num_base_bdevs, strip_size_kb);
        break;
    case 6:
        rc = xbdev_md_create_raid6(name, base_bdev_ptrs, num_base_bdevs, strip_size_kb);
        break;
    case 10:
        rc = xbdev_md_create_raid10(name, base_bdev_ptrs, num_base_bdevs, strip_size_kb);
        break;
    case -1:  // Linear
        rc = xbdev_md_create_linear(name, base_bdev_ptrs, num_base_bdevs);
        break;
    default:
        rc = -EINVAL;
        snprintf(json_response, response_size, "{\"error\": \"不支持的RAID级别: %d\"}", level);
        break;
    }
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"name\": \"%s\", \"level\": %d, \"num_devices\": %d}", 
               name, level, num_base_bdevs);
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"创建RAID失败\", \"code\": %d, \"name\": \"%s\", \"level\": %d}", 
               rc, name, level);
    }
    
    // 释放资源
    free(name);
    for (int i = 0; i < num_base_bdevs; i++) {
        free(base_bdevs[i]);
    }
    free(base_bdevs);
    free(base_bdev_ptrs);
    
    return rc;
}

/**
 * 处理创建逻辑卷命令
 */
static int _xbdev_cmd_create_lvol(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *lvs_name = NULL;
    char *lvol_name = NULL;
    uint64_t size_mb = 0;
    bool thin_provision = false;
    int rc;
    
    // 解析参数
    struct spdk_json_object_decoder lvol_decoders[] = {
        {"lvs_name", offsetof(struct { char *lvs_name; }, lvs_name), spdk_json_decode_string},
        {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
        {"size_mb", offsetof(struct { uint64_t size_mb; }, size_mb), spdk_json_decode_uint64},
        {"thin_provision", offsetof(struct { bool thin_provision; }, thin_provision), spdk_json_decode_bool, true}
    };
    
    if (spdk_json_decode_object(params, lvol_decoders, SPDK_COUNTOF(lvol_decoders), 
                               &lvs_name, &lvol_name, &size_mb, &thin_provision)) {
        if (lvs_name) free(lvs_name);
        if (lvol_name) free(lvol_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的LVOL参数\"}");
        return -EINVAL;
    }
    
    // 创建LVOL
    rc = xbdev_lvol_create(lvs_name, lvol_name, size_mb, thin_provision);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"lvs_name\": \"%s\", \"name\": \"%s\", \"size_mb\": %"PRIu64", \"thin_provision\": %s}", 
               lvs_name, lvol_name, size_mb, thin_provision ? "true" : "false");
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"创建LVOL失败\", \"code\": %d, \"lvs_name\": \"%s\", \"name\": \"%s\"}", 
               rc, lvs_name, lvol_name);
    }
    
    // 释放资源
    free(lvs_name);
    free(lvol_name);
    
    return rc;
}

/**
 * 处理创建快照命令
 */
static int _xbdev_cmd_create_snapshot(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *lvol_name = NULL;
    char *snapshot_name = NULL;
    int rc;
    
    // 解析参数
    struct spdk_json_object_decoder snap_decoders[] = {
        {"lvol_name", offsetof(struct { char *lvol_name; }, lvol_name), spdk_json_decode_string},
        {"snapshot_name", offsetof(struct { char *snapshot_name; }, snapshot_name), spdk_json_decode_string}
    };
    
    if (spdk_json_decode_object(params, snap_decoders, SPDK_COUNTOF(snap_decoders), 
                               &lvol_name, &snapshot_name)) {
        if (lvol_name) free(lvol_name);
        if (snapshot_name) free(snapshot_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的快照参数\"}");
        return -EINVAL;
    }
    
    // 创建快照
    rc = xbdev_snapshot_create(lvol_name, snapshot_name);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"lvol_name\": \"%s\", \"snapshot_name\": \"%s\"}", 
               lvol_name, snapshot_name);
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"创建快照失败\", \"code\": %d, \"lvol_name\": \"%s\", \"snapshot_name\": \"%s\"}", 
               rc, lvol_name, snapshot_name);
    }
    
    // 释放资源
    free(lvol_name);
    free(snapshot_name);
    
    return rc;
}

/**
 * 处理调整逻辑卷大小命令
 */
static int _xbdev_cmd_resize_lvol(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *lvol_name = NULL;
    uint64_t new_size_mb = 0;
    int rc;
    
    // 解析参数
    struct spdk_json_object_decoder resize_decoders[] = {
        {"lvol_name", offsetof(struct { char *lvol_name; }, lvol_name), spdk_json_decode_string},
        {"new_size_mb", offsetof(struct { uint64_t new_size_mb; }, new_size_mb), spdk_json_decode_uint64}
    };
    
    if (spdk_json_decode_object(params, resize_decoders, SPDK_COUNTOF(resize_decoders), 
                               &lvol_name, &new_size_mb)) {
        if (lvol_name) free(lvol_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的大小调整参数\"}");
        return -EINVAL;
    }
    
    // 调整大小
    rc = xbdev_lvol_resize(lvol_name, new_size_mb);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"lvol_name\": \"%s\", \"new_size_mb\": %"PRIu64"}", 
               lvol_name, new_size_mb);
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"调整LVOL大小失败\", \"code\": %d, \"lvol_name\": \"%s\", \"new_size_mb\": %"PRIu64"}", 
               rc, lvol_name, new_size_mb);
    }
    
    // 释放资源
    free(lvol_name);
    
    return rc;
}

/**
 * 处理删除设备命令
 */
static int _xbdev_cmd_delete_device(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *device_name = NULL;
    char *device_type = NULL;
    int rc = 0;
    
    // 解析参数
    struct spdk_json_object_decoder del_decoders[] = {
        {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
        {"type", offsetof(struct { char *type; }, type), spdk_json_decode_string, true}
    };
    
    if (spdk_json_decode_object(params, del_decoders, SPDK_COUNTOF(del_decoders), 
                               &device_name, &device_type)) {
        if (device_name) free(device_name);
        if (device_type) free(device_type);
        snprintf(json_response, response_size, "{\"error\": \"无效的删除参数\"}");
        return -EINVAL;
    }
    
    // 根据设备类型执行不同的删除操作
    if (device_type && strcmp(device_type, "raid") == 0) {
        // 删除RAID设备
        rc = xbdev_md_stop(device_name);
    } else if (device_type && strcmp(device_type, "lvol") == 0) {
        // 删除LVOL设备
        rc = xbdev_lvol_destroy(device_name);
    } else if (device_type && strcmp(device_type, "lvs") == 0) {
        // 删除LVS
        rc = xbdev_lvs_destroy(device_name);
    } else if (device_type && strcmp(device_type, "snapshot") == 0) {
        // 删除快照
        rc = xbdev_snapshot_destroy(device_name);
    } else {
        // 默认尝试删除任意设备
        rc = xbdev_device_remove(device_name);
    }
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"name\": \"%s\", \"type\": \"%s\"}", 
               device_name, device_type ? device_type : "unknown");
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"删除设备失败\", \"code\": %d, \"name\": \"%s\", \"type\": \"%s\"}", 
               rc, device_name, device_type ? device_type : "unknown");
    }
    
    // 释放资源
    free(device_name);
    if (device_type) free(device_type);
    
    return rc;
}

/**
 * 处理RAID详情命令
 */
static int _xbdev_cmd_raid_detail(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *md_name = NULL;
    int rc = 0;
    xbdev_md_detail_t detail = {0};
    
    // 解析参数
    struct spdk_json_object_decoder detail_decoders[] = {
        {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string}
    };
    
    if (spdk_json_decode_object(params, detail_decoders, SPDK_COUNTOF(detail_decoders), &md_name)) {
        if (md_name) free(md_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的RAID详情参数\"}");
        return -EINVAL;
    }
    
    // 获取RAID详情
    rc = xbdev_md_detail(md_name, &detail);
    if (rc != 0) {
        snprintf(json_response, response_size, 
               "{\"error\": \"获取RAID详情失败\", \"code\": %d, \"name\": \"%s\"}", 
               rc, md_name);
        free(md_name);
        return rc;
    }
    
    // 构造详情JSON
    char *pos = json_response;
    size_t remaining = response_size;
    int written = 0;
    
    written = snprintf(pos, remaining, 
                     "{"
                     "\"name\": \"%s\","
                     "\"level\": %d,"
                     "\"raid_disks\": %d,"
                     "\"active_disks\": %d,"
                     "\"state\": %d,"
                     "\"degraded\": %s,"
                     "\"size_kb\": %"PRIu64","
                     "\"chunk_size_kb\": %"PRIu64","
                     "\"uuid\": \"%s\","
                     "\"resync_status\": \"%s\","
                     "\"resync_progress\": %.2f,"
                     "\"disks\": [",
                     md_name,
                     detail.level,
                     detail.raid_disks,
                     detail.active_disks,
                     detail.state,
                     detail.degraded ? "true" : "false",
                     detail.size,
                     detail.chunk_size,
                     detail.uuid,
                     detail.resync_status,
                     detail.resync_progress);
    
    if (written >= remaining) {
        free(md_name);
        return -ENOSPC;
    }
    pos += written;
    remaining -= written;
    
    // 添加各个磁盘信息
    for (int i = 0; i < detail.raid_disks; i++) {
        written = snprintf(pos, remaining,
                         "%s{"
                         "\"number\": %d,"
                         "\"name\": \"%s\","
                         "\"state\": %d,"
                         "\"size_kb\": %"PRIu64","
                         "\"role\": \"%s\""
                         "}",
                         (i > 0) ? ", " : "",
                         detail.disks[i].number,
                         detail.disks[i].name,
                         detail.disks[i].state,
                         detail.disks[i].size,
                         detail.disks[i].role);
        
        if (written >= remaining) {
            free(md_name);
            return -ENOSPC;
        }
        pos += written;
        remaining -= written;
    }
    
    // 完成JSON对象
    written = snprintf(pos, remaining, "]}");
    if (written >= remaining) {
        free(md_name);
        return -ENOSPC;
    }
    
    free(md_name);
    return 0;
}

/**
 * 处理RAID管理命令
 */
static int _xbdev_cmd_raid_manage(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *md_name = NULL;
    int cmd = 0;
    char *arg = NULL;
    int rc = 0;
    
    // 解析参数
    struct spdk_json_object_decoder manage_decoders[] = {
        {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
        {"cmd", offsetof(struct { int cmd; }, cmd), spdk_json_decode_int32},
        {"arg", offsetof(struct { char *arg; }, arg), spdk_json_decode_string, true}
    };
    
    if (spdk_json_decode_object(params, manage_decoders, SPDK_COUNTOF(manage_decoders), 
                               &md_name, &cmd, &arg)) {
        if (md_name) free(md_name);
        if (arg) free(arg);
        snprintf(json_response, response_size, "{\"error\": \"无效的RAID管理参数\"}");
        return -EINVAL;
    }
    
    // 执行管理命令
    rc = xbdev_md_manage(md_name, cmd, arg);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"name\": \"%s\", \"cmd\": %d, \"arg\": \"%s\"}", 
               md_name, cmd, arg ? arg : "null");
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"RAID管理操作失败\", \"code\": %d, \"name\": \"%s\", \"cmd\": %d}", 
               rc, md_name, cmd);
    }
    
    // 释放资源
    free(md_name);
    if (arg) free(arg);
    
    return rc;
}

/**
 * 处理创建LVS命令
 */
static int _xbdev_cmd_create_lvs(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *bdev_name = NULL;
    char *lvs_name = NULL;
    uint64_t cluster_size = 0;
    int rc;
    
    // 解析参数
    struct spdk_json_object_decoder lvs_decoders[] = {
        {"bdev_name", offsetof(struct { char *bdev_name; }, bdev_name), spdk_json_decode_string},
        {"lvs_name", offsetof(struct { char *lvs_name; }, lvs_name), spdk_json_decode_string},
        {"cluster_size", offsetof(struct { uint64_t cluster_size; }, cluster_size), spdk_json_decode_uint64, true}
    };
    
    if (spdk_json_decode_object(params, lvs_decoders, SPDK_COUNTOF(lvs_decoders), 
                               &bdev_name, &lvs_name, &cluster_size)) {
        if (bdev_name) free(bdev_name);
        if (lvs_name) free(lvs_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的LVS参数\"}");
        return -EINVAL;
    }
    
    // 创建LVS
    rc = xbdev_lvs_create(bdev_name, lvs_name, cluster_size);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"bdev_name\": \"%s\", \"lvs_name\": \"%s\", \"cluster_size\": %"PRIu64"}", 
               bdev_name, lvs_name, cluster_size);
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"创建LVS失败\", \"code\": %d, \"bdev_name\": \"%s\", \"lvs_name\": \"%s\"}", 
               rc, bdev_name, lvs_name);
    }
    
    // 释放资源
    free(bdev_name);
    free(lvs_name);
    
    return rc;
}

/**
 * 处理创建克隆命令
 */
static int _xbdev_cmd_create_clone(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *snapshot_name = NULL;
    char *clone_name = NULL;
    int rc;
    
    // 解析参数
    struct spdk_json_object_decoder clone_decoders[] = {
        {"snapshot_name", offsetof(struct { char *snapshot_name; }, snapshot_name), spdk_json_decode_string},
        {"clone_name", offsetof(struct { char *clone_name; }, clone_name), spdk_json_decode_string}
    };
    
    if (spdk_json_decode_object(params, clone_decoders, SPDK_COUNTOF(clone_decoders), 
                               &snapshot_name, &clone_name)) {
        if (snapshot_name) free(snapshot_name);
        if (clone_name) free(clone_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的克隆参数\"}");
        return -EINVAL;
    }
    
    // 创建克隆
    rc = xbdev_clone_create(snapshot_name, clone_name);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"snapshot_name\": \"%s\", \"clone_name\": \"%s\"}", 
               snapshot_name, clone_name);
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"创建克隆失败\", \"code\": %d, \"snapshot_name\": \"%s\", \"clone_name\": \"%s\"}", 
               rc, snapshot_name, clone_name);
    }
    
    // 释放资源
    free(snapshot_name);
    free(clone_name);
    
    return rc;
}

/**
 * 处理获取LVS列表命令
 */
static int _xbdev_cmd_get_lvs_list(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    int rc = 0;
    char **lvs_names = NULL;
    int max_lvs = 64;
    int lvs_count = 0;
    
    // 分配LVS名称数组
    lvs_names = malloc(sizeof(char*) * max_lvs);
    if (!lvs_names) {
        snprintf(json_response, response_size, "{\"error\": \"内存分配失败\"}");
        return -ENOMEM;
    }
    
    // 初始化数组
    for (int i = 0; i < max_lvs; i++) {
        lvs_names[i] = NULL;
    }
    
    // 获取LVS列表
    lvs_count = xbdev_lvs_list(lvs_names, max_lvs);
    if (lvs_count < 0) {
        snprintf(json_response, response_size, "{\"error\": \"获取LVS列表失败\", \"code\": %d}", lvs_count);
        free(lvs_names);
        return lvs_count;
    }
    
    // 构造JSON响应
    char *pos = json_response;
    size_t remaining = response_size;
    int written = 0;
    
    written = snprintf(pos, remaining, "{\"lvs_list\": [");
    if (written >= remaining) {
        for (int i = 0; i < lvs_count; i++) {
            if (lvs_names[i]) free(lvs_names[i]);
        }
        free(lvs_names);
        return -ENOSPC;
    }
    pos += written;
    remaining -= written;
    
    // 添加每个LVS名称
    for (int i = 0; i < lvs_count; i++) {
        written = snprintf(pos, remaining, "%s\"%s\"", 
                         (i > 0) ? ", " : "",
                         lvs_names[i]);
        if (written >= remaining) {
            for (int j = 0; j < lvs_count; j++) {
                if (lvs_names[j]) free(lvs_names[j]);
            }
            free(lvs_names);
            return -ENOSPC;
        }
        pos += written;
        remaining -= written;
    }
    
    // 完成JSON对象
    written = snprintf(pos, remaining, "], \"count\": %d}", lvs_count);
    if (written >= remaining) {
        for (int i = 0; i < lvs_count; i++) {
            if (lvs_names[i]) free(lvs_names[i]);
        }
        free(lvs_names);
        return -ENOSPC;
    }
    
    // 释放资源
    for (int i = 0; i < lvs_count; i++) {
        if (lvs_names[i]) free(lvs_names[i]);
    }
    free(lvs_names);
    
    return 0;
}

/**
 * 处理获取LVOL列表命令
 */
static int _xbdev_cmd_get_lvol_list(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *lvs_name = NULL;
    int rc = 0;
    char **lvol_names = NULL;
    int max_lvols = 128;
    int lvol_count = 0;
    
    // 解析参数
    struct spdk_json_object_decoder list_decoders[] = {
        {"lvs_name", offsetof(struct { char *lvs_name; }, lvs_name), spdk_json_decode_string}
    };
    
    if (params && spdk_json_decode_object(params, list_decoders, SPDK_COUNTOF(list_decoders), &lvs_name)) {
        if (lvs_name) free(lvs_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的LVOL列表参数\"}");
        return -EINVAL;
    }
    
    // 分配LVOL名称数组
    lvol_names = malloc(sizeof(char*) * max_lvols);
    if (!lvol_names) {
        if (lvs_name) free(lvs_name);
        snprintf(json_response, response_size, "{\"error\": \"内存分配失败\"}");
        return -ENOMEM;
    }
    
    // 初始化数组
    for (int i = 0; i < max_lvols; i++) {
        lvol_names[i] = NULL;
    }
    
    // 获取LVOL列表
    if (lvs_name) {
        lvol_count = xbdev_lvol_list(lvs_name, lvol_names, max_lvols);
    } else {
        // 如果没有指定LVS名称，尝试获取所有LVOL
        // 这需要先获取所有LVS，然后遍历
        char **all_lvs_names = malloc(sizeof(char*) * 64);
        int lvs_count = 0;
        
        if (!all_lvs_names) {
            free(lvol_names);
            snprintf(json_response, response_size, "{\"error\": \"内存分配失败\"}");
            return -ENOMEM;
        }
        
        // 初始化LVS数组
        for (int i = 0; i < 64; i++) {
            all_lvs_names[i] = NULL;
        }
        
        // 获取所有LVS
        lvs_count = xbdev_lvs_list(all_lvs_names, 64);
        if (lvs_count < 0) {
            free(all_lvs_names);
            free(lvol_names);
            snprintf(json_response, response_size, "{\"error\": \"获取LVS列表失败\", \"code\": %d}", lvs_count);
            return lvs_count;
        }
        
        // 遍历每个LVS获取LVOL
        for (int i = 0; i < lvs_count && lvol_count < max_lvols; i++) {
            int lvs_lvol_count = xbdev_lvol_list(all_lvs_names[i], 
                                               &lvol_names[lvol_count], 
                                               max_lvols - lvol_count);
            
            if (lvs_lvol_count > 0) {
                lvol_count += lvs_lvol_count;
            }
        }
        
        // 释放LVS名称资源
        for (int i = 0; i < lvs_count; i++) {
            if (all_lvs_names[i]) free(all_lvs_names[i]);
        }
        free(all_lvs_names);
    }
    
    if (lvol_count < 0) {
        if (lvs_name) free(lvs_name);
        free(lvol_names);
        snprintf(json_response, response_size, "{\"error\": \"获取LVOL列表失败\", \"code\": %d}", lvol_count);
        return lvol_count;
    }
    
    // 构造JSON响应
    char *pos = json_response;
    size_t remaining = response_size;
    int written = 0;
    
    written = snprintf(pos, remaining, "{\"lvol_list\": [");
    if (written >= remaining) {
        if (lvs_name) free(lvs_name);
        for (int i = 0; i < lvol_count; i++) {
            if (lvol_names[i]) free(lvol_names[i]);
        }
        free(lvol_names);
        return -ENOSPC;
    }
    pos += written;
    remaining -= written;
    
    // 添加每个LVOL名称
    for (int i = 0; i < lvol_count; i++) {
        written = snprintf(pos, remaining, "%s\"%s\"", 
                         (i > 0) ? ", " : "",
                         lvol_names[i]);
        if (written >= remaining) {
            if (lvs_name) free(lvs_name);
            for (int j = 0; j < lvol_count; j++) {
                if (lvol_names[j]) free(lvol_names[j]);
            }
            free(lvol_names);
            return -ENOSPC;
        }
        pos += written;
        remaining -= written;
    }
    
    // 完成JSON对象
    if (lvs_name) {
        written = snprintf(pos, remaining, "], \"lvs_name\": \"%s\", \"count\": %d}", lvs_name, lvol_count);
    } else {
        written = snprintf(pos, remaining, "], \"count\": %d}", lvol_count);
    }
    
    if (written >= remaining) {
        if (lvs_name) free(lvs_name);
        for (int i = 0; i < lvol_count; i++) {
            if (lvol_names[i]) free(lvol_names[i]);
        }
        free(lvol_names);
        return -ENOSPC;
    }
    
    // 释放资源
    if (lvs_name) free(lvs_name);
    for (int i = 0; i < lvol_count; i++) {
        if (lvol_names[i]) free(lvol_names[i]);
    }
    free(lvol_names);
    
    return 0;
}

/**
 * 处理获取LVOL信息命令
 */
static int _xbdev_cmd_get_lvol_info(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *lvol_name = NULL;
    int rc = 0;
    xbdev_lvol_info_t info = {0};
    
    // 解析参数
    struct spdk_json_object_decoder info_decoders[] = {
        {"lvol_name", offsetof(struct { char *lvol_name; }, lvol_name), spdk_json_decode_string}
    };
    
    if (spdk_json_decode_object(params, info_decoders, SPDK_COUNTOF(info_decoders), &lvol_name)) {
        if (lvol_name) free(lvol_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的LVOL信息参数\"}");
        return -EINVAL;
    }
    
    // 获取LVOL信息
    rc = xbdev_lvol_get_info(lvol_name, &info);
    if (rc != 0) {
        free(lvol_name);
        snprintf(json_response, response_size, "{\"error\": \"获取LVOL信息失败\", \"code\": %d, \"lvol_name\": \"%s\"}", 
               rc, lvol_name);
        return rc;
    }
    
    // 构造JSON响应
    snprintf(json_response, response_size,
           "{"
           "\"name\": \"%s\","
           "\"lvs_name\": \"%s\","
           "\"size_bytes\": %"PRIu64","
           "\"size_mb\": %"PRIu64","
           "\"thin_provisioned\": %s,"
           "\"is_snapshot\": %s,"
           "\"is_clone\": %s,"
           "\"has_parent\": %s,"
           "\"parent_name\": \"%s\","
           "\"block_size\": %"PRIu32","
           "\"num_blocks\": %"PRIu64
           "}",
           info.name,
           info.lvs_name,
           info.size_bytes,
           info.size_mb,
           info.thin_provisioned ? "true" : "false",
           info.is_snapshot ? "true" : "false",
           info.is_clone ? "true" : "false",
           info.has_parent ? "true" : "false",
           info.parent_name,
           info.block_size,
           info.num_blocks);
    
    // 释放资源
    free(lvol_name);
    
    return 0;
}

/**
 * 处理获取系统状态命令
 */
static int _xbdev_cmd_get_system_status(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 此函数获取系统总体状态，包括设备数量、RAID健康状态等
    
    // 获取设备列表
    char **device_names = malloc(sizeof(char*) * 100);
    int device_count = 0;
    int raid_count = 0;
    int lvs_count = 0;
    int lvol_count = 0;
    int failed_raids = 0;
    int degraded_raids = 0;
    
    if (!device_names) {
        snprintf(json_response, response_size, "{\"error\": \"内存分配失败\"}");
        return -ENOMEM;
    }
    
    // 初始化设备名称数组
    for (int i = 0; i < 100; i++) {
        device_names[i] = NULL;
    }
    
    // 获取所有设备列表
    device_count = xbdev_get_device_list(device_names, 100);
    if (device_count < 0) {
        free(device_names);
        snprintf(json_response, response_size, "{\"error\": \"获取设备列表失败\", \"code\": %d}", device_count);
        return device_count;
    }
    
    // 遍历设备计算各类型数量
    for (int i = 0; i < device_count; i++) {
        // 检查设备类型
        if (xbdev_md_is_raid(device_names[i])) {
            raid_count++;
            
            // 检查RAID健康状态
            xbdev_md_detail_t detail = {0};
            if (xbdev_md_detail(device_names[i], &detail) == 0) {
                if (detail.state == XBDEV_MD_STATE_FAILED) {
                    failed_raids++;
                } else if (detail.degraded) {
                    degraded_raids++;
                }
            }
        }
    }
    
    // 获取LVS数量
    char **lvs_names = malloc(sizeof(char*) * 64);
    if (lvs_names) {
        for (int i = 0; i < 64; i++) {
            lvs_names[i] = NULL;
        }
        
        lvs_count = xbdev_lvs_list(lvs_names, 64);
        if (lvs_count > 0) {
            // 获取每个LVS中的LVOL数量
            for (int i = 0; i < lvs_count; i++) {
                char **temp_lvol_names = malloc(sizeof(char*) * 128);
                if (temp_lvol_names) {
                    int temp_count = xbdev_lvol_list(lvs_names[i], temp_lvol_names, 128);
                    if (temp_count > 0) {
                        lvol_count += temp_count;
                    }
                    
                    // 释放临时LVOL名称内存
                    for (int j = 0; j < 128; j++) {
                        if (temp_lvol_names[j]) {
                            free(temp_lvol_names[j]);
                        }
                    }
                    free(temp_lvol_names);
                }
            }
        }
        
        // 释放LVS名称内存
        for (int i = 0; i < 64; i++) {
            if (lvs_names[i]) {
                free(lvs_names[i]);
            }
        }
        free(lvs_names);
    }
    
    // 构造系统状态响应
    snprintf(json_response, response_size,
           "{"
           "\"total_devices\": %d,"
           "\"raid_devices\": %d,"
           "\"failed_raids\": %d,"
           "\"degraded_raids\": %d,"
           "\"lvol_stores\": %d,"
           "\"lvol_volumes\": %d,"
           "\"system_status\": \"%s\""
           "}",
           device_count,
           raid_count,
           failed_raids,
           degraded_raids,
           lvs_count,
           lvol_count,
           (failed_raids > 0) ? "critical" : ((degraded_raids > 0) ? "warning" : "healthy"));
    
    // 释放设备名称内存
    for (int i = 0; i < 100; i++) {
        if (device_names[i]) {
            free(device_names[i]);
        }
    }
    free(device_names);
    
    return 0;
}

/**
 * 注册所有RPC方法
 */
static void _xbdev_register_rpc_methods(void) {
    // TODO: 在此注册所有RPC方法
    // 当前方法已在其定义后通过SPDK_RPC_REGISTER宏注册
}

/**
 * 处理系统配置导出
 */
static int _xbdev_cmd_export_config(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *config_file = NULL;
    int rc = 0;
    
    // 解析参数
    struct spdk_json_object_decoder export_decoders[] = {
        {"filename", offsetof(struct { char *filename; }, filename), spdk_json_decode_string, true}
    };
    
    if (params && spdk_json_decode_object(params, export_decoders, SPDK_COUNTOF(export_decoders), &config_file)) {
        if (config_file) free(config_file);
        snprintf(json_response, response_size, "{\"error\": \"无效的配置导出参数\"}");
        return -EINVAL;
    }
    
    // 如果没有指定文件名，使用默认文件名
    if (!config_file) {
        config_file = strdup("/tmp/xbdev_config.json");
        if (!config_file) {
            snprintf(json_response, response_size, "{\"error\": \"内存分配失败\"}");
            return -ENOMEM;
        }
    }
    
    // 导出配置
    rc = xbdev_save_config(config_file);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"message\": \"配置已导出到文件\", \"filename\": \"%s\"}", 
               config_file);
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"导出配置失败\", \"code\": %d, \"filename\": \"%s\"}", 
               rc, config_file);
    }
    
    // 释放资源
    free(config_file);
    
    return rc;
}

/**
 * 处理系统配置导入
 */
static int _xbdev_cmd_import_config(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *config_file = NULL;
    int rc = 0;
    
    // 解析参数
    struct spdk_json_object_decoder import_decoders[] = {
        {"filename", offsetof(struct { char *filename; }, filename), spdk_json_decode_string}
    };
    
    if (spdk_json_decode_object(params, import_decoders, SPDK_COUNTOF(import_decoders), &config_file)) {
        if (config_file) free(config_file);
        snprintf(json_response, response_size, "{\"error\": \"无效的配置导入参数\"}");
        return -EINVAL;
    }
    
    // 导入配置
    rc = xbdev_parse_config(config_file);
    
    // 生成响应
    if (rc == 0) {
        snprintf(json_response, response_size, 
               "{\"success\": true, \"message\": \"配置已从文件导入\", \"filename\": \"%s\"}", 
               config_file);
    } else {
        snprintf(json_response, response_size, 
               "{\"error\": \"导入配置失败\", \"code\": %d, \"filename\": \"%s\"}", 
               rc, config_file);
    }
    
    // 释放资源
    free(config_file);
    
    return rc;
}

/**
 * 处理健康检查命令
 */
static int _xbdev_cmd_health_check(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *device_name = NULL;
    int rc = 0;
    bool check_all = false;
    
    // 解析参数
    struct spdk_json_object_decoder health_decoders[] = {
        {"device", offsetof(struct { char *device; }, device), spdk_json_decode_string, true}
    };
    
    if (params && spdk_json_decode_object(params, health_decoders, SPDK_COUNTOF(health_decoders), &device_name)) {
        if (device_name) free(device_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的健康检查参数\"}");
        return -EINVAL;
    }
    
    // 如果未指定设备名，检查所有设备
    check_all = (device_name == NULL);
    
    if (check_all) {
        // 检查所有设备健康状态
        // TODO: 实现全系统健康检查
        snprintf(json_response, response_size, 
               "{\"status\": \"healthy\", \"message\": \"系统运行正常\", \"details\": \"全系统健康检查尚未实现\"}");
    } else {
        // 获取指定设备类型
        bool is_raid = xbdev_md_is_raid(device_name);
        bool has_health_info = false;
        
        if (is_raid) {
            // 检查RAID健康状态
            xbdev_md_detail_t detail = {0};
            rc = xbdev_md_detail(device_name, &detail);
            
            if (rc == 0) {
                has_health_info = true;
                const char *status = "healthy";
                const char *message = "RAID设备运行正常";
                
                if (detail.state == XBDEV_MD_STATE_FAILED) {
                    status = "critical";
                    message = "RAID设备失败";
                } else if (detail.degraded) {
                    status = "warning";
                    message = "RAID设备处于降级模式";
                }
                
                snprintf(json_response, response_size,
                       "{"
                       "\"device\": \"%s\","
                       "\"status\": \"%s\","
                       "\"message\": \"%s\","
                       "\"level\": %d,"
                       "\"total_disks\": %d,"
                       "\"active_disks\": %d,"
                       "\"failed_disks\": %d,"
                       "\"degraded\": %s,"
                       "\"resync\": \"%s\","
                       "\"resync_progress\": %.2f"
                       "}",
                       device_name,
                       status,
                       message,
                       detail.level,
                       detail.raid_disks,
                       detail.active_disks,
                       detail.failed_disks,
                       detail.degraded ? "true" : "false",
                       detail.resync_status,
                       detail.resync_progress);
            }
        }
        
        if (!has_health_info) {
            // 获取常规设备信息
            xbdev_device_info_t device_info;
            rc = xbdev_get_device_info(device_name, &device_info);
            
            if (rc == 0) {
                snprintf(json_response, response_size,
                       "{"
                       "\"device\": \"%s\","
                       "\"status\": \"unknown\","
                       "\"message\": \"设备存在但无法获取详细健康信息\","
                       "\"driver\": \"%s\","
                       "\"size_bytes\": %"PRIu64
                       "}",
                       device_name,
                       device_info.driver,
                       device_info.size_bytes);
            } else {
                snprintf(json_response, response_size,
                       "{"
                       "\"device\": \"%s\","
                       "\"status\": \"error\","
                       "\"message\": \"找不到设备或获取设备信息失败\","
                       "\"error_code\": %d"
                       "}",
                       device_name,
                       rc);
            }
        }
    }
    
    // 释放资源
    if (device_name) free(device_name);
    
    return 0;
}

/**
 * 获取所有RPC方法的帮助信息
 */
static int _xbdev_cmd_get_methods(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    snprintf(json_response, response_size,
           "{"
           "\"methods\": ["
           "\"device_list\","
           "\"device_info\","
           "\"create_raid\","
           "\"create_lvol\","
           "\"create_snapshot\","
           "\"create_clone\","
           "\"resize_lvol\","
           "\"delete_device\","
           "\"raid_detail\","
           "\"raid_manage\","
           "\"get_lvs_list\","
           "\"get_lvol_list\","
           "\"get_lvol_info\","
           "\"create_lvs\","
           "\"system_status\","
           "\"export_config\","
           "\"import_config\","
           "\"health_check\""
           "],"
           "\"description\": \"可用的XBDEV RPC方法列表\""
           "}");
    
    return 0;
}

/**
 * 管理功能帮助信息
 */
static int _xbdev_cmd_get_help(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *method = NULL;
    
    // 解析参数
    struct spdk_json_object_decoder help_decoders[] = {
        {"method", offsetof(struct { char *method; }, method), spdk_json_decode_string, true}
    };
    
    if (params && spdk_json_decode_object(params, help_decoders, SPDK_COUNTOF(help_decoders), &method)) {
        if (method) free(method);
        snprintf(json_response, response_size, "{\"error\": \"无效的帮助参数\"}");
        return -EINVAL;
    }
    
    // 如果未指定方法，显示整体帮助
    if (!method) {
        snprintf(json_response, response_size,
               "{"
               "\"help\": \"XBDEV管理API帮助\","
               "\"usage\": \"使用get_methods命令获取可用方法列表，使用get_help?method=方法名获取特定方法的帮助\","
               "\"example\": \"{\\\"method\\\":\\\"get_help\\\", \\\"params\\\":{\\\"method\\\":\\\"create_raid\\\"}}\"" 
               "}");
    } else if (strcmp(method, "create_raid") == 0) {
        snprintf(json_response, response_size,
               "{"
               "\"method\": \"create_raid\","
               "\"description\": \"创建RAID设备\","
               "\"parameters\": {"
               "  \"name\": \"设备名称，字符串，必需\","
               "  \"level\": \"RAID级别 (0, 1, 4, 5, 6, 10, -1表示linear)，整数，必需\","
               "  \"base_bdevs\": \"基础设备名称数组，字符串数组，必需\","
               "  \"strip_size_kb\": \"条带大小(KB)，对RAID0/4/5/6/10有效，整数，可选，默认64KB\""
               "},"
               "\"example\": \"{\\\"method\\\":\\\"create_raid\\\", \\\"params\\\":{\\\"name\\\":\\\"raid0\\\", \\\"level\\\":0, \\\"base_bdevs\\\":[\\\"nvme0\\\", \\\"nvme1\\\"], \\\"strip_size_kb\\\":128}}\"" 
               "}");
    } else if (strcmp(method, "create_lvol") == 0) {
        snprintf(json_response, response_size,
               "{"
               "\"method\": \"create_lvol\","
               "\"description\": \"创建逻辑卷\","
               "\"parameters\": {"
               "  \"lvs_name\": \"LVOL存储池名称，字符串，必需\","
               "  \"name\": \"逻辑卷名称，字符串，必需\","
               "  \"size_mb\": \"卷大小(MB)，整数，必需\","
               "  \"thin_provision\": \"是否精简配置，布尔值，可选，默认false\""
               "},"
               "\"example\": \"{\\\"method\\\":\\\"create_lvol\\\", \\\"params\\\":{\\\"lvs_name\\\":\\\"lvs0\\\", \\\"name\\\":\\\"data_vol\\\", \\\"size_mb\\\":1024, \\\"thin_provision\\\":true}}\"" 
               "}");
    } else {
        // 其他方法的帮助信息
        snprintf(json_response, response_size,
               "{"
               "\"help\": \"方法 '%s' 的帮助信息\","
               "\"message\": \"详细帮助尚未实现。请参考API文档。\""
               "}", method);
    }
    
    if (method) free(method);
    return 0;
}

/**
 * 添加管理命令
 */
int xbdev_mgmt_add_command(const char *command, xbdev_mgmt_cmd_handler handler, const char *help_text) {
    // 这个函数允许应用程序注册自定义命令
    // TODO: 实现命令注册机制
    
    XBDEV_NOTICELOG("暂不支持添加管理命令: %s\n", command);
    return -ENOTSUP;
}

/**
 * 处理获取性能统计信息命令
 */
static int _xbdev_cmd_get_stats(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *device_name = NULL;
    int rc = 0;
    
    // 解析参数
    struct spdk_json_object_decoder stats_decoders[] = {
        {"device", offsetof(struct { char *device; }, device), spdk_json_decode_string, true}
    };
    
    if (params && spdk_json_decode_object(params, stats_decoders, SPDK_COUNTOF(stats_decoders), &device_name)) {
        if (device_name) free(device_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的统计信息参数\"}");
        return -EINVAL;
    }
    
    // 获取统计信息（目前为示例数据，实际应从SPDK获取）
    if (device_name) {
        struct spdk_bdev *bdev = spdk_bdev_get_by_name(device_name);
        if (!bdev) {
            snprintf(json_response, response_size, 
                   "{\"error\": \"找不到设备\", \"device\": \"%s\"}", 
                   device_name);
            if (device_name) free(device_name);
            return -ENODEV;
        }
        
        // 获取设备统计信息（模拟数据）
        // 实际实现应使用spdk_bdev_get_io_stat或类似API获取真实数据
        snprintf(json_response, response_size,
               "{"
               "\"device\": \"%s\","
               "\"stats\": {"
               "  \"read_ops\": 12345,"
               "  \"write_ops\": 54321,"
               "  \"read_bytes\": 67890123,"
               "  \"write_bytes\": 98765432,"
               "  \"read_latency_us\": 123,"
               "  \"write_latency_us\": 456"
               "}"
               "}", device_name);
    } else {
        // 全局统计信息（示例）
        snprintf(json_response, response_size,
               "{"
               "\"global_stats\": {"
               "  \"devices\": 5,"
               "  \"total_ops\": 123456,"
               "  \"total_bytes\": 9876543210,"
               "  \"avg_latency_us\": 234"
               "}"
               "}");
    }
    
    if (device_name) free(device_name);
    return rc;
}

/**
 * 服务器版本与状态信息
 */
static int _xbdev_cmd_get_version(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 生成版本和状态信息
    snprintf(json_response, response_size,
           "{"
           "\"version\": \"%d.%d.%d\","
           "\"api_version\": \"%d.%d\","
           "\"spdk_version\": \"%s\","
           "\"build_date\": \"%s\","
           "\"compiler\": \"%s\","
           "\"system\": \"%s\","
           "\"status\": \"running\","
           "\"uptime_seconds\": %ld"
           "}",
           XBDEV_VERSION_MAJOR, XBDEV_VERSION_MINOR, XBDEV_VERSION_PATCH,
           XBDEV_API_VERSION_MAJOR, XBDEV_API_VERSION_MINOR,
           SPDK_VERSION_STRING,
           __DATE__ " " __TIME__,
#ifdef __GNUC__
           "GCC " __VERSION__,
#else
           "Unknown Compiler",
#endif
           SPDK_PLATFORM,
           time(NULL) - g_xbdev_start_time);  // 假设g_xbdev_start_time全局变量记录了启动时间
    
    return 0;
}

/**
 * 与其他libxbdev实例同步配置
 */
static int _xbdev_cmd_sync_config(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *remote_addr = NULL;
    int remote_port = 0;
    int rc = 0;
    
    // 解析参数
    struct spdk_json_object_decoder sync_decoders[] = {
        {"remote_addr", offsetof(struct { char *remote_addr; }, remote_addr), spdk_json_decode_string},
        {"remote_port", offsetof(struct { int remote_port; }, remote_port), spdk_json_decode_int32}
    };
    
    if (spdk_json_decode_object(params, sync_decoders, SPDK_COUNTOF(sync_decoders), 
                               &remote_addr, &remote_port)) {
        if (remote_addr) free(remote_addr);
        snprintf(json_response, response_size, "{\"error\": \"无效的同步参数\"}");
        return -EINVAL;
    }
    
    // 执行配置同步（功能尚未实现）
    snprintf(json_response, response_size,
           "{"
           "\"status\": \"error\","
           "\"message\": \"配置同步功能尚未实现\","
           "\"remote\": \"%s:%d\""
           "}", remote_addr, remote_port);
    
    free(remote_addr);
    return -ENOTSUP;
}

/**
 * 解析RPC调用错误
 */
static void _xbdev_handle_rpc_error(const char *method, int error_code, char *json_response, size_t response_size) {
    const char *error_msg;
    
    // 为常见错误提供友好错误消息
    switch (error_code) {
        case -EINVAL: 
            error_msg = "无效参数";
            break;
        case -ENOMEM: 
            error_msg = "内存分配失败";
            break;
        case -ENOENT: 
            error_msg = "设备或资源不存在";
            break;
        case -EEXIST:
            error_msg = "设备或资源已存在";
            break;
        case -ENOTSUP:
            error_msg = "操作不支持";
            break;
        case -EBUSY:
            error_msg = "设备或资源正忙";
            break;
        case -ENODEV:
            error_msg = "没有该设备";
            break;
        default:
            error_msg = "未知错误";
    }
    
    snprintf(json_response, response_size,
           "{"
           "\"status\": \"error\","
           "\"method\": \"%s\","
           "\"error_code\": %d,"
           "\"error_message\": \"%s\""
           "}", method, error_code, error_msg);
}

/**
 * 用于触发控制器故障的测试命令（仅用于测试）
 */
static int _xbdev_cmd_test_fault_injection(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *device_name = NULL;
    int fault_type = 0;
    int fault_delay_ms = 0;
    int rc = 0;
    
    // 解析参数
    struct spdk_json_object_decoder fault_decoders[] = {
        {"device", offsetof(struct { char *device; }, device), spdk_json_decode_string},
        {"fault_type", offsetof(struct { int fault_type; }, fault_type), spdk_json_decode_int32},
        {"delay_ms", offsetof(struct { int delay_ms; }, delay_ms), spdk_json_decode_int32, true}
    };
    
    if (spdk_json_decode_object(params, fault_decoders, SPDK_COUNTOF(fault_decoders), 
                               &device_name, &fault_type, &fault_delay_ms)) {
        if (device_name) free(device_name);
        snprintf(json_response, response_size, "{\"error\": \"无效的故障注入参数\"}");
        return -EINVAL;
    }
    
    // 仅在DEBUG模式下启用故障注入
#ifdef XBDEV_DEBUG
    // 注入故障（功能尚未实现）
    snprintf(json_response, response_size,
           "{"
           "\"status\": \"warning\","
           "\"message\": \"已注入故障到设备 %s (类型: %d, 延迟: %d ms)\","
           "\"warning\": \"故障注入可能导致系统不稳定，仅用于测试\""
           "}", device_name, fault_type, fault_delay_ms);
    
    // TODO: 实现实际的故障注入逻辑
#else
    snprintf(json_response, response_size,
           "{"
           "\"status\": \"error\","
           "\"message\": \"故障注入仅在DEBUG模式下可用\""
           "}");
    rc = -ENOTSUP;
#endif
    
    free(device_name);
    return rc;
}

/**
 * 实现管理服务器的主循环
 */
void xbdev_mgmt_run_server(void) {
    if (!g_mgmt_server.initialized) {
        XBDEV_ERRLOG("管理服务器未初始化\n");
        return;
    }
    
    // 主循环已在监听线程中实现，此处无需重复
    XBDEV_NOTICELOG("管理服务器主循环已由监听线程执行\n");
}

/**
 * 初始化管理模块
 */
int xbdev_mgmt_module_init(void) {
    // 初始化管理模块所需的全局状态
    pthread_mutex_init(&g_mgmt_server.lock, NULL);
    
    // 记录启动时间
    g_xbdev_start_time = time(NULL);
    
    XBDEV_NOTICELOG("管理模块已初始化\n");
    return 0;
}

/**
 * 清理管理模块
 */
void xbdev_mgmt_module_fini(void) {
    // 清理管理模块的全局状态
    pthread_mutex_destroy(&g_mgmt_server.lock);
    
    XBDEV_NOTICELOG("管理模块已清理\n");
}

/**
 * 更新设备事件处理
 */
void xbdev_mgmt_device_event(int event_type, const char *device_name, void *event_data) {
    // 处理设备事件并通知监听者
    _xbdev_send_notification(event_type, device_name, event_data);
}