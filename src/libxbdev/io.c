/**
 * @file io.c
 * @brief 实现基本的IO操作(读写等)
 *
 * 该文件实现了基本的块设备IO操作，包括同步和异步的
 * 读取、写入、刷新、取消映射和复位等操作。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/env.h>
#include <spdk/thread.h>
#include <spdk/bdev.h>
#include <spdk/log.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/**
 * 读取操作上下文
 */
struct read_ctx {
    int fd;                       // 文件描述符
    void *buf;                    // 读取缓冲区
    size_t count;                 // 读取长度(字节)
    uint64_t offset;              // 读取偏移(字节)
    struct spdk_io_channel *chan; // IO通道
    bool done;                    // 操作是否完成
    int rc;                       // 操作结果
};

/**
 * 写入操作上下文
 */
struct write_ctx {
    int fd;                       // 文件描述符
    const void *buf;              // 写入缓冲区
    size_t count;                 // 写入长度(字节)
    uint64_t offset;              // 写入偏移(字节)
    struct spdk_io_channel *chan; // IO通道
    bool done;                    // 操作是否完成
    int rc;                       // 操作结果
};

/**
 * IO操作上下文(通用)
 */
struct io_ctx {
    int fd;                       // 文件描述符
    void *buf;                    // IO缓冲区
    size_t count;                 // IO长度(字节)
    uint64_t offset;              // IO偏移(字节)
    struct spdk_io_channel *chan; // IO通道
    bool done;                    // 操作是否完成
    int rc;                       // 操作结果
    void *cb_arg;                 // 回调参数
    xbdev_io_cb cb;               // 完成回调函数
};

/**
 * 执行读取操作(SPDK线程上下文)
 *
 * @param ctx 读取上下文
 */
void xbdev_read_on_thread(void *ctx)
{
    struct read_ctx *args = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = args->offset / block_size;
    num_blocks = (args->count + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("Read out of bounds: offset=%"PRIu64", size=%zu, device_blocks=%"PRIu64"\n", 
                   args->offset, args->count, num_blocks_device);
        spdk_put_io_channel(args->chan);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 对齐缓冲区(如果需要)
    void *aligned_buf = args->buf;
    if ((uintptr_t)args->buf & (spdk_bdev_get_buf_align(bdev) - 1)) {
        // 缓冲区未对齐，需要分配对齐缓冲区
        aligned_buf = spdk_dma_malloc(args->count, spdk_bdev_get_buf_align(bdev), NULL);
        if (!aligned_buf) {
            XBDEV_ERRLOG("Failed to allocate aligned buffer: size=%zu\n", args->count);
            spdk_put_io_channel(args->chan);
            args->rc = -ENOMEM;
            args->done = true;
            return;
        }
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交读取请求
    rc = spdk_bdev_read_blocks(entry->desc, args->chan, aligned_buf, 
                             offset_blocks, num_blocks,
                             xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_read;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
            void *buf;
            uint64_t offset_blocks;
            uint64_t num_blocks;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        retry_ctx->buf = aligned_buf;
        retry_ctx->offset_blocks = offset_blocks;
        retry_ctx->num_blocks = num_blocks;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("Read failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 检查操作状态
    if (completion.status != SPDK_BDEV_IO_STATUS_SUCCESS) {
        XBDEV_ERRLOG("IO operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = -EIO;
        args->done = true;
        return;
    }
    
    // 如果使用了对齐缓冲区，复制数据到用户缓冲区
    if (aligned_buf != args->buf) {
        memcpy(args->buf, aligned_buf, args->count);
        spdk_dma_free(aligned_buf);
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    args->rc = args->count;
    args->done = true;
}

/**
 * 执行写入操作(SPDK线程上下文)
 *
 * @param ctx 写入上下文
 */
void xbdev_write_on_thread(void *ctx)
{
    struct write_ctx *args = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = args->offset / block_size;
    num_blocks = (args->count + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("Write out of bounds: offset=%"PRIu64", size=%zu, device_blocks=%"PRIu64"\n", 
                   args->offset, args->count, num_blocks_device);
        spdk_put_io_channel(args->chan);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 对齐缓冲区(如果需要)
    void *aligned_buf = (void *)args->buf;  // 去除const限制，内部使用
    if ((uintptr_t)args->buf & (spdk_bdev_get_buf_align(bdev) - 1)) {
        // 缓冲区未对齐，需要分配对齐缓冲区
        aligned_buf = spdk_dma_malloc(args->count, spdk_bdev_get_buf_align(bdev), NULL);
        if (!aligned_buf) {
            XBDEV_ERRLOG("Failed to allocate aligned buffer: size=%zu\n", args->count);
            spdk_put_io_channel(args->chan);
            args->rc = -ENOMEM;
            args->done = true;
            return;
        }
        
        // 复制数据到对齐缓冲区
        memcpy(aligned_buf, args->buf, args->count);
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交写入请求
    rc = spdk_bdev_write_blocks(entry->desc, args->chan, aligned_buf, 
                              offset_blocks, num_blocks,
                              xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_write;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
            void *buf;
            uint64_t offset_blocks;
            uint64_t num_blocks;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        retry_ctx->buf = aligned_buf;
        retry_ctx->offset_blocks = offset_blocks;
        retry_ctx->num_blocks = num_blocks;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("Write failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 检查操作状态
    if (completion.status != SPDK_BDEV_IO_STATUS_SUCCESS) {
        XBDEV_ERRLOG("IO operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = -EIO;
        args->done = true;
        return;
    }
    
    // 如果使用了对齐缓冲区，释放它
    if (aligned_buf != args->buf) {
        spdk_dma_free(aligned_buf);
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    args->rc = args->count;
    args->done = true;
}

/**
 * 同步读取操作
 *
 * @param fd 文件描述符
 * @param buf 读取缓冲区
 * @param count 读取长度(字节)
 * @param offset 读取偏移(字节)
 * @return 成功返回读取的字节数，失败返回负的错误码
 */
ssize_t xbdev_read(int fd, void *buf, size_t count, uint64_t offset)
{
    xbdev_request_t *req;
    struct read_ctx ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0 || !buf || count == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, buf=%p, count=%zu\n", fd, buf, count);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_READ;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * 同步写入操作
 *
 * @param fd 文件描述符
 * @param buf 写入缓冲区
 * @param count 写入长度(字节)
 * @param offset 写入偏移(字节)
 * @return 成功返回写入的字节数，失败返回负的错误码
 */
ssize_t xbdev_write(int fd, const void *buf, size_t count, uint64_t offset)
{
    xbdev_request_t *req;
    struct write_ctx ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0 || !buf || count == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, buf=%p, count=%zu\n", fd, buf, count);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * 异步读取回调
 *
 * 由SPDK在IO完成时调用。
 *
 * @param bdev_io BDEV IO对象
 * @param success IO是否成功
 * @param cb_arg 回调参数(IO上下文)
 */
static void _async_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_ctx *io_ctx = cb_arg;
    int result;
    
    // 确定操作结果
    if (success) {
        result = io_ctx->count;
    } else {
        XBDEV_ERRLOG("Async read failed: fd=%d, offset=%"PRIu64"\n", io_ctx->fd, io_ctx->offset);
        result = -EIO;
    }
    
    // 释放BDEV IO资源
    spdk_bdev_free_io(bdev_io);
    
    // 释放IO通道
    if (io_ctx->chan) {
        spdk_put_io_channel(io_ctx->chan);
        io_ctx->chan = NULL;
    }
    
    // 调用用户回调函数
    if (io_ctx->cb) {
        io_ctx->cb(io_ctx->cb_arg, result);
    }
    
    // 释放IO上下文
    free(io_ctx);
}

/**
 * 执行异步读取操作(SPDK线程上下文)
 *
 * @param ctx IO上下文
 */
static void _async_read_on_thread(void *ctx)
{
    struct io_ctx *io_ctx = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(io_ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", io_ctx->fd);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EBADF);
        }
        free(io_ctx);
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", io_ctx->fd);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EINVAL);
        }
        free(io_ctx);
        return;
    }
    
    // 获取IO通道
    io_ctx->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!io_ctx->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -ENOMEM);
        }
        free(io_ctx);
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = io_ctx->offset / block_size;
    num_blocks = (io_ctx->count + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("Read out of bounds: offset=%"PRIu64", size=%zu, device_blocks=%"PRIu64"\n", 
                   io_ctx->offset, io_ctx->count, num_blocks_device);
        spdk_put_io_channel(io_ctx->chan);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EINVAL);
        }
        free(io_ctx);
        return;
    }
    
    // 对齐检查
    if ((uintptr_t)io_ctx->buf & (spdk_bdev_get_buf_align(bdev) - 1)) {
        XBDEV_ERRLOG("Buffer not aligned: %p (alignment requirement: %u)\n", 
                   io_ctx->buf, spdk_bdev_get_buf_align(bdev));
        spdk_put_io_channel(io_ctx->chan);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EINVAL);
        }
        free(io_ctx);
        return;
    }
    
    // 提交读取请求
    rc = spdk_bdev_read_blocks(entry->desc, io_ctx->chan, io_ctx->buf, 
                             offset_blocks, num_blocks,
                             _async_read_complete, io_ctx);
    
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to submit async read: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        spdk_put_io_channel(io_ctx->chan);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, rc);
        }
        free(io_ctx);
    }
    // 成功情况下，回调函数将释放io_ctx
}

/**
 * 异步写入回调
 *
 * 由SPDK在IO完成时调用。
 *
 * @param bdev_io BDEV IO对象
 * @param success IO是否成功
 * @param cb_arg 回调参数(IO上下文)
 */
static void _async_write_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_ctx *io_ctx = cb_arg;
    int result;
    
    // 确定操作结果
    if (success) {
        result = io_ctx->count;
    } else {
        XBDEV_ERRLOG("Async write failed: fd=%d, offset=%"PRIu64"\n", io_ctx->fd, io_ctx->offset);
        result = -EIO;
    }
    
    // 释放BDEV IO资源
    spdk_bdev_free_io(bdev_io);
    
    // 释放IO通道
    if (io_ctx->chan) {
        spdk_put_io_channel(io_ctx->chan);
        io_ctx->chan = NULL;
    }
    
    // 调用用户回调函数
    if (io_ctx->cb) {
        io_ctx->cb(io_ctx->cb_arg, result);
    }
    
    // 释放IO上下文
    free(io_ctx);
}

/**
 * 执行异步写入操作(SPDK线程上下文)
 *
 * @param ctx IO上下文
 */
static void _async_write_on_thread(void *ctx)
{
    struct io_ctx *io_ctx = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(io_ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", io_ctx->fd);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EBADF);
        }
        free(io_ctx);
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", io_ctx->fd);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EINVAL);
        }
        free(io_ctx);
        return;
    }
    
    // 获取IO通道
    io_ctx->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!io_ctx->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -ENOMEM);
        }
        free(io_ctx);
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = io_ctx->offset / block_size;
    num_blocks = (io_ctx->count + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("Write out of bounds: offset=%"PRIu64", size=%zu, device_blocks=%"PRIu64"\n", 
                   io_ctx->offset, io_ctx->count, num_blocks_device);
        spdk_put_io_channel(io_ctx->chan);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EINVAL);
        }
        free(io_ctx);
        return;
    }
    
    // 对齐检查
    if ((uintptr_t)io_ctx->buf & (spdk_bdev_get_buf_align(bdev) - 1)) {
        XBDEV_ERRLOG("Buffer not aligned: %p (alignment requirement: %u)\n", 
                   io_ctx->buf, spdk_bdev_get_buf_align(bdev));
        spdk_put_io_channel(io_ctx->chan);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, -EINVAL);
        }
        free(io_ctx);
        return;
    }
    
    // 提交写入请求
    rc = spdk_bdev_write_blocks(entry->desc, io_ctx->chan, io_ctx->buf, 
                              offset_blocks, num_blocks,
                              _async_write_complete, io_ctx);
    
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to submit async write: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        spdk_put_io_channel(io_ctx->chan);
        if (io_ctx->cb) {
            io_ctx->cb(io_ctx->cb_arg, rc);
        }
        free(io_ctx);
    }
    // 成功情况下，回调函数将释放io_ctx
}

