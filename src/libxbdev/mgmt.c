/**
 * @file mgmt.c
 * @brief 管理接口实现
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/jsonrpc.h>
#include <spdk/rpc.h>
#include <spdk/string.h>

struct mgmt_server {
    bool running;                    // 服务器是否运行
    char *listen_addr;              // 监听地址
    int port;                       // 监听端口
    xbdev_notification_cb notify_cb;// 通知回调
    void *notify_ctx;               // 回调上下文
};

static struct mgmt_server g_mgmt_server = {0};

#include "xbdev_security.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/util.h>
#include <spdk/bdev.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <json-c/json.h>
#include <signal.h>

// HTTP服务器配置
#define XBDEV_MGMT_DEFAULT_PORT 8181
#define XBDEV_MGMT_DEFAULT_LISTEN_ADDR "127.0.0.1"
#define XBDEV_MAX_MSG_SIZE (64 * 1024)  // 最大消息大小 64KB

// 管理命令表项
struct mgmt_cmd_entry {
    char name[64];                  // 命令名称
    xbdev_mgmt_cmd_handler handler;  // 处理函数
    char help[256];                 // 帮助文本
    struct mgmt_cmd_entry *next;     // 链表下一项
};

// 通知回调项
struct notification_cb_entry {
    xbdev_notification_cb cb;        // 回调函数
    void *ctx;                       // 回调上下文
    struct notification_cb_entry *next; // 链表下一项
};

// 全局状态
static struct {
    bool initialized;                // 是否已初始化
    bool running;                    // 是否运行中
    char listen_addr[64];            // 监听地址
    int port;                        // 监听端口
    struct spdk_jsonrpc_server *server; // JSON-RPC服务器
    pthread_t server_thread;         // 服务器线程
    bool server_thread_running;      // 服务器线程运行标志
    struct mgmt_cmd_entry *cmd_list;  // 命令列表
    struct notification_cb_entry *notify_list; // 通知回调列表
    pthread_mutex_t cmd_mutex;       // 命令列表锁
    pthread_mutex_t notify_mutex;    // 通知列表锁
} g_mgmt = {0};

/**
 * 处理版本查询命令
 */
static int _handle_version(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    snprintf(json_response, response_size,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"version\":\"%d.%d.%d\",\"api_version\":\"%d\"}}",
             XBDEV_VERSION_MAJOR, XBDEV_VERSION_MINOR, XBDEV_VERSION_PATCH,
             XBDEV_API_VERSION);
    return 0;
}

/**
 * 处理帮助命令
 */
static int _handle_help(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    struct json_object *result = json_object_new_object();
    struct json_object *commands = json_object_new_array();
    
    pthread_mutex_lock(&g_mgmt.cmd_mutex);
    
    struct mgmt_cmd_entry *entry = g_mgmt.cmd_list;
    while (entry) {
        struct json_object *cmd = json_object_new_object();
        json_object_object_add(cmd, "name", json_object_new_string(entry->name));
        json_object_object_add(cmd, "help", json_object_new_string(entry->help));
        json_object_array_add(commands, cmd);
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_mgmt.cmd_mutex);
    
    json_object_object_add(result, "commands", commands);
    
    // 构造响应
    snprintf(json_response, response_size,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":%s}",
             json_object_to_json_string(result));
    
    // 释放JSON对象
    json_object_put(result);
    
    return 0;
}

/**
 * 处理设备列表命令
 */
static int _handle_device_list(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 调用内部API获取设备列表
    return _xbdev_get_device_list(json_response, response_size);
}

/**
 * 处理设备信息命令
 */
static int _handle_device_info(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *device_name = NULL;
    
    // 解析设备名称
    if (params) {
        struct spdk_json_object_decoder decoders[] = {
            {"device", offsetof(struct { char *device; }, device), spdk_json_decode_string}
        };
        
        if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &device_name) == 0) {
            // 调用内部API获取设备信息
            int rc = _xbdev_get_device_info(device_name, json_response, response_size);
            free(device_name);
            return rc;
        }
        
        // 如果解码失败，尝试从JSON参数中直接获取设备名称
        struct json_object *jobj;
        struct json_object *device_obj;
        const char *dev_name = NULL;
        
        jobj = json_tokener_parse(params->start);
        if (jobj && json_object_object_get_ex(jobj, "device", &device_obj)) {
            dev_name = json_object_get_string(device_obj);
            if (dev_name) {
                int rc = _xbdev_get_device_info(dev_name, json_response, response_size);
                json_object_put(jobj);
                return rc;
            }
        }
        
        if (jobj) {
            json_object_put(jobj);
        }
    }
    
    // 如果没有提供设备名称，返回错误
    snprintf(json_response, response_size, 
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"无效参数：缺少设备名称\"}}");
    return -EINVAL;
}

