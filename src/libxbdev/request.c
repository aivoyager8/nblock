/**
 * @file request.c
 * @brief 实现请求处理和分派功能
 *
 * 该文件实现各类IO请求的处理和分派逻辑，将上层API转换为SPDK调用。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/env.h>
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/util.h>
#include <spdk/bdev.h>
#include <spdk/queue.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/**
 * 读操作IO上下文
 */
struct io_read_ctx {
    int fd;             // 文件描述符
    void *buf;          // 缓冲区
    size_t count;       // 读取长度
    uint64_t offset;    // 偏移量
    int status;         // 操作状态
    bool done;          // 是否完成
};

/**
 * 写操作IO上下文
 */
struct io_write_ctx {
    int fd;             // 文件描述符
    void *buf;          // 缓冲区
    size_t count;       // 写入长度
    uint64_t offset;    // 偏移量
    int status;         // 操作状态
    bool done;          // 是否完成
};

/**
 * 向量IO上下文
 */
struct io_vector_ctx {
    int fd;                // 文件描述符
    struct iovec *iov;     // IO向量
    int iovcnt;            // 向量数量
    uint64_t offset;       // 偏移量
    bool write_op;         // 是否为写操作
    int status;            // 操作状态
    bool done;             // 是否完成
};

/**
 * 异步IO上下文
 */
struct io_async_ctx {
    int fd;                        // 文件描述符
    void *buf;                     // 缓冲区
    size_t count;                  // 操作长度
    uint64_t offset;               // 偏移量
    bool write_op;                 // 是否为写操作
    int status;                    // 操作状态
    bool done;                     // 是否完成
    xbdev_io_completion_cb user_cb;// 用户回调
    void *user_ctx;                // 用户上下文
};

/**
 * 命令IO上下文
 */
struct io_cmd_ctx {
    int fd;                        // 文件描述符
    enum spdk_bdev_io_type io_type;// IO类型
    void *cmd_args;                // 命令参数
    int status;                    // 操作状态
    bool done;                     // 是否完成
};

/**
 * 请求队列
 */
struct request_queue {
    uint32_t size;                   // 队列大小
    uint32_t head;                   // 队列头
    uint32_t tail;                   // 队列尾
    xbdev_request_t **reqs;          // 请求数组
    pthread_spinlock_t lock;         // 自旋锁
};

static struct {
    struct request_queue *queues;    // 请求队列数组
    uint32_t num_queues;            // 队列数量
    bool initialized;                // 是否已初始化
} g_request_system = {0};

/**
 * IO操作回调函数（通用）
 */
static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct {
        int *status;
        bool *done;
    } *ctx = cb_arg;
    
    // 设置状态和完成标志
    *ctx->status = success ? 0 : -EIO;
    *ctx->done = true;
    
    // 释放BDEV IO
    spdk_bdev_free_io(bdev_io);
}

/**
 * 异步IO操作回调函数
 */
static void aio_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_async_ctx *ctx = cb_arg;
    
    // 设置状态和完成标志
    ctx->status = success ? (int)ctx->count : -EIO;
    ctx->done = true;
    
    // 调用用户回调
    if (ctx->user_cb) {
        ctx->user_cb(ctx->fd, ctx->status, ctx->user_ctx);
    }
    
    // 释放BDEV IO和上下文
    spdk_bdev_free_io(bdev_io);
    free(ctx);
}

/**
 * SPDK线程上下文中执行读操作
 */
void xbdev_read_on_thread(void *arg)
{
    struct io_read_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 参数检查
    if (!ctx->buf || ctx->count == 0) {
        ctx->status = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc) {
        ctx->status = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 获取IO通道
    io_channel = spdk_bdev_get_io_channel(entry->desc);
    if (!io_channel) {
        ctx->status = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 构造回调参数
    struct {
        int *status;
        bool *done;
    } cb_arg = {
        .status = &ctx->status,
        .done = &ctx->done
    };
    
    // 执行读操作
    rc = spdk_bdev_read(entry->desc, io_channel, ctx->buf, ctx->offset, ctx->count, 
                      io_completion_cb, &cb_arg);
    
    // 释放IO通道
    spdk_put_io_channel(io_channel);
    
    // 检查提交状态
    if (rc != 0) {
        ctx->status = rc;
        ctx->done = true;
    }
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * SPDK线程上下文中执行写操作
 */
void xbdev_write_on_thread(void *arg)
{
    struct io_write_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 参数检查
    if (!ctx->buf || ctx->count == 0) {
        ctx->status = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->