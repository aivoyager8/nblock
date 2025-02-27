/**
 * @file xbdev_internal.h
 * @brief libxbdev内部数据结构和函数定义
 *
 * 本文件定义了libxbdev库内部使用的数据结构和函数，
 * 不对外部应用程序暴露这些定义。
 */

#ifndef _XBDEV_INTERNAL_H_
#define _XBDEV_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <spdk/bdev.h>
#include <spdk/thread.h>
#include <spdk/env.h>
#include "xbdev.h"

/**
 * IO请求类型
 */
typedef enum {
    XBDEV_REQ_READ,               // 读取请求
    XBDEV_REQ_WRITE,              // 写入请求
    XBDEV_REQ_FLUSH,              // 刷新请求
    XBDEV_REQ_UNMAP,              // 取消映射请求
    XBDEV_REQ_RESET,              // 设备重置请求
    XBDEV_REQ_COMPARE,            // 比较请求
    XBDEV_REQ_COMPARE_AND_WRITE,  // 比较并写入请求
    XBDEV_REQ_WRITE_ZEROES,       // 写入零请求
    XBDEV_REQ_GET_STATS,          // 获取统计信息请求
    XBDEV_REQ_RESET_STATS,        // 重置统计信息请求
    XBDEV_REQ_BATCH,              // 批量IO请求
    XBDEV_REQ_MAX
} xbdev_req_type_t;

/**
 * 请求状态
 */
typedef enum {
    XBDEV_REQ_STATUS_NEW,         // 新请求
    XBDEV_REQ_STATUS_SUBMITTED,   // 已提交
    XBDEV_REQ_STATUS_PROCESSING,  // 处理中
    XBDEV_REQ_STATUS_COMPLETED,   // 已完成
    XBDEV_REQ_STATUS_ERROR        // 错误
} xbdev_req_status_t;

/**
 * IO请求结构
 */
typedef struct xbdev_request {
    xbdev_req_type_t type;        // 请求类型
    xbdev_req_status_t status;    // 请求状态
    void *ctx;                    // 请求上下文
    void *cb_arg;                 // 回调参数
    xbdev_cb cb;                  // 完成回调函数
    int result;                   // 操作结果
    uint64_t submit_tsc;          // 提交时间戳(CPU周期)
    uint64_t complete_tsc;        // 完成时间戳(CPU周期)
    struct xbdev_request *next;   // 链表下一个节点
} xbdev_request_t;

/**
 * 文件描述符表项
 */
typedef struct {
    struct spdk_bdev *bdev;       // BDEV对象
    struct spdk_bdev_desc *desc;  // BDEV描述符
    int ref_count;                // 引用计数
    uint64_t timeout_ms;          // IO超时时间(毫秒)
    uint32_t retry_count;         // IO重试次数
    uint32_t cache_policy;        // 缓存策略
    bool claimed;                 // 是否已被占用
} xbdev_fd_entry_t;

/**
 * 同步IO完成上下文
 */
typedef struct xbdev_sync_io_completion {
    bool done;                    // 是否已完成
    enum spdk_bdev_io_status status;  // 完成状态
} xbdev_sync_io_completion_t;

/**
 * 库初始化标志
 */
extern bool g_xbdev_initialized;

/**
 * 文件描述符表
 */
extern xbdev_fd_entry_t *g_fd_table;

/**
 * 文件描述符表大小
 */
extern int g_fd_table_size;

/**
 * 同步请求池大小
 */
#define XBDEV_SYNC_REQ_POOL_SIZE 128

/**
 * 同步请求池
 */
extern xbdev_request_t *g_sync_req_pool;

/**
 * 分配同步请求
 *
 * @return 请求结构指针，失败返回NULL
 */
xbdev_request_t *xbdev_sync_request_alloc(void);

/**
 * 释放同步请求
 *
 * @param req 请求指针
 */
void xbdev_sync_request_free(xbdev_request_t *req);

/**
 * 执行同步请求
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_sync_request_execute(xbdev_request_t *req);

/**
 * 分配异步请求
 *
 * @return 请求结构指针，失败返回NULL
 */