/**
 * 处理创建RAID命令
 */
static int _handle_create_raid(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 调用内部命令处理函数
    return _xbdev_cmd_create_raid(params, json_response, response_size);
}

/**
 * 处理创建LVOL命令
 */
static int _handle_create_lvol(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 调用内部命令处理函数
    return _xbdev_cmd_create_lvol(params, json_response, response_size);
}

/**
 * 处理创建快照命令
 */
static int _handle_create_snapshot(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 调用内部命令处理函数
    return _xbdev_cmd_create_snapshot(params, json_response, response_size);
}

/**
 * 处理调整LVOL大小命令
 */
static int _handle_resize_lvol(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 调用内部命令处理函数
    return _xbdev_cmd_resize_lvol(params, json_response, response_size);
}

/**
 * 处理删除设备命令
 */
static int _handle_delete_device(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 调用内部命令处理函数
    return _xbdev_cmd_delete_device(params, json_response, response_size);
}

/**
 * 处理性能统计命令
 */
static int _handle_stats_get(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *device_name = NULL;
    bool get_avg = false;
    
    // 解析设备名称和选项
    if (params) {
        struct spdk_json_object_decoder decoders[] = {
            {"device", offsetof(struct { char *device; }, device), spdk_json_decode_string},
            {"average", offsetof(struct { bool average; }, average), spdk_json_decode_bool, true}
        };
        
        if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &device_name, &get_avg) != 0) {
            snprintf(json_response, response_size, 
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"无效参数\"}}");
            return -EINVAL;
        }
    } else {
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"缺少参数\"}}");
        return -EINVAL;
    }
    
    // 获取统计信息
    xbdev_stats_t stats;
    int rc = xbdev_stats_get(device_name, &stats, get_avg);
    
    if (rc != 0) {
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":%d,\"message\":\"获取统计信息失败\"}}",
                 rc);
        free(device_name);
        return rc;
    }
    
    // 构造JSON响应
    struct json_object *result = json_object_new_object();
    struct json_object *stats_obj = json_object_new_object();
    
    // 添加统计信息字段
    json_object_object_add(stats_obj, "total_iops", json_object_new_double(stats.total_iops));
    json_object_object_add(stats_obj, "read_iops", json_object_new_double(stats.read_iops));
    json_object_object_add(stats_obj, "write_iops", json_object_new_double(stats.write_iops));
    json_object_object_add(stats_obj, "unmap_iops", json_object_new_double(stats.unmap_iops));
    
    json_object_object_add(stats_obj, "total_bandwidth_mb", json_object_new_double(stats.total_bandwidth_mb));
    json_object_object_add(stats_obj, "read_bandwidth_mb", json_object_new_double(stats.read_bandwidth_mb));
    json_object_object_add(stats_obj, "write_bandwidth_mb", json_object_new_double(stats.write_bandwidth_mb));
    
    json_object_object_add(stats_obj, "avg_latency_us", json_object_new_double(stats.avg_latency_us));
    json_object_object_add(stats_obj, "avg_queue_depth", json_object_new_double(stats.avg_queue_depth));
    
    json_object_object_add(stats_obj, "read_ops", json_object_new_int64(stats.read_ops));
    json_object_object_add(stats_obj, "write_ops", json_object_new_int64(stats.write_ops));
    json_object_object_add(stats_obj, "unmap_ops", json_object_new_int64(stats.unmap_ops));
    
    json_object_object_add(stats_obj, "read_bytes", json_object_new_int64(stats.read_bytes));
    json_object_object_add(stats_obj, "write_bytes", json_object_new_int64(stats.write_bytes));
    
    json_object_object_add(stats_obj, "iops_utilization", json_object_new_double(stats.iops_utilization));
    json_object_object_add(stats_obj, "bandwidth_utilization", json_object_new_double(stats.bandwidth_utilization));
    
    // 添加设备名称信息
    json_object_object_add(result, "device", json_object_new_string(device_name));
    json_object_object_add(result, "type", json_object_new_string(get_avg ? "average" : "current"));
    json_object_object_add(result, "stats", stats_obj);
    
    // 构造响应
    snprintf(json_response, response_size,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":%s}",
             json_object_to_json_string(result));
    
    // 释放资源
    json_object_put(result);
    free(device_name);
    
    return 0;
}