/**
 * 异步读取操作
 *
 * @param fd 文件描述符
 * @param buf 读取缓冲区
 * @param count 读取长度(字节)
 * @param offset 读取偏移(字节)
 * @param cb_arg 回调参数
 * @param cb 完成回调函数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_read(int fd, void *buf, size_t count, uint64_t offset, void *cb_arg, xbdev_io_cb cb)
{
    xbdev_request_t *req;
    struct io_ctx *io_ctx;
    int rc;
    
    // 参数检查
    if (fd < 0 || !buf || count == 0 || !cb) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, buf=%p, count=%zu, cb=%p\n", fd, buf, count, cb);
        return -EINVAL;
    }
    
    // 分配IO上下文
    io_ctx = calloc(1, sizeof(struct io_ctx));
    if (!io_ctx) {
        XBDEV_ERRLOG("Failed to allocate IO context\n");
        return -ENOMEM;
    }
    
    // 设置IO上下文
    io_ctx->fd = fd;
    io_ctx->buf = buf;
    io_ctx->count = count;
    io_ctx->offset = offset;
    io_ctx->cb = cb;
    io_ctx->cb_arg = cb_arg;
    io_ctx->done = false;
    io_ctx->rc = 0;
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        free(io_ctx);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_READ;
    req->ctx = io_ctx;
    req->cb = NULL;  // 不使用请求级别回调，而是使用IO完成回调
    req->cb_arg = NULL;
    
    // 提交请求
    rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to submit request: %d\n", rc);
        free(io_ctx);
        xbdev_request_free(req);
        return rc;
    }
    
    // 请求已提交，资源将由完成回调释放
    return 0;
}

/**
 * 异步写入操作
 *
 * @param fd 文件描述符
 * @param buf 写入缓冲区
 * @param count 写入长度(字节)
 * @param offset 写入偏移(字节)
 * @param cb_arg 回调参数
 * @param cb 完成回调函数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_write(int fd, const void *buf, size_t count, uint64_t offset, void *cb_arg, xbdev_io_cb cb)
{
    xbdev_request_t *req;
    struct io_ctx *io_ctx;
    int rc;
    
    // 参数检查
    if (fd < 0 || !buf || count == 0 || !cb) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, buf=%p, count=%zu, cb=%p\n", fd, buf, count, cb);
        return -EINVAL;
    }
    
    // 分配IO上下文
    io_ctx = calloc(1, sizeof(struct io_ctx));
    if (!io_ctx) {
        XBDEV_ERRLOG("Failed to allocate IO context\n");
        return -ENOMEM;
    }
    
    // 设置IO上下文
    io_ctx->fd = fd;
    io_ctx->buf = (void *)buf;  // 去除const限制，内部使用
    io_ctx->count = count;
    io_ctx->offset = offset;
    io_ctx->cb = cb;
    io_ctx->cb_arg = cb_arg;
    io_ctx->done = false;
    io_ctx->rc = 0;
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        free(io_ctx);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITE;
    req->ctx = io_ctx;
    req->cb = NULL;  // 不使用请求级别回调，而是使用IO完成回调
    req->cb_arg = NULL;
    
    // 提交请求
    rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to submit request: %d\n", rc);
        free(io_ctx);
        xbdev_request_free(req);
        return rc;
    }
    
    // 请求已提交，资源将由完成回调释放
    return 0;
}

/**
 * 刷新操作上下文
 */