xbdev_request_t *xbdev_request_alloc(void);

/**
 * 释放异步请求
 *
 * @param req 请求指针
 */
void xbdev_request_free(xbdev_request_t *req);

/**
 * 提交异步请求
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_submit(xbdev_request_t *req);

/**
 * 获取文件描述符表项
 *
 * @param fd 文件描述符
 * @return 文件描述符表项指针，失败返回NULL
 */
xbdev_fd_entry_t *_xbdev_get_fd_entry(int fd);

/**
 * 处理完成队列
 *
 * @param max_events 最大处理事件数
 * @return 处理的事件数
 */
int _xbdev_process_completions(int max_events);

/**
 * 同步IO完成回调
 *
 * @param bdev_io BDEV IO对象
 * @param success IO是否成功
 * @param cb_arg 回调参数
 */
void xbdev_sync_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/**
 * 等待同步操作完成
 *
 * @param done 完成标志指针
 * @param timeout_us 超时时间(微秒)
 * @return 成功返回0，超时返回-ETIMEDOUT
 */
int _xbdev_wait_for_completion(bool *done, uint64_t timeout_us);

/**
 * 读取操作重试函数
 */
void xbdev_sync_io_retry_read(void *arg);

/**
 * 写入操作重试函数
 */
void xbdev_sync_io_retry_write(void *arg);

/**
 * 刷新操作重试函数
 */
void xbdev_sync_io_retry_flush(void *arg);

/**
 * UNMAP操作重试函数
 */
void xbdev_sync_io_retry_unmap(void *arg);

/**
 * 复位设备重试函数
 */
void xbdev_sync_io_retry_reset(void *arg);

/**
 * 写入零操作重试函数
 */
void xbdev_sync_io_retry_write_zeroes(void *arg);

/**
 * 比较操作重试函数
 */
void xbdev_sync_io_retry_compare(void *arg);

/**
 * 比较并写入操作重试函数
 */
void xbdev_sync_io_retry_compare_and_write(void *arg);

/**
 * 异步IO回调函数
 */
void xbdev_async_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);

/**
 * 读取操作SPDK线程上下文处理函数
 */
void xbdev_read_on_thread(void *ctx);

/**
 * 写入操作SPDK线程上下文处理函数
 */
void xbdev_write_on_thread(void *ctx);

/**
 * 刷新操作SPDK线程上下文处理函数
 */
void xbdev_flush_on_thread(void *ctx);

/**
 * UNMAP操作SPDK线程上下文处理函数
 */
void xbdev_unmap_on_thread(void *ctx);

/**
 * 重置操作SPDK线程上下文处理函数
 */
void xbdev_reset_on_thread(void *ctx);

/**
 * 写入零操作SPDK线程上下文处理函数
 */
void xbdev_write_zeroes_on_thread(void *ctx);

/**
 * 比较操作SPDK线程上下文处理函数
 */
void xbdev_compare_on_thread(void *ctx);

/**
 * 比较并写入操作SPDK线程上下文处理函数
 */
void xbdev_compare_and_write_on_thread(void *ctx);

/**
 * 获取统计信息SPDK线程上下文处理函数
 */
void xbdev_get_io_stats_on_thread(void *ctx);

/**
 * 重置统计信息SPDK线程上下文处理函数
 */
void xbdev_reset_io_stats_on_thread(void *ctx);

/**
 * 日志宏定义
 */
#define XBDEV_NOTICELOG(fmt, args...) \
    SPDK_NOTICELOG("[XBDEV] " fmt, ##args)
#define XBDEV_WARNLOG(fmt, args...) \
    SPDK_WARNLOG("[XBDEV] " fmt, ##args)
#define XBDEV_ERRLOG(fmt, args...) \
    SPDK_ERRLOG("[XBDEV] " fmt, ##args)
#define XBDEV_INFOLOG(fmt, args...) \
    SPDK_INFOLOG(xbdev, fmt, ##args)
#define XBDEV_DEBUGLOG(fmt, args...) \
    SPDK_DEBUGLOG(xbdev, fmt, ##args)

#endif /* _XBDEV_INTERNAL_H_ */