/**
 * 处理修改安全配置命令
 */
static int _handle_security_config(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    // 解析安全配置参数
    char *mode_str = NULL;
    char *credentials = NULL;
    char *allowed_ips = NULL;
    char *allowed_users = NULL;
    bool audit_logging = false;
    char *audit_log_path = NULL;
    
    if (params) {
        struct spdk_json_object_decoder decoders[] = {
            {"mode", offsetof(struct { char *mode; }, mode), spdk_json_decode_string},
            {"credentials", offsetof(struct { char *creds; }, creds), spdk_json_decode_string, true},
            {"allowed_ips", offsetof(struct { char *ips; }, ips), spdk_json_decode_string, true},
            {"allowed_users", offsetof(struct { char *users; }, users), spdk_json_decode_string, true},
            {"audit_logging", offsetof(struct { bool logging; }, logging), spdk_json_decode_bool, true},
            {"audit_log_path", offsetof(struct { char *path; }, path), spdk_json_decode_string, true}
        };
        
        if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &mode_str, 
                                  &credentials, &allowed_ips, &allowed_users, 
                                  &audit_logging, &audit_log_path) != 0) {
            snprintf(json_response, response_size, 
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"无效安全参数\"}}");
            return -EINVAL;
        }
    } else {
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"缺少安全参数\"}}");
        return -EINVAL;
    }
    
    // 转换模式字符串到枚举
    xbdev_access_mode_t mode = XBDEV_ACCESS_MODE_OPEN;
    if (strcmp(mode_str, "open") == 0) {
        mode = XBDEV_ACCESS_MODE_OPEN;
    } else if (strcmp(mode_str, "basic") == 0) {
        mode = XBDEV_ACCESS_MODE_BASIC;
    } else if (strcmp(mode_str, "token") == 0) {
        mode = XBDEV_ACCESS_MODE_TOKEN;
    } else if (strcmp(mode_str, "tls_cert") == 0) {
        mode = XBDEV_ACCESS_MODE_TLS_CERT;
    } else {
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"无效的访问控制模式: %s\"}}",
                 mode_str);
        free(mode_str);
        if (credentials) free(credentials);
        if (allowed_ips) free(allowed_ips);
        if (allowed_users) free(allowed_users);
        if (audit_log_path) free(audit_log_path);
        return -EINVAL;
    }
    
    // 设置访问控制配置
    xbdev_access_control_t config;
    memset(&config, 0, sizeof(config));
    
    config.mode = mode;
    
    if (credentials) {
        strncpy(config.credentials, credentials, sizeof(config.credentials) - 1);
    }
    
    if (allowed_ips) {
        strncpy(config.allowed_ips, allowed_ips, sizeof(config.allowed_ips) - 1);
    }
    
    if (allowed_users) {
        strncpy(config.allowed_users, allowed_users, sizeof(config.allowed_users) - 1);
    }
    
    config.audit_logging = audit_logging;
    
    if (audit_log_path) {
        strncpy(config.audit_log_path, audit_log_path, sizeof(config.audit_log_path) - 1);
    }
    
    // 应用配置
    int rc = xbdev_mgmt_set_access_control(&config);
    
    if (rc != 0) {
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":%d,\"message\":\"设置安全配置失败\"}}",
                 rc);
    } else {
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"success\",\"message\":\"安全配置已更新\"}}");
    }
    
    // 释放资源
    free(mode_str);
    if (credentials) free(credentials);
    if (allowed_ips) free(allowed_ips);
    if (allowed_users) free(allowed_users);
    if (audit_log_path) free(audit_log_path);
    
    return rc;
}

/**
 * 处理认证请求
 */