struct flush_ctx {
    int fd;                       // 文件描述符
    struct spdk_io_channel *chan; // IO通道
    bool done;                    // 操作是否完成
    int rc;                       // 操作结果
};

/**
 * 执行刷新操作(SPDK线程上下文)
 *
 * @param ctx 刷新上下文
 */
void xbdev_flush_on_thread(void *ctx)
{
    struct flush_ctx *args = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交刷新请求
    rc = spdk_bdev_flush_blocks(entry->desc, args->chan, 0, 
                              spdk_bdev_get_num_blocks(bdev),
                              xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_flush;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("Flush failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 检查操作状态
    if (completion.status != SPDK_BDEV_IO_STATUS_SUCCESS) {
        XBDEV_ERRLOG("IO operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        spdk_put_io_channel(args->chan);
        args->rc = -EIO;
        args->done = true;
        return;
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    args->rc = 0;
    args->done = true;
}

/**
 * 同步刷新操作
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_flush(int fd)
{
    xbdev_request_t *req;
    struct flush_ctx ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_FLUSH;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * UNMAP操作上下文
 */
struct unmap_ctx {
    int fd;                       // 文件描述符
    uint64_t offset;              // 起始偏移(字节)
    uint64_t length;              // 长度(字节)
    struct spdk_io_channel *chan; // IO通道
    bool done;                    // 操作是否完成
    int rc;                       // 操作结果
};

/**
 * 执行UNMAP操作(SPDK线程上下文)
 *
 * @param ctx UNMAP上下文
 */
void xbdev_unmap_on_thread(void *ctx)
{
    struct unmap_ctx *args = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = args->offset / block_size;
    num_blocks = (args->length + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("UNMAP out of bounds: offset=%"PRIu64", length=%"PRIu64", device_blocks=%"PRIu64"\n", 
                   args->offset, args->length, num_blocks_device);
        spdk_put_io_channel(args->chan);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交UNMAP请求
    rc = spdk_bdev_unmap_blocks(entry->desc, args->chan, offset_blocks, num_blocks,
                              xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_unmap;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
            uint64_t offset_blocks;
            uint64_t num_blocks;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        retry_ctx->offset_blocks = offset_blocks;
        retry_ctx->num_blocks = num_blocks;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("UNMAP failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 检查操作状态
    if (completion.status != SPDK_BDEV_IO_STATUS_SUCCESS) {
        XBDEV_ERRLOG("IO operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        spdk_put_io_channel(args->chan);
        args->rc = -EIO;
        args->done = true;
        return;
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    args->rc = 0;
    args->done = true;
}

/**
 * 同步UNMAP操作
 *
 * @param fd 文件描述符
 * @param offset 起始偏移(字节)
 * @param length 长度(字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_unmap(int fd, uint64_t offset, uint64_t length)
{
    xbdev_request_t *req;
    struct unmap_ctx ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0 || length == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, offset=%"PRIu64", length=%"PRIu64"\n", 
                   fd, offset, length);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.offset = offset;
    ctx.length = length;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_UNMAP;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * 重置操作上下文
 */
struct reset_ctx {
    int fd;                       // 文件描述符
    struct spdk_io_channel *chan; // IO通道
    bool done;                    // 操作是否完成
    int rc;                       // 操作结果
};

/**
 * 执行设备重置操作(SPDK线程上下文)
 *
 * @param ctx 重置上下文
 */
void xbdev_reset_on_thread(void *ctx)
{
    struct reset_ctx *args = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 60 * 1000000;  // 60秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交重置请求
    rc = spdk_bdev_reset(entry->desc, args->chan, xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_reset;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("Reset failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 检查操作状态
    if (completion.status != SPDK_BDEV_IO_STATUS_SUCCESS) {
        XBDEV_ERRLOG("IO operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        spdk_put_io_channel(args->chan);
        args->rc = -EIO;
        args->done = true;
        return;
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    args->rc = 0;
    args->done = true;
}

/**
 * 同步设备重置操作
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_reset(int fd)
{
    xbdev_request_t *req;
    struct reset_ctx ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_RESET;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * 从文件描述符获取设备名称
 *
 * @param fd 文件描述符
 * @param name 输出缓冲区
 * @param name_len 缓冲区长度
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_name(int fd, char *name, size_t name_len)
{
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (fd < 0 || !name || name_len == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, name=%p, name_len=%zu\n", fd, name, name_len);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    // 检查BDEV是否有效
    if (!entry->bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return -EINVAL;
    }
    
    // 获取设备名称
    const char *bdev_name = spdk_bdev_get_name(entry->bdev);
    if (!bdev_name) {
        XBDEV_ERRLOG("Failed to get BDEV name: fd=%d\n", fd);
        return -EINVAL;
    }
    
    // 确保缓冲区足够大
    if (strnlen(bdev_name, name_len) >= name_len) {
        XBDEV_ERRLOG("Buffer too small: required=%zu, provided=%zu\n", 
                   strnlen(bdev_name, name_len) + 1, name_len);
        return -EOVERFLOW;
    }
    
    // 复制名称到输出缓冲区
    snprintf(name, name_len, "%s", bdev_name);
    
    return 0;
}

/**
 * 从文件描述符获取设备信息
 *
 * @param fd 文件描述符
 * @param info 输出参数，存储设备信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_device_info(int fd, xbdev_device_info_t *info)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    
    // 参数检查
    if (fd < 0 || !info) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, info=%p\n", fd, info);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return -EINVAL;
    }
    
    // 填充设备信息
    snprintf(info->name, sizeof(info->name), "%s", spdk_bdev_get_name(bdev));
    snprintf(info->product_name, sizeof(info->product_name), "%s", spdk_bdev_get_product_name(bdev));
    
    info->block_size = spdk_bdev_get_block_size(bdev);
    info->num_blocks = spdk_bdev_get_num_blocks(bdev);
    info->size_bytes = info->block_size * info->num_blocks;
    info->write_cache = spdk_bdev_has_write_cache(bdev);
    info->md_size = spdk_bdev_get_md_size(bdev);
    info->optimal_io_boundary = spdk_bdev_get_optimal_io_boundary(bdev);
    info->required_alignment = spdk_bdev_get_buf_align(bdev);
    info->claimed = spdk_bdev_is_claimed(bdev);
    
    // 获取支持的IO类型
    info->supported_io_types = 0;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ))
        info->supported_io_types |= XBDEV_IO_TYPE_READ;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE))
        info->supported_io_types |= XBDEV_IO_TYPE_WRITE;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP))
        info->supported_io_types |= XBDEV_IO_TYPE_UNMAP;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH))
        info->supported_io_types |= XBDEV_IO_TYPE_FLUSH;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESET))
        info->supported_io_types |= XBDEV_IO_TYPE_RESET;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE))
        info->supported_io_types |= XBDEV_IO_TYPE_COMPARE;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE))
        info->supported_io_types |= XBDEV_IO_TYPE_COMPARE_AND_WRITE;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES))
        info->supported_io_types |= XBDEV_IO_TYPE_WRITE_ZEROES;
    
    // 获取BDEV驱动类型
    snprintf(info->driver, sizeof(info->driver), "%s", spdk_bdev_get_module_name(bdev));
    
    return 0;
}

/**
 * 获取设备IO统计信息
 *
 * @param fd 文件描述符
 * @param stats 输出参数，存储IO统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_io_stats(int fd, xbdev_io_stats_t *stats)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    struct spdk_bdev_io_stat bdev_stats;
    xbdev_request_t *req;
    struct {
        int fd;
        xbdev_io_stats_t *stats;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0 || !stats) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, stats=%p\n", fd, stats);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.stats = stats;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_GET_STATS;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * SPDK线程上下文中获取IO统计信息
 */
void xbdev_get_io_stats_on_thread(void *ctx)
{
    struct {
        int fd;
        xbdev_io_stats_t *stats;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } *args = ctx;
    
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    struct spdk_bdev_io_stat bdev_stats = {0};
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 获取IO统计信息
    spdk_bdev_get_io_stat(bdev, args->chan, &bdev_stats);
    
    // 转换为库的统计结构
    args->stats->bytes_read = bdev_stats.bytes_read;
    args->stats->bytes_written = bdev_stats.bytes_written;
    args->stats->num_read_ops = bdev_stats.num_read_ops;
    args->stats->num_write_ops = bdev_stats.num_write_ops;
    args->stats->num_unmap_ops = bdev_stats.num_unmap_ops;
    args->stats->read_latency_ticks = bdev_stats.read_latency_ticks;
    args->stats->write_latency_ticks = bdev_stats.write_latency_ticks;
    args->stats->unmap_latency_ticks = bdev_stats.unmap_latency_ticks;
    
    // 计算平均延迟(微秒)
    uint64_t ticks_per_us = spdk_get_ticks_hz() / 1000000;
    if (ticks_per_us == 0) ticks_per_us = 1; // 防止除零
    
    if (args->stats->num_read_ops > 0) {
        args->stats->avg_read_latency_us = 
            args->stats->read_latency_ticks / ticks_per_us / args->stats->num_read_ops;
    } else {
        args->stats->avg_read_latency_us = 0;
    }
    
    if (args->stats->num_write_ops > 0) {
        args->stats->avg_write_latency_us = 
            args->stats->write_latency_ticks / ticks_per_us / args->stats->num_write_ops;
    } else {
        args->stats->avg_write_latency_us = 0;
    }
    
    if (args->stats->num_unmap_ops > 0) {
        args->stats->avg_unmap_latency_us = 
            args->stats->unmap_latency_ticks / ticks_per_us / args->stats->num_unmap_ops;
    } else {
        args->stats->avg_unmap_latency_us = 0;
    }
    
    // 计算IOPS和带宽
    uint64_t time_in_sec = spdk_get_ticks() / spdk_get_ticks_hz();
    if (time_in_sec > 0) {
        args->stats->read_iops = args->stats->num_read_ops / time_in_sec;
        args->stats->write_iops = args->stats->num_write_ops / time_in_sec;
        args->stats->read_bw_mbps = args->stats->bytes_read / (1024*1024) / time_in_sec;
        args->stats->write_bw_mbps = args->stats->bytes_written / (1024*1024) / time_in_sec;
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    args->chan = NULL;
    
    args->rc = 0;
    args->done = true;
}

/**
 * 重置设备IO统计信息
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_reset_io_stats(int fd)
{
    xbdev_request_t *req;
    struct {
        int fd;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_RESET_STATS;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * SPDK线程上下文中重置IO统计信息
 */
void xbdev_reset_io_stats_on_thread(void *ctx)
{
    struct {
        int fd;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } *args = ctx;
    
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 重置IO统计信息
    spdk_bdev_reset_io_stat(bdev, args->chan);
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    args->chan = NULL;
    
    args->rc = 0;
    args->done = true;
}

/**
 * 设置设备的IO队列深度
 *
 * @param fd 文件描述符
 * @param queue_depth 队列深度
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_queue_depth(int fd, uint16_t queue_depth)
{
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (fd < 0 || queue_depth == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, queue_depth=%u\n", fd, queue_depth);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    if (!entry->bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return -EINVAL;
    }
    
    // SPDK目前不支持直接更改队列深度，此函数仅作为接口预留
    XBDEV_NOTICELOG("Setting queue depth is not implemented\n");
    
    return -ENOTSUP;
}

/**
 * 异步执行IO批处理请求上下文
 */
struct batch_io_ctx {
    int fd;                          // 文件描述符
    struct spdk_io_channel *chan;    // IO通道
    xbdev_io_batch_t *batch;         // 批量IO请求
    int completed;                   // 已完成的请求数
    int success;                     // 成功的请求数
    void *cb_arg;                    // 回调参数
    xbdev_batch_cb cb;               // 完成回调函数
};

/**
 * 批量IO完成回调
 */
static void _batch_io_complete(void *cb_arg, int rc)
{
    struct batch_io_ctx *batch_ctx = cb_arg;
    
    batch_ctx->completed++;
    if (rc >= 0) {
        batch_ctx->success++;
    }
    
    // 检查是否所有请求都已完成
    if (batch_ctx->completed == batch_ctx->batch->num_ops) {
        // 调用用户回调函数
        if (batch_ctx->cb) {
            batch_ctx->cb(batch_ctx->cb_arg, batch_ctx->success, batch_ctx->batch->num_ops);
        }
        
        // 释放资源
        free(batch_ctx);
    }
}

/**
 * 执行IO批处理(SPDK线程上下文)
 */
static void _batch_io_on_thread(void *ctx)
{
    struct batch_io_ctx *batch_ctx = ctx;
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(batch_ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", batch_ctx->fd);
        if (batch_ctx->cb) {
            batch_ctx->cb(batch_ctx->cb_arg, 0, batch_ctx->batch->num_ops);
        }
        free(batch_ctx);
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", batch_ctx->fd);
        if (batch_ctx->cb) {
            batch_ctx->cb(batch_ctx->cb_arg, 0, batch_ctx->batch->num_ops);
        }
        free(batch_ctx);
        return;
    }
    
    // 获取IO通道
    batch_ctx->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!batch_ctx->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        if (batch_ctx->cb) {
            batch_ctx->cb(batch_ctx->cb_arg, 0, batch_ctx->batch->num_ops);
        }
        free(batch_ctx);
        return;
    }
    
    // 提交批处理中的每个IO操作
    for (int i = 0; i < batch_ctx->batch->num_ops; i++) {
        xbdev_io_op_t *op = &batch_ctx->batch->ops[i];
        
        // 执行相应的IO操作
        switch (op->type) {
            case XBDEV_OP_READ:
                xbdev_aio_read(batch_ctx->fd, op->buf, op->count, op->offset, 
                             batch_ctx, _batch_io_complete);
                break;
                
            case XBDEV_OP_WRITE:
                xbdev_aio_write(batch_ctx->fd, op->buf, op->count, op->offset, 
                              batch_ctx, _batch_io_complete);
                break;
                
            default:
                XBDEV_ERRLOG("Unsupported IO operation type: %d\n", op->type);
                _batch_io_complete(batch_ctx, -EINVAL);
                break;
        }
    }
    
    // 释放IO通道(在最后一个操作完成后释放)
    // 注意: 资源会在最后一个IO操作完成后释放
}

/**
 * 创建IO批处理
 *
 * @param max_ops 最大操作数
 * @return 成功返回批处理句柄，失败返回NULL
 */
xbdev_io_batch_t *xbdev_io_batch_create(int max_ops)
{
    xbdev_io_batch_t *batch;
    
    // 参数检查
    if (max_ops <= 0) {
        XBDEV_ERRLOG("Invalid number of operations: %d\n", max_ops);
        return NULL;
    }
    
    // 分配批处理结构
    batch = calloc(1, sizeof(xbdev_io_batch_t) + max_ops * sizeof(xbdev_io_op_t));
    if (!batch) {
        XBDEV_ERRLOG("Failed to allocate batch structure\n");
        return NULL;
    }
    
    batch->max_ops = max_ops;
    batch->num_ops = 0;
    
    return batch;
}

/**
 * 向批处理添加IO操作
 *
 * @param batch 批处理句柄
 * @param fd 文件描述符
 * @param buf 数据缓冲区
 * @param count 数据长度
 * @param offset 操作偏移量
 * @param type 操作类型(读/写)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_batch_add(xbdev_io_batch_t *batch, int fd, void *buf, size_t count, 
                      uint64_t offset, int type)
{
    // 参数检查
    if (!batch || fd < 0 || !buf || count == 0) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // 检查操作类型
    if (type != XBDEV_OP_READ && type != XBDEV_OP_WRITE) {
        XBDEV_ERRLOG("Unsupported IO operation type: %d\n", type);
        return -EINVAL;
    }
    
    // 检查批处理是否已满
    if (batch->num_ops >= batch->max_ops) {
        XBDEV_ERRLOG("Batch is full: num_ops=%d, max_ops=%d\n", 
                   batch->num_ops, batch->max_ops);
        return -ENOSPC;
    }
    
    // 添加操作到批处理
    xbdev_io_op_t *op = &batch->ops[batch->num_ops];
    op->fd = fd;
    op->buf = buf;
    op->count = count;
    op->offset = offset;
    op->type = type;
    
    batch->num_ops++;
    
    return 0;
}

/**
 * 提交IO批处理
 *
 * @param batch 批处理句柄
 * @param cb_arg 回调参数
 * @param cb 完成回调函数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_batch_submit(xbdev_io_batch_t *batch, void *cb_arg, xbdev_batch_cb cb)
{
    xbdev_request_t *req;
    struct batch_io_ctx *batch_ctx;
    int rc;
    
    // 参数检查
    if (!batch || batch->num_ops == 0) {
        XBDEV_ERRLOG("Invalid batch: batch=%p, num_ops=%d\n", 
                   batch, batch ? batch->num_ops : 0);
        return -EINVAL;
    }
    
    // 所有操作必须使用相同的文件描述符
    int fd = batch->ops[0].fd;
    for (int i = 1; i < batch->num_ops; i++) {
        if (batch->ops[i].fd != fd) {
            XBDEV_ERRLOG("All operations in the batch must use the same file descriptor\n");
            return -EINVAL;
        }
    }
    
    // 分配批处理上下文
    batch_ctx = calloc(1, sizeof(struct batch_io_ctx));
    if (!batch_ctx) {
        XBDEV_ERRLOG("Failed to allocate batch context\n");
        return -ENOMEM;
    }
    
    // 设置上下文
    batch_ctx->fd = fd;
    batch_ctx->batch = batch;
    batch_ctx->cb = cb;
    batch_ctx->cb_arg = cb_arg;
    batch_ctx->completed = 0;
    batch_ctx->success = 0;
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        free(batch_ctx);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_BATCH;
    req->ctx = batch_ctx;
    req->cb = NULL;
    req->cb_arg = NULL;
    
    // 提交请求
    rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to submit request: %d\n", rc);
        free(batch_ctx);
        xbdev_request_free(req);
        return rc;
    }
    
    // 请求已提交，资源将由完成回调释放
    return 0;
}

/**
 * 释放IO批处理
 *
 * @param batch 批处理句柄
 */
void xbdev_io_batch_free(xbdev_io_batch_t *batch)
{
    if (batch) {
        free(batch);
    }
}

/**
 * 同步执行IO批处理
 *
 * @param batch 批处理句柄
 * @return 成功返回成功的操作数，失败返回负的错误码
 */
int xbdev_io_batch_execute(xbdev_io_batch_t *batch)
{
    struct {
        bool done;
        int success_count;
        int total_count;
    } ctx = {0};
    
    // 参数检查
    if (!batch || batch->num_ops == 0) {
        XBDEV_ERRLOG("Invalid batch: batch=%p, num_ops=%d\n", 
                   batch, batch ? batch->num_ops : 0);
        return -EINVAL;
    }
    
    // 同步批处理回调
    xbdev_batch_cb sync_cb = (xbdev_batch_cb)({ 
        void _sync_batch_cb(void *arg, int success, int total) {
            struct { bool done; int success_count; int total_count; } *ctx = arg;
            ctx->success_count = success;
            ctx->total_count = total;
            ctx->done = true;
        }
        _sync_batch_cb;
    });
    
    // 提交批处理
    int rc = xbdev_io_batch_submit(batch, &ctx, sync_cb);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to submit batch: %d\n", rc);
        return rc;
    }
    
    // 等待批处理完成
    while (!ctx.done) {
        // 轮询完成事件
        xbdev_poll_completions(10, 1000); // 处理最多10个事件，超时1ms
    }
    
    return ctx.success_count;
}

/**
 * 检查设备是否支持指定的IO类型
 *
 * @param fd 文件描述符
 * @param io_type IO类型
 * @return 如果支持返回true，否则返回false
 */
bool xbdev_io_type_supported(int fd, uint32_t io_type)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return false;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return false;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return false;
    }
    
    // 检查IO类型支持
    switch (io_type) {
        case XBDEV_IO_TYPE_READ:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ);
            
        case XBDEV_IO_TYPE_WRITE:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE);
            
        case XBDEV_IO_TYPE_UNMAP:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP);
            
        case XBDEV_IO_TYPE_FLUSH:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH);
            
        case XBDEV_IO_TYPE_RESET:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESET);
            
        case XBDEV_IO_TYPE_COMPARE:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE);
            
        case XBDEV_IO_TYPE_COMPARE_AND_WRITE:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE);
            
        case XBDEV_IO_TYPE_WRITE_ZEROES:
            return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
            
        default:
            XBDEV_ERRLOG("Unknown IO type: %u\n", io_type);
            return false;
    }
}

/**
 * 同步写零操作
 *
 * @param fd 文件描述符
 * @param offset 起始偏移(字节)
 * @param length 长度(字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_write_zeroes(int fd, uint64_t offset, uint64_t length)
{
    xbdev_request_t *req;
    struct unmap_ctx ctx = {0}; // 复用unmap上下文结构，字段相同
    int rc;
    
    // 参数检查
    if (fd < 0 || length == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, offset=%"PRIu64", length=%"PRIu64"\n", 
                   fd, offset, length);
        return -EINVAL;
    }
    
    // 检查是否支持write_zeroes
    if (!xbdev_io_type_supported(fd, XBDEV_IO_TYPE_WRITE_ZEROES)) {
        XBDEV_ERRLOG("Device does not support write_zeroes operation\n");
        return -ENOTSUP;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.offset = offset;
    ctx.length = length;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITE_ZEROES;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * 执行写零操作(SPDK线程上下文)
 *
 * @param ctx 上下文
 */
void xbdev_write_zeroes_on_thread(void *ctx)
{
    struct unmap_ctx *args = ctx; // 复用unmap上下文结构，字段相同
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = args->offset / block_size;
    num_blocks = (args->length + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("WRITE_ZEROES out of bounds: offset=%"PRIu64", length=%"PRIu64", device_blocks=%"PRIu64"\n", 
                   args->offset, args->length, num_blocks_device);
        spdk_put_io_channel(args->chan);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交WRITE_ZEROES请求
    rc = spdk_bdev_write_zeroes_blocks(entry->desc, args->chan, offset_blocks, num_blocks,
                                     xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_write_zeroes;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
            uint64_t offset_blocks;
            uint64_t num_blocks;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        retry_ctx->offset_blocks = offset_blocks;
        retry_ctx->num_blocks = num_blocks;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("WRITE_ZEROES failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 检查操作状态
    if (completion.status != SPDK_BDEV_IO_STATUS_SUCCESS) {
        XBDEV_ERRLOG("IO operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        spdk_put_io_channel(args->chan);
        args->rc = -EIO;
        args->done = true;
        return;
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    args->rc = 0;
    args->done = true;
}

/**
 * 分配DMA对齐的内存缓冲区
 *
 * @param size 缓冲区大小
 * @return 成功返回缓冲区指针，失败返回NULL
 */
void *xbdev_dma_buffer_alloc(size_t size)
{
    void *buf;
    
    // 参数检查
    if (size == 0) {
        XBDEV_ERRLOG("Invalid buffer size: %zu\n", size);
        return NULL;
    }
    
    // 分配DMA对齐内存
    buf = spdk_dma_malloc(size, 0, NULL);
    if (!buf) {
        XBDEV_ERRLOG("Failed to allocate DMA buffer: size=%zu\n", size);
        return NULL;
    }
    
    return buf;
}

/**
 * 释放DMA对齐的内存缓冲区
 *
 * @param buf 缓冲区指针
 */
void xbdev_dma_buffer_free(void *buf)
{
    if (buf) {
        spdk_dma_free(buf);
    }
}

/**
 * 同步比较操作
 *
 * @param fd 文件描述符
 * @param buf 比较缓冲区
 * @param count 比较长度(字节)
 * @param offset 比较偏移(字节)
 * @return 成功并且数据相同返回0，数据不同返回1，失败返回负的错误码
 */
int xbdev_compare(int fd, const void *buf, size_t count, uint64_t offset)
{
    xbdev_request_t *req;
    struct {
        int fd;
        const void *buf;
        size_t count;
        uint64_t offset;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0 || !buf || count == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, buf=%p, count=%zu\n", fd, buf, count);
        return -EINVAL;
    }
    
    // 检查是否支持compare
    if (!xbdev_io_type_supported(fd, XBDEV_IO_TYPE_COMPARE)) {
        XBDEV_ERRLOG("Device does not support compare operation\n");
        return -ENOTSUP;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_COMPARE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * 执行比较操作(SPDK线程上下文)
 *
 * @param ctx 上下文
 */
void xbdev_compare_on_thread(void *ctx)
{
    struct {
        int fd;
        const void *buf;
        size_t count;
        uint64_t offset;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } *args = ctx;
    
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = args->offset / block_size;
    num_blocks = (args->count + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("COMPARE out of bounds: offset=%"PRIu64", size=%zu, device_blocks=%"PRIu64"\n", 
                   args->offset, args->count, num_blocks_device);
        spdk_put_io_channel(args->chan);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 对齐缓冲区(如果需要)
    void *aligned_buf = (void *)args->buf;  // 去除const限制，内部使用
    if ((uintptr_t)args->buf & (spdk_bdev_get_buf_align(bdev) - 1)) {
        // 缓冲区未对齐，需要分配对齐缓冲区
        aligned_buf = spdk_dma_malloc(args->count, spdk_bdev_get_buf_align(bdev), NULL);
        if (!aligned_buf) {
            XBDEV_ERRLOG("Failed to allocate aligned buffer: size=%zu\n", args->count);
            spdk_put_io_channel(args->chan);
            args->rc = -ENOMEM;
            args->done = true;
            return;
        }
        
        // 复制数据到对齐缓冲区
        memcpy(aligned_buf, args->buf, args->count);
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交COMPARE请求
    rc = spdk_bdev_compare_blocks(entry->desc, args->chan, aligned_buf, 
                                offset_blocks, num_blocks,
                                xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_compare;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
            void *buf;
            uint64_t offset_blocks;
            uint64_t num_blocks;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        retry_ctx->buf = aligned_buf;
        retry_ctx->offset_blocks = offset_blocks;
        retry_ctx->num_blocks = num_blocks;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("COMPARE failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        if (aligned_buf != args->buf) {
            spdk_dma_free(aligned_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 如果使用了对齐缓冲区，释放它
    if (aligned_buf != args->buf) {
        spdk_dma_free(aligned_buf);
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    if (completion.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
        // 比较成功，数据相同
        args->rc = 0;
    } else if (completion.status == SPDK_BDEV_IO_STATUS_MISCOMPARE) {
        // 数据不同
        args->rc = 1;
    } else {
        // 其他错误
        XBDEV_ERRLOG("COMPARE operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        args->rc = -EIO;
    }
    
    args->done = true;
}

/**
 * 同步比较并写入操作
 *
 * @param fd 文件描述符
 * @param compare_buf 比较缓冲区
 * @param write_buf 写入缓冲区
 * @param count 操作长度(字节)
 * @param offset 操作偏移(字节)
 * @return 成功返回0，数据不匹配返回1，失败返回负的错误码
 */
int xbdev_compare_and_write(int fd, const void *compare_buf, const void *write_buf, 
                          size_t count, uint64_t offset)
{
    xbdev_request_t *req;
    struct {
        int fd;
        const void *compare_buf;
        const void *write_buf;
        size_t count;
        uint64_t offset;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (fd < 0 || !compare_buf || !write_buf || count == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, compare_buf=%p, write_buf=%p, count=%zu\n", 
                   fd, compare_buf, write_buf, count);
        return -EINVAL;
    }
    
    // 检查是否支持compare_and_write
    if (!xbdev_io_type_supported(fd, XBDEV_IO_TYPE_COMPARE_AND_WRITE)) {
        XBDEV_ERRLOG("Device does not support compare_and_write operation\n");
        return -ENOTSUP;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.compare_buf = compare_buf;
    ctx.write_buf = write_buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_COMPARE_AND_WRITE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    // 返回结果
    return ctx.rc;
}

/**
 * 执行比较并写入操作(SPDK线程上下文)
 *
 * @param ctx 上下文
 */
void xbdev_compare_and_write_on_thread(void *ctx)
{
    struct {
        int fd;
        const void *compare_buf;
        const void *write_buf;
        size_t count;
        uint64_t offset;
        struct spdk_io_channel *chan;
        bool done;
        int rc;
    } *args = ctx;
    
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", args->fd);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 获取IO通道
    args->chan = spdk_bdev_get_io_channel(entry->desc);
    if (!args->chan) {
        XBDEV_ERRLOG("Failed to get IO channel: %s\n", spdk_bdev_get_name(bdev));
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = args->offset / block_size;
    num_blocks = (args->count + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("COMPARE_AND_WRITE out of bounds: offset=%"PRIu64", size=%zu, device_blocks=%"PRIu64"\n", 
                   args->offset, args->count, num_blocks_device);
        spdk_put_io_channel(args->chan);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 对齐缓冲区(如果需要)
    void *aligned_compare_buf = (void *)args->compare_buf;  // 去除const限制，内部使用
    void *aligned_write_buf = (void *)args->write_buf;
    
    // 检查比较缓冲区对齐
    bool compare_buf_allocated = false;
    if ((uintptr_t)args->compare_buf & (spdk_bdev_get_buf_align(bdev) - 1)) {
        // 缓冲区未对齐，需要分配对齐缓冲区
        aligned_compare_buf = spdk_dma_malloc(args->count, spdk_bdev_get_buf_align(bdev), NULL);
        if (!aligned_compare_buf) {
            XBDEV_ERRLOG("Failed to allocate aligned compare buffer: size=%zu\n", args->count);
            spdk_put_io_channel(args->chan);
            args->rc = -ENOMEM;
            args->done = true;
            return;
        }
        compare_buf_allocated = true;
        // 复制数据到对齐缓冲区
        memcpy(aligned_compare_buf, args->compare_buf, args->count);
    }
    
    // 检查写入缓冲区对齐
    bool write_buf_allocated = false;
    if ((uintptr_t)args->write_buf & (spdk_bdev_get_buf_align(bdev) - 1)) {
        // 缓冲区未对齐，需要分配对齐缓冲区
        aligned_write_buf = spdk_dma_malloc(args->count, spdk_bdev_get_buf_align(bdev), NULL);
        if (!aligned_write_buf) {
            XBDEV_ERRLOG("Failed to allocate aligned write buffer: size=%zu\n", args->count);
            if (compare_buf_allocated) {
                spdk_dma_free(aligned_compare_buf);
            }
            spdk_put_io_channel(args->chan);
            args->rc = -ENOMEM;
            args->done = true;
            return;
        }
        write_buf_allocated = true;
        // 复制数据到对齐缓冲区
        memcpy(aligned_write_buf, args->write_buf, args->count);
    }
    
    // 设置IO完成结构
    completion.done = false;
    completion.status = SPDK_BDEV_IO_STATUS_PENDING;
    
    // 提交COMPARE_AND_WRITE请求
    rc = spdk_bdev_comparev_and_writev_blocks(entry->desc, args->chan,
                                           aligned_compare_buf, aligned_write_buf,
                                           offset_blocks, num_blocks,
                                           xbdev_sync_io_completion_cb, &completion);
    
    if (rc == -ENOMEM) {
        // 资源暂时不足，设置等待回调
        struct spdk_bdev_io_wait_entry bdev_io_wait;
        
        bdev_io_wait.bdev = bdev;
        bdev_io_wait.cb_fn = xbdev_sync_io_retry_compare_and_write;
        bdev_io_wait.cb_arg = &completion;
        
        // 保存重试上下文
        struct {
            struct spdk_bdev *bdev;
            struct spdk_bdev_desc *desc;
            struct spdk_io_channel *channel;
            void *compare_buf;
            void *write_buf;
            uint64_t offset_blocks;
            uint64_t num_blocks;
        } *retry_ctx = (void *)((uint8_t *)&completion + sizeof(completion));
        
        retry_ctx->bdev = bdev;
        retry_ctx->desc = entry->desc;
        retry_ctx->channel = args->chan;
        retry_ctx->compare_buf = aligned_compare_buf;
        retry_ctx->write_buf = aligned_write_buf;
        retry_ctx->offset_blocks = offset_blocks;
        retry_ctx->num_blocks = num_blocks;
        
        // 将等待条目放入队列
        spdk_bdev_queue_io_wait(bdev, args->chan, &bdev_io_wait);
    } else if (rc != 0) {
        XBDEV_ERRLOG("COMPARE_AND_WRITE failed: %s, rc=%d\n", spdk_bdev_get_name(bdev), rc);
        if (compare_buf_allocated) {
            spdk_dma_free(aligned_compare_buf);
        }
        if (write_buf_allocated) {
            spdk_dma_free(aligned_write_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 等待IO完成
    rc = _xbdev_wait_for_completion(&completion.done, timeout_us);
    if (rc != 0) {
        XBDEV_ERRLOG("IO wait timeout: %s\n", spdk_bdev_get_name(bdev));
        if (compare_buf_allocated) {
            spdk_dma_free(aligned_compare_buf);
        }
        if (write_buf_allocated) {
            spdk_dma_free(aligned_write_buf);
        }
        spdk_put_io_channel(args->chan);
        args->rc = rc;
        args->done = true;
        return;
    }
    
    // 释放对齐缓冲区
    if (compare_buf_allocated) {
        spdk_dma_free(aligned_compare_buf);
    }
    if (write_buf_allocated) {
        spdk_dma_free(aligned_write_buf);
    }
    
    // 释放IO通道
    spdk_put_io_channel(args->chan);
    
    // 设置操作结果
    if (completion.status == SPDK_BDEV_IO_STATUS_SUCCESS) {
        // 比较成功并且写入成功，数据相同
        args->rc = 0;
    } else if (completion.status == SPDK_BDEV_IO_STATUS_MISCOMPARE) {
        // 比较失败，数据不同，没有执行写入
        args->rc = 1;
    } else {
        // 其他错误
        XBDEV_ERRLOG("COMPARE_AND_WRITE operation failed: %s, status=%d\n", 
                   spdk_bdev_get_name(bdev), completion.status);
        args->rc = -EIO;
    }
    
    args->done = true;
}

/**
 * 轮询IO完成事件
 *
 * @param max_events 最大处理的事件数，0表示无限制
 * @param timeout_us 超时时间(微秒)，0表示非阻塞立即返回
 * @return 成功处理的事件数
 */
int xbdev_poll_completions(uint32_t max_events, uint64_t timeout_us)
{
    int count = 0;
    uint64_t start_time = spdk_get_ticks();
    uint64_t end_time = start_time + timeout_us * spdk_get_ticks_hz() / 1000000ULL;
    
    // 如果无限制，设置为最大值
    if (max_events == 0) {
        max_events = UINT32_MAX;
    }
    
    do {
        // 处理完成队列
        count += _xbdev_process_completions(max_events - count);
        
        // 如果已经处理了足够的事件或者无需超时等待，直接返回
        if (count >= max_events || timeout_us == 0) {
            break;
        }
        
        // 轮询SPDK事件，并适当休眠
        if (spdk_get_ticks() < end_time) {
            // 短暂休眠避免CPU占用过高
            usleep(10);
        }
    } while (spdk_get_ticks() < end_time);
    
    return count;
}

/**
 * 预取数据到缓存
 *
 * @param fd 文件描述符
 * @param offset 起始偏移(字节)
 * @param length 长度(字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_readahead(int fd, uint64_t offset, uint64_t length)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    
    // 参数检查
    if (fd < 0 || length == 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, offset=%"PRIu64", length=%"PRIu64"\n", 
                   fd, offset, length);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return -EINVAL;
    }
    
    // 计算块偏移和数量
    uint32_t block_size = spdk_bdev_get_block_size(bdev);
    uint64_t num_blocks_device = spdk_bdev_get_num_blocks(bdev);
    
    offset_blocks = offset / block_size;
    num_blocks = (length + block_size - 1) / block_size;  // 向上取整
    
    // 检查边界条件
    if (offset_blocks >= num_blocks_device || 
        offset_blocks + num_blocks > num_blocks_device) {
        XBDEV_ERRLOG("READAHEAD out of bounds: offset=%"PRIu64", length=%"PRIu64", device_blocks=%"PRIu64"\n", 
                   offset, length, num_blocks_device);
        return -EINVAL;
    }
    
    // SPDK不直接支持readahead操作，这里是一个占位实现
    // 在实际使用中，我们可能需要在这里添加特定后端的预取优化
    
    XBDEV_NOTICELOG("READAHEAD request received but not executed: current SPDK architecture does not support explicit readahead operation\n");
    
    return 0;
}

/**
 * 设置文件描述符的通用选项
 *
 * @param fd 文件描述符
 * @param option 选项代码
 * @param value 选项值
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_option(int fd, int option, uint64_t value)
{
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    // 根据选项类型设置不同的选项
    switch (option) {
        case XBDEV_OPT_TIMEOUT_MS:
            // 设置IO超时值
            entry->timeout_ms = value;
            XBDEV_NOTICELOG("Set IO timeout for file descriptor %d to %"PRIu64" ms\n", fd, value);
            break;
            
        case XBDEV_OPT_RETRY_COUNT:
            // 设置IO重试次数
            entry->retry_count = value;
            XBDEV_NOTICELOG("Set IO retry count for file descriptor %d to %"PRIu64"\n", fd, value);
            break;
            
        case XBDEV_OPT_QUEUE_DEPTH:
            // 设置队列深度
            return xbdev_set_queue_depth(fd, (uint16_t)value);
            
        case XBDEV_OPT_CACHE_POLICY:
            // 设置缓存策略
            if (value > XBDEV_CACHE_POLICY_MAX) {
                XBDEV_ERRLOG("Invalid cache policy value: %"PRIu64"\n", value);
                return -EINVAL;
            }
            entry->cache_policy = value;
            XBDEV_NOTICELOG("Set cache policy for file descriptor %d to %"PRIu64"\n", fd, value);
            break;
            
        default:
            XBDEV_ERRLOG("Unknown option code: %d\n", option);
            return -EINVAL;
    }
    
    return 0;
}

/**
 * 获取文件描述符的通用选项
 *
 * @param fd 文件描述符
 * @param option 选项代码
 * @param value 选项值输出参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_option(int fd, int option, uint64_t *value)
{
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (fd < 0 || !value) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, value=%p\n", fd, value);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    // 根据选项类型获取不同的选项
    switch (option) {
        case XBDEV_OPT_TIMEOUT_MS:
            // 获取IO超时值
            *value = entry->timeout_ms;
            break;
            
        case XBDEV_OPT_RETRY_COUNT:
            // 获取IO重试次数
            *value = entry->retry_count;
            break;
            
        case XBDEV_OPT_QUEUE_DEPTH:
            // 获取队列深度
            // 注意：当前SPDK不支持直接获取队列深度
            *value = 0;  // 默认返回0
            XBDEV_NOTICELOG("Getting queue depth is not implemented\n");
            return -ENOTSUP;
            
        case XBDEV_OPT_CACHE_POLICY:
            // 获取缓存策略
            *value = entry->cache_policy;
            break;
            
        default:
            XBDEV_ERRLOG("Unknown option code: %d\n", option);
            return -EINVAL;
    }
    
    return 0;
}

/**
 * 批量读取多个区域
 *
 * @param fd 文件描述符
 * @param iovs 读取区域数组
 * @param iovcnt 读取区域数量
 * @param offset 读取起始偏移(字节)
 * @return 成功返回读取的字节数，失败返回负的错误码
 */
ssize_t xbdev_readv(int fd, const struct iovec *iovs, int iovcnt, uint64_t offset)
{
    // 参数检查
    if (fd < 0 || !iovs || iovcnt <= 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, iovs=%p, iovcnt=%d\n", fd, iovs, iovcnt);
        return -EINVAL;
    }
    
    // 计算总长度并分配批处理
    size_t total_length = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iovs[i].iov_base || iovs[i].iov_len == 0) {
            XBDEV_ERRLOG("Invalid iovec element #%d: base=%p, len=%zu\n", 
                       i, iovs[i].iov_base, iovs[i].iov_len);
            return -EINVAL;
        }
        total_length += iovs[i].iov_len;
    }
    
    // 创建批处理
    xbdev_io_batch_t *batch = xbdev_io_batch_create(iovcnt);
    if (!batch) {
        XBDEV_ERRLOG("Failed to create IO batch\n");
        return -ENOMEM;
    }
    
    // 添加每个区域的读取操作
    uint64_t current_offset = offset;
    for (int i = 0; i < iovcnt; i++) {
        int rc = xbdev_io_batch_add(batch, fd, iovs[i].iov_base, iovs[i].iov_len, 
                                 current_offset, XBDEV_OP_READ);
        if (rc != 0) {
            XBDEV_ERRLOG("Failed to add read operation: #%d, rc=%d\n", i, rc);
            xbdev_io_batch_free(batch);
            return rc;
        }
        current_offset += iovs[i].iov_len;
    }
    
    // 执行批处理
    int success_count = xbdev_io_batch_execute(batch);
    
    // 释放批处理资源
    xbdev_io_batch_free(batch);
    
    // 检查执行结果
    if (success_count < 0) {
        XBDEV_ERRLOG("Failed to execute batch read: rc=%d\n", success_count);
        return success_count;
    } else if (success_count != iovcnt) {
        XBDEV_WARNLOG("Batch read partially succeeded: %d/%d\n", success_count, iovcnt);
        return -EIO;
    }
    
    // 返回读取的总字节数
    return total_length;
}

/**
 * 批量写入多个区域
 *
 * @param fd 文件描述符
 * @param iovs 写入区域数组
 * @param iovcnt 写入区域数量
 * @param offset 写入起始偏移(字节)
 * @return 成功返回写入的字节数，失败返回负的错误码
 */
ssize_t xbdev_writev(int fd, const struct iovec *iovs, int iovcnt, uint64_t offset)
{
    // 参数检查
    if (fd < 0 || !iovs || iovcnt <= 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, iovs=%p, iovcnt=%d\n", fd, iovs, iovcnt);
        return -EINVAL;
    }
    
    // 计算总长度并分配批处理
    size_t total_length = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iovs[i].iov_base || iovs[i].iov_len == 0) {
            XBDEV_ERRLOG("Invalid iovec element #%d: base=%p, len=%zu\n", 
                       i, iovs[i].iov_base, iovs[i].iov_len);
            return -EINVAL;
        }
        total_length += iovs[i].iov_len;
    }
    
    // 创建批处理
    xbdev_io_batch_t *batch = xbdev_io_batch_create(iovcnt);
    if (!batch) {
        XBDEV_ERRLOG("Failed to create IO batch\n");
        return -ENOMEM;
    }
    
    // 添加每个区域的写入操作
    uint64_t current_offset = offset;
    for (int i = 0; i < iovcnt; i++) {
        int rc = xbdev_io_batch_add(batch, fd, iovs[i].iov_base, iovs[i].iov_len,
                                 current_offset, XBDEV_OP_WRITE);
        if (rc != 0) {
            XBDEV_ERRLOG("Failed to add write operation: #%d, rc=%d\n", i, rc);
            xbdev_io_batch_free(batch);
            return rc;
        }
        current_offset += iovs[i].iov_len;
    }
    
    // 执行批处理
    int success_count = xbdev_io_batch_execute(batch);
    
    // 释放批处理资源
    xbdev_io_batch_free(batch);
    
    // 检查执行结果
    if (success_count < 0) {
        XBDEV_ERRLOG("Failed to execute batch write: rc=%d\n", success_count);
        return success_count;
    } else if (success_count != iovcnt) {
        XBDEV_WARNLOG("Batch write partially succeeded: %d/%d\n", success_count, iovcnt);
        return -EIO;
    }
    
    // 返回写入的总字节数
    return total_length;
}

/**
 * 批量操作：在同一个原子事务中执行多个读写操作
 * 
 * @param fd 文件描述符
 * @param ops 操作数组
 * @param ops_cnt 操作数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_atomic_batch(int fd, xbdev_atomic_op_t *ops, int ops_cnt)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    struct spdk_io_channel *chan = NULL;
    struct xbdev_sync_io_completion completion = {0};
    uint64_t timeout_us = 30 * 1000000;  // 30秒超时
    int rc;
    
    // 参数检查
    if (fd < 0 || !ops || ops_cnt <= 0) {
        XBDEV_ERRLOG("Invalid parameters: fd=%d, ops=%p, ops_cnt=%d\n", fd, ops, ops_cnt);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return -EBADF;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return -EINVAL;
    }
    
    // 目前SPDK没有直接支持原子批处理操作的接口
    // 这里返回不支持错误，实际实现时可以使用批处理接口
    XBDEV_ERRLOG("Atomic batch operation not implemented\n");
    return -ENOTSUP;
}

/**
 * 获取设备对齐要求
 *
 * @param fd 文件描述符
 * @return 成功返回对齐要求(字节)，失败返回0
 */
uint32_t xbdev_get_alignment(int fd)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return 0;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return 0;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return 0;
    }
    
    // 返回设备的对齐要求
    return spdk_bdev_get_buf_align(bdev);
}

/**
 * 获取设备块大小
 *
 * @param fd 文件描述符
 * @return 成功返回块大小(字节)，失败返回0
 */
uint32_t xbdev_get_block_size(int fd)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return 0;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return 0;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return 0;
    }
    
    // 返回设备的块大小
    return spdk_bdev_get_block_size(bdev);
}

/**
 * 获取设备块数量
 *
 * @param fd 文件描述符
 * @return 成功返回块数量，失败返回0
 */
uint64_t xbdev_get_num_blocks(int fd)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    
    // 参数检查
    if (fd < 0) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return 0;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("Invalid file descriptor: %d\n", fd);
        return 0;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        XBDEV_ERRLOG("Invalid BDEV: fd=%d\n", fd);
        return 0;
    }
    
    // 返回设备的块数量
    return spdk_bdev_get_num_blocks(bdev);
}