/**
 * @file xbdev_internal.h
 * @brief 内部头文件，定义了libxbdev的内部数据结构和函数
 *
 * 此头文件仅供内部使用，包含了实现所需的内部组件定义。
 * 不应由用户直接包含此文件。
 */

#ifndef _XBDEV_INTERNAL_H_
#define _XBDEV_INTERNAL_H_

#include <spdk/bdev.h>
#include <pthread.h>
#include "xbdev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 日志宏定义
 */
#define XBDEV_ERRLOG(...) fprintf(stderr, "[ERROR] " __VA_ARGS__)
#define XBDEV_WARNLOG(...) fprintf(stderr, "[WARN] " __VA_ARGS__)
#define XBDEV_NOTICELOG(...) fprintf(stderr, "[NOTICE] " __VA_ARGS__)
#define XBDEV_INFOLOG(...) fprintf(stderr, "[INFO] " __VA_ARGS__)
#define XBDEV_DEBUGLOG(...) fprintf(stderr, "[DEBUG] " __VA_ARGS__)

/**
 * 最大打开文件数
 */
#define XBDEV_MAX_OPEN_FILES 1024

/**
 * 最大同步IO等待时间（微秒）
 */
#define XBDEV_SYNC_IO_TIMEOUT_US 30000000  // 30秒

/**
 * 文件描述符表项结构
 */
typedef struct xbdev_fd_entry {
    TAILQ_ENTRY(xbdev_fd_entry) link;  // 链表节点
    int fd;                            // 文件描述符
    char bdev_name[256];              // bdev名称
    struct spdk_bdev *bdev;           // bdev指针
    struct spdk_bdev_desc *desc;      // bdev描述符
    pthread_mutex_t mutex;            // 互斥锁
    uint32_t io_timeout_ms;          // IO超时时间
    uint8_t io_priority;             // IO优先级
    int max_concurrent_ios;          // 最大并发IO数
    xbdev_io_retry_cb retry_cb;      // 重试回调
    void *retry_cb_arg;              // 重试回调参数
    xbdev_cache_policy_t cache_policy;  // 缓存策略  
    void *memory_domain;             // 内存域
    void *memory_domain_ctx;         // 内存域上下文
    xbdev_priority_policy_t priority_policy;  // 优先级策略
} xbdev_fd_entry_t;

/**
 * 文件描述符表
 */
typedef struct {
    xbdev_fd_entry_t entries[XBDEV_MAX_OPEN_FILES]; // 文件描述符表项
} xbdev_fd_table_t;

/**
 * JSON配置结构体
 */
typedef struct {
    struct spdk_json_val *base_devices;   // 基础设备配置
    struct spdk_json_val *raid_devices;   // RAID设备配置
    struct spdk_json_val *lvol_stores;    // LVOL存储池配置
    struct spdk_json_val *lvol_volumes;   // LVOL卷配置
    struct spdk_json_val *snapshots;      // 快照配置
    struct spdk_json_val *clones;         // 克隆配置
} xbdev_config_t;

/**
 * 注册BDEV类型
 */
typedef enum {
    XBDEV_BDEV_TYPE_NVME = 0,
    XBDEV_BDEV_TYPE_NVMF,
    XBDEV_BDEV_TYPE_RBD,
    XBDEV_BDEV_TYPE_AIO,
    XBDEV_BDEV_TYPE_MALLOC,
    XBDEV_BDEV_TYPE_NULL,
    XBDEV_BDEV_TYPE_RAID0,
    XBDEV_BDEV_TYPE_RAID1,
    XBDEV_BDEV_TYPE_RAID5,
    XBDEV_BDEV_TYPE_LVOL
} xbdev_bdev_type_t;

/**
 * 请求类型定义
 */