static int _handle_authenticate(const struct spdk_json_val *params, char *json_response, size_t response_size) {
    char *username = NULL;
    char *password = NULL;
    uint32_t token_expiry = 3600; // 默认1小时过期
    bool generate_token = false;
    
    // 解析认证参数
    if (params) {
        struct spdk_json_object_decoder decoders[] = {
            {"username", offsetof(struct { char *user; }, user), spdk_json_decode_string},
            {"password", offsetof(struct { char *pass; }, pass), spdk_json_decode_string},
            {"generate_token", offsetof(struct { bool gen_token; }, gen_token), spdk_json_decode_bool, true},
            {"token_expiry", offsetof(struct { uint32_t expiry; }, expiry), spdk_json_decode_uint32, true}
        };
        
        if (spdk_json_decode_object(params, decoders, SPDK_COUNTOF(decoders), &username, 
                                  &password, &generate_token, &token_expiry) != 0) {
            snprintf(json_response, response_size, 
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"无效认证参数\"}}");
            return -EINVAL;
        }
    } else {
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"缺少认证参数\"}}");
        return -EINVAL;
    }
    
    // 进行认证
    int rc = xbdev_mgmt_authenticate(username, password);
    
    if (rc != 0) {
        // 认证失败
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":%d,\"message\":\"认证失败\"}}",
                 rc);
        free(username);
        free(password);
        return rc;
    }
    
    // 认证成功
    if (generate_token) {
        // 如果需要生成令牌
        char token[256] = {0};
        
        rc = xbdev_mgmt_generate_token(username, password, token, sizeof(token), token_expiry);
        
        if (rc != 0) {
            snprintf(json_response, response_size, 
                     "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":%d,\"message\":\"生成令牌失败\"}}",
                     rc);
            free(username);
            free(password);
            return rc;
        }
        
        // 返回令牌信息
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"authenticated\":true,\"token\":\"%s\",\"expires_in\":%d}}",
                 token, token_expiry);
    } else {
        // 不需要生成令牌，仅返回认证成功
        snprintf(json_response, response_size, 
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"authenticated\":true}}");
    }
    
    // 记录审计日志
    xbdev_security_audit_log(username, "authenticate", "", rc);
    
    // 释放资源
    free(username);
    free(password);
    
    return 0;
}

/**
 * JSON-RPC处理函数
 */
static void handle_rpc_request(struct spdk_jsonrpc_request *request, const struct spdk_json_val *method, const struct spdk_json_val *params) {
    char *method_name = NULL;
    char json_response[XBDEV_MAX_MSG_SIZE] = {0};
    bool method_found = false;
    int rc;
    
    // 获取方法名
    if (spdk_json_decode_string(method, &method_name) != 0) {
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_REQUEST, "无效的方法名");
        return;
    }
    
    // 输出调试信息
    XBDEV_NOTICELOG("处理JSON-RPC请求: %s\n", method_name);
    
    // 处理系统内置方法
    if (strcmp(method_name, "version") == 0) {
        rc = _handle_version(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "help") == 0) {
        rc = _handle_help(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "device_list") == 0) {
        rc = _handle_device_list(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "device_info") == 0) {
        rc = _handle_device_info(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "create_raid") == 0) {
        rc = _handle_create_raid(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "create_lvol") == 0) {
        rc = _handle_create_lvol(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "create_snapshot") == 0) {
        rc = _handle_create_snapshot(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "resize_lvol") == 0) {
        rc = _handle_resize_lvol(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "delete_device") == 0) {
        rc = _handle_delete_device(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "stats_get") == 0) {
        rc = _handle_stats_get(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "security_config") == 0) {
        rc = _handle_security_config(params, json_response, sizeof(json_response));
        method_found = true;
    } else if (strcmp(method_name, "authenticate") == 0) {
        rc = _handle_authenticate(params, json_response, sizeof(json_response));
        method_found = true;
    }
    
    // 如果不是内置方法，查找用户注册的命令
    if (!method_found) {
        pthread_mutex_lock(&g_mgmt.cmd_mutex);
        
        struct mgmt_cmd_entry *entry = g_mgmt.cmd_list;
        while (entry) {
            if (strcmp(entry->name, method_name) == 0) {
                // 调用用户注册的处理函数
                pthread_mutex_unlock(&g_mgmt.cmd_mutex);
                rc = entry->handler(params, json_response, sizeof(json_response));
                method_found = true;
                break;
            }
            entry = entry->next;
        }
        
        if (!method_found) {
            pthread_mutex_unlock(&g_mgmt.cmd_mutex);
        }
    }
    
    // 如果找不到方法，返回错误
    if (!method_found) {
        free(method_name);
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_METHOD_NOT_FOUND, "未知方法");
        return;
    }
    
    // 发送响应
    if (rc == 0 && json_response[0] != '\0') {
        // 如果响应已经是JSON格式，直接发送
        spdk_jsonrpc_send_raw_response(request, json_response);
    } else {
        // 构造错误响应
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR, "内部错误");
    }
    
    free(method_name);
}

