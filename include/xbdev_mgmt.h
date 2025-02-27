/**
 * @file xbdev_mgmt.h
 * @brief 管理服务器API定义
 *
 * 本文件定义了libxbdev的管理服务器API，用于远程管理和监控。
 */

#ifndef XBDEV_MGMT_H
#define XBDEV_MGMT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 设备事件类型
 */
#define XBDEV_EVENT_DEVICE_ADDED     1  // 设备添加
#define XBDEV_EVENT_DEVICE_REMOVED   2  // 设备移除
#define XBDEV_EVENT_DEVICE_FAILED    3  // 设备故障
#define XBDEV_EVENT_RAID_DEGRADED    4  // RAID降级
#define XBDEV_EVENT_RAID_FAILED      5  // RAID失败
#define XBDEV_EVENT_RAID_RESYNCING   6  // RAID正在重同步
#define XBDEV_EVENT_RAID_RECOVERED   7  // RAID已恢复
#define XBDEV_EVENT_CAPACITY_WARNING 8  // 容量警告
#define XBDEV_EVENT_CAPACITY_CRITICAL 9 // 容量严重不足

/**
 * 事件通知回调类型
 * 
 * @param event_type 事件类型
 * @param device_name 设备名称
 * @param event_data 事件数据（根据事件类型不同而不同）
 * @param ctx 用户上下文
 */
typedef void (*xbdev_notification_cb)(int event_type, const char *device_name, void *event_data, void *ctx);

/**
 * 管理命令处理函数类型
 *
 * @param params JSON参数对象
 * @param json_response 输出JSON响应的缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
typedef int (*xbdev_mgmt_cmd_handler)(const struct spdk_json_val *params, char *json_response, size_t response_size);

/**
 * 初始化管理服务器
 *
 * @param listen_addr 监听地址（如"127.0.0.1"）
 * @param port 监听端口
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_server_init(const char *listen_addr, int port);

/**
 * 清理管理服务器
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_server_fini(void);

/**
 * 注册通知回调
 *
 * @param cb 回调函数
 * @param cb_arg 回调函数的用户参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_register_notification_callback(xbdev_notification_cb cb, void *cb_arg);

/**
 * 执行管理命令
 *
 * @param json_cmd JSON格式的命令字符串
 * @param json_response 输出JSON响应的缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_execute_cmd(const char *json_cmd, char *json_response, size_t response_size);

/**
 * 添加自定义管理命令
 *
 * @param command 命令名称
 * @param handler 命令处理函数
 * @param help_text 帮助文本
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_add_command(const char *command, xbdev_mgmt_cmd_handler handler, const char *help_text);

/**
 * 运行管理服务器主循环（阻塞）
 */
void xbdev_mgmt_run_server(void);

/**
 * 初始化管理模块
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_module_init(void);

/**
 * 清理管理模块
 */
void xbdev_mgmt_module_fini(void);

/**
 * 发送设备事件
 *
 * @param event_type 事件类型
 * @param device_name 设备名称
 * @param event_data 事件数据
 */
void xbdev_mgmt_device_event(int event_type, const char *device_name, void *event_data);

#ifdef __cplusplus
}
#endif

#endif // XBDEV_MGMT_H