typedef enum {
    XBDEV_REQ_READ,           // 读操作
    XBDEV_REQ_WRITE,          // 写操作
    XBDEV_REQ_FLUSH,          // 刷新操作
    XBDEV_REQ_UNMAP,          // UNMAP操作
    XBDEV_REQ_RESET,          // 重置操作
    XBDEV_REQ_READV,          // 向量读操作
    XBDEV_REQ_WRITEV,         // 向量写操作
    XBDEV_REQ_CUSTOM          // 自定义操作
} xbdev_req_type_t;

/**
 * 请求结构体
 */
typedef struct xbdev_request {
    xbdev_req_type_t type;    // 请求类型
    void *ctx;                // 请求上下文
    int64_t id;               // 请求ID
    void *ext_opts;           // 扩展选项
} xbdev_request_t;

/**
 * 全局内部函数声明
 */
void _xbdev_close_desc(xbdev_fd_entry_t *entry);
xbdev_fd_entry_t *_xbdev_get_fd_entry(int fd);
int _xbdev_parse_json(const char *json_text, size_t json_len, xbdev_config_t *config);
int _xbdev_free_config(xbdev_config_t *config);
int _xbdev_apply_config(struct spdk_json_val *values);
int _xbdev_apply_base_devices(xbdev_config_t *config);
int _xbdev_apply_raid_devices(xbdev_config_t *config);
int _xbdev_apply_lvol_stores(xbdev_config_t *config);
int _xbdev_apply_lvol_volumes(xbdev_config_t *config);
int _xbdev_apply_snapshots(xbdev_config_t *config);
int _xbdev_apply_clones(xbdev_config_t *config);
int _xbdev_collect_current_config(xbdev_config_t *config);
void _xbdev_write_config_json(struct spdk_json_write_ctx *w, xbdev_config_t *config);

/**
 * SPDK线程上下文函数声明
 */
void xbdev_open_on_thread(void *ctx);
void xbdev_close_on_thread(void *ctx);
void xbdev_read_on_thread(void *ctx);
void xbdev_write_on_thread(void *ctx);
void xbdev_readv_on_thread(void *ctx);
void xbdev_writev_on_thread(void *ctx);
void xbdev_flush_on_thread(void *ctx);
void xbdev_unmap_on_thread(void *ctx);
void xbdev_reset_on_thread(void *ctx);
void xbdev_register_bdev_on_thread(void *ctx);
void xbdev_device_remove_on_thread(void *ctx);
void xbdev_md_dispatch_request(xbdev_request_t *req);
void lvol_create_pool_on_thread(void *ctx);
void lvol_destroy_pool_on_thread(void *ctx);
void lvol_create_on_thread(void *ctx);
void lvol_destroy_on_thread(void *ctx);
void lvol_create_snapshot_on_thread(void *ctx);
void lvol_create_clone_on_thread(void *ctx);
void lvol_rename_on_thread(void *ctx);
void lvol_resize_on_thread(void *ctx);
void lvol_get_info_on_thread(void *ctx);
void lvol_get_pool_info_on_thread(void *ctx);
void xbdev_nvme_register_on_thread(void *ctx);
void xbdev_nvmf_register_on_thread(void *ctx);
void xbdev_aio_register_on_thread(void *ctx);
void xbdev_malloc_register_on_thread(void *ctx);
void xbdev_null_register_on_thread(void *ctx);

/**
 * 队列处理函数声明
 */
void xbdev_process_request_queues(void);

/**
 * 内部函数声明
 */
xbdev_fd_entry_t* _xbdev_get_fd_entry(int fd);
xbdev_request_t* xbdev_request_alloc(void);
void xbdev_request_free(xbdev_request_t *req);
int xbdev_request_submit(xbdev_request_t *req);
xbdev_request_t* xbdev_sync_request_alloc(void);
void xbdev_sync_request_free(xbdev_request_t *req);
int xbdev_sync_request_execute(xbdev_request_t *req);

#ifdef __cplusplus
}
#endif

#endif // _XBDEV_INTERNAL_H_