/**
 * 服务器线程函数
 */
static void *mgmt_server_thread(void *arg) {
    while (g_mgmt.running) {
        // 轮询JSON-RPC服务器，处理请求
        if (g_mgmt.server) {
            spdk_jsonrpc_server_poll(g_mgmt.server);
        }
        
        // 短暂休眠，避免CPU占用过高
        usleep(10000); // 10ms
    }
    
    return NULL;
}

/**
 * 初始化管理服务器
 */
int xbdev_mgmt_server_init(const char *listen_addr, int port) {
    int rc;
    
    // 检查是否已初始化
    if (g_mgmt.initialized) {
        XBDEV_WARNLOG("管理服务器已经初始化\n");
        return 0;
    }
    
    // 初始化互斥锁
    pthread_mutex_init(&g_mgmt.cmd_mutex, NULL);
    pthread_mutex_init(&g_mgmt.notify_mutex, NULL);
    
    // 设置监听地址和端口
    if (listen_addr) {
        strncpy(g_mgmt.listen_addr, listen_addr, sizeof(g_mgmt.listen_addr) - 1);
    } else {
        strncpy(g_mgmt.listen_addr, XBDEV_MGMT_DEFAULT_LISTEN_ADDR, sizeof(g_mgmt.listen_addr) - 1);
    }
    
    g_mgmt.port = port > 0 ? port : XBDEV_MGMT_DEFAULT_PORT;
    
    // 创建JSON-RPC服务器
    g_mgmt.server = spdk_jsonrpc_server_listen(g_mgmt.listen_addr, g_mgmt.port, NULL);
    if (!g_mgmt.server) {
        XBDEV_ERRLOG("无法创建JSON-RPC服务器: %s:%d\n", g_mgmt.listen_addr, g_mgmt.port);
        pthread_mutex_destroy(&g_mgmt.cmd_mutex);
        pthread_mutex_destroy(&g_mgmt.notify_mutex);
        return -EINVAL;
    }
    
    // 注册请求处理回调
    spdk_jsonrpc_set_handler(g_mgmt.server, handle_rpc_request);
    
    // 标记为正在运行
    g_mgmt.running = true;
    g_mgmt.initialized = true;
    
    // 启动服务器线程
    rc = pthread_create(&g_mgmt.server_thread, NULL, mgmt_server_thread, NULL);
    if (rc != 0) {
        XBDEV_ERRLOG("无法创建服务器线程: %d\n", rc);
        spdk_jsonrpc_server_shutdown(g_mgmt.server);
        g_mgmt.server = NULL;
        g_mgmt.running = false;
        g_mgmt.initialized = false;
        pthread_mutex_destroy(&g_mgmt.cmd_mutex);
        pthread_mutex_destroy(&g_mgmt.notify_mutex);
        return -rc;
    }
    
    g_mgmt.server_thread_running = true;
    
    XBDEV_NOTICELOG("管理服务器已启动: %s:%d\n", g_mgmt.listen_addr, g_mgmt.port);
    
    return 0;
}

/**
 * 清理管理服务器
 */
int xbdev_mgmt_server_fini(void) {
    // 检查是否已初始化
    if (!g_mgmt.initialized) {
        return 0;
    }
    
    // 停止服务器
    g_mgmt.running = false;
    
    // 等待服务器线程退出
    if (g_mgmt.server_thread_running) {
        pthread_join(g_mgmt.server_thread, NULL);
        g_mgmt.server_thread_running = false;
    }
    
    // 关闭JSON-RPC服务器
    if (g_mgmt.server) {
        spdk_jsonrpc_server_shutdown(g_mgmt.server);
        g_mgmt.server = NULL;
    }
    
    // 释放命令列表
    pthread_mutex_lock(&g_mgmt.cmd_mutex);
    struct mgmt_cmd_entry *cmd_entry = g_mgmt.cmd_list;
    while (cmd_entry) {
        struct mgmt_cmd_entry *next = cmd_entry->next;
        free(cmd_entry);
        cmd_entry = next;
    }
    g_mgmt.cmd_list = NULL;
    pthread_mutex_unlock(&g_mgmt.cmd_mutex);
    
    // 释放通知回调列表
    pthread_mutex_lock(&g_mgmt.notify_mutex);
    struct notification_cb_entry *notify_entry = g_mgmt.notify_list;
    while (notify_entry) {
        struct notification_cb_entry *next = notify_entry->next;
        free(notify_entry);
        notify_entry = next;
    }
    g_mgmt.notify_list = NULL;
    pthread_mutex_unlock(&g_mgmt.notify_mutex);
    
    // 销毁互斥锁
    pthread_mutex_destroy(&g_mgmt.cmd_mutex);
    pthread_mutex_destroy(&g_mgmt.notify_mutex);
    
    // 重置全局状态
    memset(&g_mgmt, 0, sizeof(g_mgmt));
    
    XBDEV_NOTICELOG("管理服务器已关闭\n");
    
    return 0;
}

/**
 * 注册通知回调
 */
int xbdev_mgmt_register_notification_callback(xbdev_notification_cb cb, void *cb_arg) {
    if (!cb) {
        return -EINVAL;
    }
    
    // 检查是否已初始化
    if (!g_mgmt.initialized) {
        XBDEV_ERRLOG("管理服务器未初始化\n");
        return -EINVAL;
    }
    
    // 分配通知回调项
    struct notification_cb_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        XBDEV_ERRLOG("无法分配通知回调项\n");
        return -ENOMEM;
    }
    
    // 设置回调信息
    entry->cb = cb;
    entry->ctx = cb_arg;
    
    // 添加到链表
    pthread_mutex_lock(&g_mgmt.notify_mutex);
    entry->next = g_mgmt.notify_list;
    g_mgmt.notify_list = entry;
    pthread_mutex_unlock(&g_mgmt.notify_mutex);
    
    XBDEV_NOTICELOG("已注册通知回调\n");
    
    return 0;
}

/**
 * 添加自定义管理命令
 */
int xbdev_mgmt_add_command(const char *command, xbdev_mgmt_cmd_handler handler, const char *help_text) {
    if (!command || !handler) {
        return -EINVAL;
    }
    
    // 检查是否已初始化
    if (!g_mgmt.initialized) {
        XBDEV_ERRLOG("管理服务器未初始化\n");
        return -EINVAL;
    }
    
    // 检查命令是否已存在
    pthread_mutex_lock(&g_mgmt.cmd_mutex);
    struct mgmt_cmd_entry *entry = g_mgmt.cmd_list;
    while (entry) {
        if (strcmp(entry->name, command) == 0) {
            pthread_mutex_unlock(&g_mgmt.cmd_mutex);
            XBDEV_ERRLOG("命令已存在: %s\n", command);
            return -EEXIST;
        }
        entry = entry->next;
    }
    
    // 分配命令项
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        pthread_mutex_unlock(&g_mgmt.cmd_mutex);
        XBDEV_ERRLOG("无法分配命令项\n");
        return -ENOMEM;
    }
    
    // 设置命令信息
    strncpy(entry->name, command, sizeof(entry->name) - 1);
    entry->handler = handler;
    if (help_text) {
        strncpy(entry->help, help_text, sizeof(entry->help) - 1);
    } else {
        strncpy(entry->help, "No help available", sizeof(entry->help) - 1);
    }
    
    // 添加到链表
    entry->next = g_mgmt.cmd_list;
    g_mgmt.cmd_list = entry;
    
    pthread_mutex_unlock(&g_mgmt.cmd_mutex);
    
    XBDEV_NOTICELOG("已添加命令: %s\n", command);
    
    return 0;
}

/**
 * 执行管理命令
 */
int xbdev_mgmt_execute_cmd(const char *json_cmd, char *json_response, size_t response_size) {
    struct spdk_json_val *values = NULL;
    size_t values_cnt = 0;
    struct spdk_json_val method;
    struct spdk_json_val params;
    char *method_name = NULL;
    int rc = 0;
    
    if (!json_cmd || !json_response || response_size == 0) {
        return -EINVAL;
    }
    
    // 清空响应缓冲区
    memset(json_response, 0, response_size);
    
    // 解析JSON命令
    rc = spdk_json_parse(json_cmd, strlen(json_cmd), NULL, 0, &values_cnt, 0);
    if (rc < 0) {
        snprintf(json_response, response_size, 
                 "{\"error\":{\"code\":-32700,\"message\":\"解析错误：无效的JSON\"}}");
        return -EINVAL;
    }
    
    values = calloc(values_cnt, sizeof(*values));
    if (values == NULL) {
        snprintf(json_response, response_size, 
                 "{\"error\":{\"code\":-32603,\"message\":\"内部错误：内存分配失败\"}}");
        return -ENOMEM;
    }
    
    rc = spdk_json_parse(json_cmd, strlen(json_cmd), values, values_cnt, NULL, 0);
    if (rc < 0) {
        snprintf(json_response, response_size, 
                 "{\"error\":{\"code\":-32700,\"message\":\"解析错误：无效的JSON\"}}");
        free(values);
        return -EINVAL;
    }
    
    // 验证是否为有效的JSON-RPC请求格式
    uint32_t version_index, method_index, params_index;
    bool version_found = false, method_found = false, params_found = false;
    
    for (size_t i = 0; i < values_cnt; i++) {
        if (values[i].type == SPDK_JSON_VAL_NAME) {
            if (spdk_json_strequal((const char *)values[i].start, values[i].len, "jsonrpc")) {
                version_index = i + 1;
                version_found = true;
            } else if (spdk_json_strequal((const char *)values[i].start, values[i].len, "method")) {
                method_index = i + 1;
                method_found = true;
                method = values[method_index];
            } else if (spdk_json_strequal((const char *)values[i].start, values[i].len, "params")) {
                params_index = i + 1;
                params_found = true;
                params = values[params_index];
            }
        }
    }
    
    // 确保找到了方法名
    if (!method_found) {
        snprintf(json_response, response_size, 
                 "{\"error\":{\"code\":-32600,\"message\":\"无效的请求：缺少方法名\"}}");
        free(values);
        return -EINVAL;
    }
    
    // 解析方法名
    rc = spdk_json_decode_string(&method, &method_name);
    if (rc != 0) {
        snprintf(json_response, response_size, 
                 "{\"error\":{\"code\":-32600,\"message\":\"无效的请求：方法名必须是字符串\"}}");
        free(values);
        return -EINVAL;
    }
    
    // 执行命令
    XBDEV_NOTICELOG("执行管理命令: %s\n", method_name);
    
    rc = 0;
    bool method_executed = false;
    
    // 首先检查内置方法
    if (strcmp(method_name, "version") == 0) {
        rc = _handle_version(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "help") == 0) {
        rc = _handle_help(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "device_list") == 0) {
        rc = _handle_device_list(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "device_info") == 0) {
        rc = _handle_device_info(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "create_raid") == 0) {
        rc = _handle_create_raid(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "create_lvol") == 0) {
        rc = _handle_create_lvol(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "create_snapshot") == 0) {
        rc = _handle_create_snapshot(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "resize_lvol") == 0) {
        rc = _handle_resize_lvol(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "delete_device") == 0) {
        rc = _handle_delete_device(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "stats_get") == 0) {
        rc = _handle_stats_get(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "security_config") == 0) {
        rc = _handle_security_config(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    } else if (strcmp(method_name, "authenticate") == 0) {
        rc = _handle_authenticate(params_found ? &params : NULL, json_response, response_size);
        method_executed = true;
    }
    
    // 如果不是内置方法，查找用户注册的命令
    if (!method_executed) {
        pthread_mutex_lock(&g_mgmt.cmd_mutex);
        
        struct mgmt_cmd_entry *entry = g_mgmt.cmd_list;
        while (entry) {
            if (strcmp(entry->name, method_name) == 0) {
                // 调用用户注册的处理函数
                pthread_mutex_unlock(&g_mgmt.cmd_mutex);
                rc = entry->handler(params_found ? &params : NULL, json_response, response_size);
                method_executed = true;
                break;
            }
            entry = entry->next;
        }
        
        if (!method_executed) {
            pthread_mutex_unlock(&g_mgmt.cmd_mutex);
            snprintf(json_response, response_size,
                    "{\"error\":{\"code\":-32601,\"message\":\"方法不存在：%s\"}}",
                    method_name);
            rc = -ENOENT;
        }
    }
    
    // 如果命令执行成功但没有设置响应，设置默认响应
    if (rc == 0 && method_executed && json_response[0] == '\0') {
        snprintf(json_response, response_size,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"success\"}");
    }
    
    // 释放资源
    free(method_name);
    free(values);
    
    return rc;
}

/**
 * 发送设备事件通知
 *
 * @param event_type 事件类型
 * @param device_name 设备名称
 * @param event_data 事件数据
 */
void xbdev_mgmt_device_event(int event_type, const char *device_name, void *event_data) {
    // 检查是否初始化
    if (!g_mgmt.initialized) {
        return;
    }
    
    // 记录事件
    const char *event_type_str = "unknown";
    switch (event_type) {
        case XBDEV_EVENT_DEVICE_ADDED:
            event_type_str = "device_added";
            break;
        case XBDEV_EVENT_DEVICE_REMOVED:
            event_type_str = "device_removed";
            break;
        case XBDEV_EVENT_DEVICE_FAILED:
            event_type_str = "device_failed";
            break;
        case XBDEV_EVENT_RAID_DEGRADED:
            event_type_str = "raid_degraded";
            break;
        case XBDEV_EVENT_RAID_FAILED:
            event_type_str = "raid_failed";
            break;
        case XBDEV_EVENT_RAID_RESYNCING:
            event_type_str = "raid_resyncing";
            break;
        case XBDEV_EVENT_RAID_RECOVERED:
            event_type_str = "raid_recovered";
            break;
        case XBDEV_EVENT_CAPACITY_WARNING:
            event_type_str = "capacity_warning";
            break;
        case XBDEV_EVENT_CAPACITY_CRITICAL:
            event_type_str = "capacity_critical";
            break;
    }
    
    XBDEV_NOTICELOG("设备事件: %s, 设备: %s\n", event_type_str, device_name ? device_name : "unknown");
    
    // 通知所有注册的回调
    pthread_mutex_lock(&g_mgmt.notify_mutex);
    
    struct notification_cb_entry *entry = g_mgmt.notify_list;
    while (entry) {
        if (entry->cb) {
            // 在通知互斥锁外调用回调，避免死锁
            pthread_mutex_unlock(&g_mgmt.notify_mutex);
            entry->cb(event_type, device_name, event_data, entry->ctx);
            pthread_mutex_lock(&g_mgmt.notify_mutex);
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_mgmt.notify_mutex);
}

/**
 * 运行管理服务器主循环（阻塞）
 */
void xbdev_mgmt_run_server(void) {
    if (!g_mgmt.initialized || !g_mgmt.server) {
        XBDEV_ERRLOG("管理服务器未初始化\n");
        return;
    }
    
    XBDEV_NOTICELOG("管理服务器主循环开始运行\n");
    
    // 设置信号处理
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    
    // 主循环
    while (g_mgmt.running) {
        // 轮询JSON-RPC服务器，处理请求
        spdk_jsonrpc_server_poll(g_mgmt.server);
        
        // 短暂休眠，避免CPU占用过高
        usleep(10000); // 10ms
    }
    
    XBDEV_NOTICELOG("管理服务器主循环已退出\n");
}

/**
 * 初始化管理模块
 */
int xbdev_mgmt_module_init(void) {
    // 默认在127.0.0.1:8181上启动管理服务器
    return xbdev_mgmt_server_init(NULL, 0);
}

/**
 * 清理管理模块
 */
void xbdev_mgmt_module_fini(void) {
    xbdev_mgmt_server_fini();
}

/**
 * 响应关闭信号，优雅退出
 */
static void _handle_shutdown_signal(int signo) {
    XBDEV_NOTICELOG("收到关闭信号 %d, 正在关闭管理服务器...\n", signo);
    g_mgmt.running = false;
}

/**
 * 设置服务器监听地址
 * 
 * @param address 新的监听地址
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_set_listen_address(const char *address) {
    if (!address || !g_mgmt.initialized) {
        return -EINVAL;
    }
    
    // 需要重启服务器才能更改监听地址
    XBDEV_NOTICELOG("更改监听地址需要重启管理服务器\n");
    
    return -ENOTSUP;
}

/**
 * 获取管理服务器状态
 * 
 * @param running 输出参数，是否运行中
 * @param address 输出参数，当前监听地址
 * @param port 输出参数，当前监听端口
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_get_status(bool *running, char *address, int *port) {
    if (!g_mgmt.initialized) {
        return -EINVAL;
    }
    
    if (running) {
        *running = g_mgmt.running;
    }
    
    if (address) {
        strncpy(address, g_mgmt.listen_addr, 64);
    }
    
    if (port) {
        *port = g_mgmt.port;
    }
    
    return 0;
}