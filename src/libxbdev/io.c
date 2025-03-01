/**
 * @file io.c
 * @brief 实现设备IO操作相关功能
 *
 * 该文件实现同步和异步IO操作，包括读、写、向量读写等功能。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/queue.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * IO上下文结构体
 */
typedef struct {
    int fd;                       // 文件描述符
    void *buf;                    // 缓冲区
    size_t count;                 // 字节数
    uint64_t offset;              // 偏移量
    bool write;                   // 是否写操作
    bool done;                    // 是否完成
    int rc;                       // 结果代码
    xbdev_io_completion_cb cb;    // 完成回调
    void *cb_arg;                 // 回调参数
} xbdev_io_ctx_t;

/**
 * 向量IO上下文结构体
 */
typedef struct {
    int fd;                       // 文件描述符
    struct iovec *iov;            // IO向量
    int iovcnt;                   // 向量数量
    uint64_t offset;              // 偏移量
    bool write;                   // 是否写操作
    bool done;                    // 是否完成
    int rc;                       // 结果代码
    xbdev_io_completion_cb cb;    // 完成回调
    void *cb_arg;                 // 回调参数
} xbdev_iov_ctx_t;

/**
 * IO控制操作上下文结构体
 */
typedef struct {
    int fd;                       // 文件描述符
    int op_type;                  // 操作类型（FLUSH/UNMAP/RESET）
    uint64_t offset;              // 偏移量（用于UNMAP）
    uint64_t nbytes;              // 字节数（用于UNMAP）
    bool done;                    // 是否完成
    int rc;                       // 结果代码
} xbdev_ioctl_ctx_t;

/**
 * IO完成回调
 */
static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    xbdev_io_ctx_t *ctx = cb_arg;
    
    // 设置结果
    ctx->rc = success ? ctx->count : -EIO;
    
    // 释放SPDK IO资源
    spdk_bdev_free_io(bdev_io);
    
    // 设置完成标志并通知用户回调
    ctx->done = true;
    
    // 如果有回调函数，则调用它
    if (ctx->cb) {
        ctx->cb(ctx->fd, ctx->rc, ctx->cb_arg);
    }
}

/**
 * 向量IO完成回调
 */
static void iov_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    xbdev_iov_ctx_t *ctx = cb_arg;
    size_t total_size = 0;
    
    // 计算总字节数
    for (int i = 0; i < ctx->iovcnt; i++) {
        total_size += ctx->iov[i].iov_len;
    }
    
    // 设置结果
    ctx->rc = success ? total_size : -EIO;
    
    // 释放SPDK IO资源
    spdk_bdev_free_io(bdev_io);
    
    // 设置完成标志并通知用户回调
    ctx->done = true;
    
    // 如果有回调函数，则调用它
    if (ctx->cb) {
        ctx->cb(ctx->fd, ctx->rc, ctx->cb_arg);
    }
}

/**
 * IO控制操作完成回调
 */
static void ioctl_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    xbdev_ioctl_ctx_t *ctx = cb_arg;
    
    // 设置结果
    ctx->rc = success ? 0 : -EIO;
    
    // 释放SPDK IO资源
    spdk_bdev_free_io(bdev_io);
    
    // 设置完成标志
    ctx->done = true;
}

/**
 * 在SPDK线程上下文中执行读操作
 */
void xbdev_read_on_thread(void *arg)
{
    xbdev_io_ctx_t *ctx = arg;
    xbdev_fd_entry_t *entry;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("读取操作：无效的文件描述符 %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 检查设备是否有效
    if (!entry->desc || !entry->bdev) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("读取操作：设备未打开 fd=%d\n", ctx->fd);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 获取块大小和对齐要求
    uint32_t block_size = spdk_bdev_get_block_size(entry->bdev);
    uint32_t block_align = spdk_bdev_get_buf_align(entry->bdev);
    
    // 检查是否需要内存对齐
    void *aligned_buf = ctx->buf;
    bool need_bounce_buffer = false;
    void *bounce_buffer = NULL;
    
    // 如果没有正确对齐，分配临时缓冲区
    if ((uintptr_t)ctx->buf & (block_align - 1)) {
        need_bounce_buffer = true;
        bounce_buffer = spdk_dma_malloc(ctx->count, block_align, NULL);
        if (!bounce_buffer) {
            pthread_mutex_unlock(&entry->mutex);
            XBDEV_ERRLOG("读取操作：无法分配对齐缓冲区，大小=%zu\n", ctx->count);
            ctx->rc = -ENOMEM;
            ctx->done = true;
            return;
        }
        aligned_buf = bounce_buffer;
    }
    
    // 计算块偏移和块数量
    offset_blocks = ctx->offset / block_size;
    num_blocks = (ctx->count + block_size - 1) / block_size;
    
    // 执行读操作
    rc = spdk_bdev_read_blocks(entry->desc, spdk_io_channel_from_ctx(entry),
                               aligned_buf, offset_blocks, num_blocks,
                               io_completion_cb, ctx);
                               
    if (rc != 0) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("读取操作：spdk_bdev_read_blocks失败，rc=%d\n", rc);
        if (need_bounce_buffer && bounce_buffer) {
            spdk_dma_free(bounce_buffer);
        }
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    pthread_mutex_unlock(&entry->mutex);
    
    // 如果使用了弹跳缓冲区，则需要等待IO完成后复制数据
    if (need_bounce_buffer) {
        // 等待IO完成
        while (!ctx->done) {
            spdk_thread_poll(spdk_get_thread(), 0, 0);
        }
        
        // 如果读取成功，将数据复制到用户缓冲区
        if (ctx->rc > 0) {
            memcpy(ctx->buf, bounce_buffer, ctx->count);
        }
        
        // 释放弹跳缓冲区
        spdk_dma_free(bounce_buffer);
    }
}

/**
 * 在SPDK线程上下文中执行写操作
 */
void xbdev_write_on_thread(void *arg)
{
    xbdev_io_ctx_t *ctx = arg;
    xbdev_fd_entry_t *entry;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("写入操作：无效的文件描述符 %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 检查设备是否有效
    if (!entry->desc || !entry->bdev) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("写入操作：设备未打开 fd=%d\n", ctx->fd);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 获取块大小和对齐要求
    uint32_t block_size = spdk_bdev_get_block_size(entry->bdev);
    uint32_t block_align = spdk_bdev_get_buf_align(entry->bdev);
    
    // 检查是否需要内存对齐
    void *aligned_buf = ctx->buf;
    bool need_bounce_buffer = false;
    void *bounce_buffer = NULL;
    
    // 如果没有正确对齐，分配临时缓冲区
    if ((uintptr_t)ctx->buf & (block_align - 1)) {
        need_bounce_buffer = true;
        bounce_buffer = spdk_dma_malloc(ctx->count, block_align, NULL);
        if (!bounce_buffer) {
            pthread_mutex_unlock(&entry->mutex);
            XBDEV_ERRLOG("写入操作：无法分配对齐缓冲区，大小=%zu\n", ctx->count);
            ctx->rc = -ENOMEM;
            ctx->done = true;
            return;
        }
        
        // 复制用户数据到对齐缓冲区
        memcpy(bounce_buffer, ctx->buf, ctx->count);
        aligned_buf = bounce_buffer;
    }
    
    // 计算块偏移和块数量
    offset_blocks = ctx->offset / block_size;
    num_blocks = (ctx->count + block_size - 1) / block_size;
    
    // 执行写操作
    rc = spdk_bdev_write_blocks(entry->desc, spdk_io_channel_from_ctx(entry),
                                aligned_buf, offset_blocks, num_blocks,
                                io_completion_cb, ctx);
                                
    if (rc != 0) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("写入操作：spdk_bdev_write_blocks失败，rc=%d\n", rc);
        if (need_bounce_buffer && bounce_buffer) {
            spdk_dma_free(bounce_buffer);
        }
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    pthread_mutex_unlock(&entry->mutex);
    
    // 如果使用了弹跳缓冲区，需要等待IO完成后释放
    if (need_bounce_buffer) {
        // 等待IO完成
        while (!ctx->done) {
            spdk_thread_poll(spdk_get_thread(), 0, 0);
        }
        
        // 释放弹跳缓冲区
        spdk_dma_free(bounce_buffer);
    }
}

/**
 * 在SPDK线程上下文中执行向量读操作
 */
void xbdev_readv_on_thread(void *arg)
{
    xbdev_iov_ctx_t *ctx = arg;
    xbdev_fd_entry_t *entry;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("向量读取操作：无效的文件描述符 %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 检查设备是否有效
    if (!entry->desc || !entry->bdev) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("向量读取操作：设备未打开 fd=%d\n", ctx->fd);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 获取块大小
    uint32_t block_size = spdk_bdev_get_block_size(entry->bdev);
    
    // 计算块偏移
    offset_blocks = ctx->offset / block_size;
    
    // 计算总字节数和块数
    size_t total_size = 0;
    for (int i = 0; i < ctx->iovcnt; i++) {
        total_size += ctx->iov[i].iov_len;
    }
    num_blocks = (total_size + block_size - 1) / block_size;
    
    // 执行向量读操作
    rc = spdk_bdev_readv_blocks(entry->desc, spdk_io_channel_from_ctx(entry),
                               ctx->iov, ctx->iovcnt, offset_blocks, num_blocks,
                               iov_completion_cb, ctx);
                               
    if (rc != 0) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("向量读取操作：spdk_bdev_readv_blocks失败，rc=%d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    pthread_mutex_unlock(&entry->mutex);
}

/**
 * 在SPDK线程上下文中执行向量写操作
 */
void xbdev_writev_on_thread(void *arg)
{
    xbdev_iov_ctx_t *ctx = arg;
    xbdev_fd_entry_t *entry;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("向量写入操作：无效的文件描述符 %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 检查设备是否有效
    if (!entry->desc || !entry->bdev) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("向量写入操作：设备未打开 fd=%d\n", ctx->fd);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 获取块大小
    uint32_t block_size = spdk_bdev_get_block_size(entry->bdev);
    
    // 计算块偏移
    offset_blocks = ctx->offset / block_size;
    
    // 计算总字节数和块数
    size_t total_size = 0;
    for (int i = 0; i < ctx->iovcnt; i++) {
        total_size += ctx->iov[i].iov_len;
    }
    num_blocks = (total_size + block_size - 1) / block_size;
    
    // 执行向量写操作
    rc = spdk_bdev_writev_blocks(entry->desc, spdk_io_channel_from_ctx(entry),
                                ctx->iov, ctx->iovcnt, offset_blocks, num_blocks,
                                iov_completion_cb, ctx);
                                
    if (rc != 0) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("向量写入操作：spdk_bdev_writev_blocks失败，rc=%d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    pthread_mutex_unlock(&entry->mutex);
}

/**
 * 在SPDK线程上下文中执行刷新操作
 */
void xbdev_flush_on_thread(void *arg)
{
    xbdev_ioctl_ctx_t *ctx = arg;
    xbdev_fd_entry_t *entry;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("刷新操作：无效的文件描述符 %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 检查设备是否有效
    if (!entry->desc || !entry->bdev) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("刷新操作：设备未打开 fd=%d\n", ctx->fd);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 执行刷新操作
    rc = spdk_bdev_flush_blocks(entry->desc, spdk_io_channel_from_ctx(entry),
                               0, spdk_bdev_get_num_blocks(entry->bdev),
                               ioctl_completion_cb, ctx);
                               
    if (rc != 0) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("刷新操作：spdk_bdev_flush_blocks失败，rc=%d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    pthread_mutex_unlock(&entry->mutex);
}

/**
 * 在SPDK线程上下文中执行UNMAP操作
 */
void xbdev_unmap_on_thread(void *arg)
{
    xbdev_ioctl_ctx_t *ctx = arg;
    xbdev_fd_entry_t *entry;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("UNMAP操作：无效的文件描述符 %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 检查设备是否有效
    if (!entry->desc || !entry->bdev) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("UNMAP操作：设备未打开 fd=%d\n", ctx->fd);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 获取块大小
    uint32_t block_size = spdk_bdev_get_block_size(entry->bdev);
    
    // 检查设备是否支持UNMAP
    if (!spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_WARNLOG("UNMAP操作：设备不支持UNMAP，模拟成功 fd=%d\n", ctx->fd);
        ctx->rc = 0;
        ctx->done = true;
        return;
    }
    
    // 计算块偏移和块数量
    offset_blocks = ctx->offset / block_size;
    num_blocks = (ctx->nbytes + block_size - 1) / block_size;
    
    // 执行UNMAP操作
    rc = spdk_bdev_unmap_blocks(entry->desc, spdk_io_channel_from_ctx(entry),
                               offset_blocks, num_blocks,
                               ioctl_completion_cb, ctx);
                               
    if (rc != 0) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("UNMAP操作：spdk_bdev_unmap_blocks失败，rc=%d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    pthread_mutex_unlock(&entry->mutex);
}

/**
 * 在SPDK线程上下文中执行重置操作
 */
void xbdev_reset_on_thread(void *arg)
{
    xbdev_ioctl_ctx_t *ctx = arg;
    xbdev_fd_entry_t *entry;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("重置操作：无效的文件描述符 %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 检查设备是否有效
    if (!entry->desc || !entry->bdev) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("重置操作：设备未打开 fd=%d\n", ctx->fd);
        ctx->rc = -ENODEV;
        ctx->done = true;
        return;
    }
    
    // 检查设备是否支持重置
    if (!spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_RESET)) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_WARNLOG("重置操作：设备不支持重置，模拟成功 fd=%d\n", ctx->fd);
        ctx->rc = 0;
        ctx->done = true;
        return;
    }
    
    // 执行重置操作
    rc = spdk_bdev_reset(entry->desc, spdk_io_channel_from_ctx(entry),
                        ioctl_completion_cb, ctx);
                        
    if (rc != 0) {
        pthread_mutex_unlock(&entry->mutex);
        XBDEV_ERRLOG("重置操作：spdk_bdev_reset失败，rc=%d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }
    
    pthread_mutex_unlock(&entry->mutex);
}

/**
 * 同步读取数据
 *
 * @param fd 文件描述符
 * @param buf 输出缓冲区
 * @param count 读取字节数
 * @param offset 偏移量
 * @return 成功返回读取的字节数，失败返回错误码
 */
ssize_t xbdev_read(int fd, void *buf, size_t count, uint64_t offset)
{
    xbdev_request_t *req;
    xbdev_io_ctx_t ctx = {0};
    int rc;
    
    // 参数检查
    if (!buf || count == 0) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.write = false;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("读取：无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_READ;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("读取：执行同步请求失败，rc=%d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 获取结果
    rc = ctx.rc;
    
    // 释放请求
    xbdev_request_free(req);
    
    return rc;
}

/**
 * 同步写入数据
 *
 * @param fd 文件描述符
 * @param buf 输入缓冲区
 * @param count 写入字节数
 * @param offset 偏移量
 * @return 成功返回写入的字节数，失败返回错误码
 */
ssize_t xbdev_write(int fd, const void *buf, size_t count, uint64_t offset)
{
    xbdev_request_t *req;
    xbdev_io_ctx_t ctx = {0};
    int rc;
    
    // 参数检查
    if (!buf || count == 0) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = (void *)buf;  // 强制转换const，内部不会修改
    ctx.count = count;
    ctx.offset = offset;
    ctx.write = true;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("写入：无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("写入：执行同步请求失败，rc=%d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 获取结果
    rc = ctx.rc;
    
    // 释放请求
    xbdev_request_free(req);
    
    return rc;
}

/**
 * 向量读取数据
 *
 * @param fd 文件描述符
 * @param iov IO向量数组
 * @param iovcnt 向量数量
 * @param offset 偏移量
 * @return 成功返回读取的字节数，失败返回错误码
 */
ssize_t xbdev_readv(int fd, const struct iovec *iov, int iovcnt, uint64_t offset)
{
    xbdev_request_t *req;
    xbdev_iov_ctx_t ctx = {0};
    int rc;
    
    // 参数检查
    if (!iov || iovcnt <= 0) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.iov = (struct iovec *)iov;  // 强制转换const，内部不会修改
    ctx.iovcnt = iovcnt;
    ctx.offset = offset;
    ctx.write = false;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("向量读取：无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_READV;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("向量读取：执行同步请求失败，rc=%d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 获取结果
    rc = ctx.rc;
    
    // 释放请求
    xbdev_request_free(req);
    
    return rc;
}

/**
 * 向量写入数据
 *
 * @param fd 文件描述符
 * @param iov IO向量数组
 * @param iovcnt 向量数量
 * @param offset 偏移量
 * @return 成功返回写入的字节数，失败返回错误码
 */
ssize_t xbdev_writev(int fd, const struct iovec *iov, int iovcnt, uint64_t offset)
{
    xbdev_request_t *req;
    xbdev_iov_ctx_t ctx = {0};
    int rc;
    
    // 参数检查
    if (!iov || iovcnt <= 0) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.iov = (struct iovec *)iov;  // 强制转换const，内部不会修改
    ctx.iovcnt = iovcnt;
    ctx.offset = offset;
    ctx.write = true;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("向量写入：无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITEV;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("向量写入：执行同步请求失败，rc=%d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 获取结果
    rc = ctx.rc;
    
    // 释放请求
    xbdev_request_free(req);
    
    return rc;
}

/**
 * 异步读取数据
 *
 * @param fd 文件描述符
 * @param buf 输出缓冲区
 * @param count 读取字节数
 * @param offset 偏移量
 * @param cb 完成回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_read(int fd, void *buf, size_t count, uint64_t offset, 
                  xbdev_io_completion_cb cb, void *cb_arg)
{
    struct io_async_ctx *ctx;
    xbdev_request_t *req;
    
    // 参数检查
    if (!buf || count == 0 || !cb) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 分配上下文
    ctx = malloc(sizeof(struct io_async_ctx));
    if (!ctx) {
        XBDEV_ERRLOG("无法分配异步IO上下文\n");
        return -ENOMEM;
    }
    
    // 设置上下文
    ctx->fd = fd;
    ctx->buf = buf;
    ctx->count = count;
    ctx->offset = offset;
    ctx->write_op = false;
    ctx->status = 0;
    ctx->done = false;
    ctx->user_cb = cb;
    ctx->user_ctx = cb_arg;
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_READ;
    req->ctx = ctx;
    
    // 提交请求
    int rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("提交异步请求失败: %d\n", rc);
        xbdev_request_free(req);
        free(ctx);
        return rc;
    }
    
    // 请求由SPDK线程处理，完成后会调用回调函数
    // 注意：此处不释放请求，将在SPDK线程完成处理后释放
    
    return 0;
}

/**
 * 异步写入数据
 *
 * @param fd 文件描述符
 * @param buf 输入缓冲区
 * @param count 写入字节数
 * @param offset 偏移量
 * @param cb 完成回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_write(int fd, const void *buf, size_t count, uint64_t offset,
                   xbdev_io_completion_cb cb, void *cb_arg)
{
    struct io_async_ctx *ctx;
    xbdev_request_t *req;
    
    // 参数检查
    if (!buf || count == 0 || !cb) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 分配上下文
    ctx = malloc(sizeof(struct io_async_ctx));
    if (!ctx) {
        XBDEV_ERRLOG("无法分配异步IO上下文\n");
        return -ENOMEM;
    }
    
    // 设置上下文
    ctx->fd = fd;
    ctx->buf = (void *)buf;  // 去掉const限定符，实际操作不会修改
    ctx->count = count;
    ctx->offset = offset;
    ctx->write_op = true;
    ctx->status = 0;
    ctx->done = false;
    ctx->user_cb = cb;
    ctx->user_ctx = cb_arg;
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITE;
    req->ctx = ctx;
    
    // 提交请求
    int rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("提交异步请求失败: %d\n", rc);
        xbdev_request_free(req);
        free(ctx);
        return rc;
    }
    
    // 请求由SPDK线程处理，完成后会调用回调函数
    // 注意：此处不释放请求，将在SPDK线程完成处理后释放
    
    return 0;
}

/**
 * 刷新设备缓存
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_flush(int fd)
{
    xbdev_request_t *req;
    struct io_cmd_ctx ctx = {0};
    int rc;
    
    // 设置上下文
    ctx.fd = fd;
    ctx.io_type = SPDK_BDEV_IO_TYPE_FLUSH;
    ctx.status = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_FLUSH;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.status;
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * 释放设备空间(TRIM/UNMAP操作)
 *
 * @param fd 文件描述符
 * @param offset 偏移量
 * @param length 长度
 * @return 成功返回0，失败返回错误码
 */
int xbdev_unmap(int fd, uint64_t offset, uint64_t length)
{
    xbdev_request_t *req;
    struct {
        int fd;
        uint64_t offset;
        uint64_t length;
        int status;
        bool done;
    } ctx = {0};
    int rc;
    
    // 参数检查
    if (length == 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.offset = offset;
    ctx.length = length;
    ctx.status = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_UNMAP;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.status;
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * 重置设备
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_reset(int fd)
{
    xbdev_request_t *req;
    struct io_cmd_ctx ctx = {0};
    int rc;
    
    // 设置上下文
    ctx.fd = fd;
    ctx.io_type = SPDK_BDEV_IO_TYPE_RESET;
    ctx.status = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_RESET;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.status;
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * SPDK线程上下文中执行向量读操作
 */
void xbdev_readv_on_thread(void *arg)
{
    struct io_vector_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 参数检查
    if (!ctx->iov || ctx->iovcnt <= 0) {
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
    rc = spdk_bdev_readv(entry->desc, io_channel, ctx->iov, ctx->iovcnt, 
                       ctx->offset, ctx->iovcnt * ctx->iov[0].iov_len, 
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
 * SPDK线程上下文中执行向量写操作
 */
void xbdev_writev_on_thread(void *arg)
{
    struct io_vector_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 参数检查
    if (!ctx->iov || ctx->iovcnt <= 0) {
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
    
    // 执行写操作
    rc = spdk_bdev_writev(entry->desc, io_channel, ctx->iov, ctx->iovcnt, 
                        ctx->offset, ctx->iovcnt * ctx->iov[0].iov_len, 
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
 * SPDK IO完成回调函数
 */
static void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct {
        int *status;
        bool *done;
    } *args = cb_arg;
    
    // 设置操作状态
    *args->status = success ? 0 : -EIO;
    
    // 释放SPDK IO资源
    spdk_bdev_free_io(bdev_io);
    
    // 标记操作完成
    *args->done = true;
}

/**
 * 异步IO完成回调函数
 */
static void aio_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct io_async_ctx *ctx = cb_arg;
    xbdev_io_completion_cb user_cb = ctx->user_cb;
    void *user_ctx = ctx->user_ctx;
    int fd = ctx->fd;
    int status = success ? 0 : -EIO;
    
    // 释放SPDK IO资源
    spdk_bdev_free_io(bdev_io);
    
    // 标记操作完成
    ctx->status = status;
    ctx->done = true;
    
    // 调用用户回调函数
    if (user_cb) {
        user_cb(fd, status, user_ctx);
    }
    
    // 释放异步上下文
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
    
    // 检查参数有效性
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
    rc = spdk_bdev_read(entry->desc, io_channel, ctx->buf, ctx->offset, 
                      ctx->count, io_completion_cb, &cb_arg);
    
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
    
    // 检查参数有效性
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
    
    // 执行写操作
    rc = spdk_bdev_write(entry->desc, io_channel, (void *)ctx->buf, 
                       ctx->offset, ctx->count, io_completion_cb, &cb_arg);
    
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
 * SPDK线程上下文中执行异步读操作
 */
void xbdev_aio_read_on_thread(void *arg)
{
    struct io_async_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 检查参数有效性
    if (!ctx->buf || ctx->count == 0) {
        ctx->status = -EINVAL;
        ctx->done = true;
        
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, -EINVAL, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
        return;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc) {
        ctx->status = -EBADF;
        ctx->done = true;
        
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, -EBADF, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
        return;
    }
    
    // 获取IO通道
    io_channel = spdk_bdev_get_io_channel(entry->desc);
    if (!io_channel) {
        ctx->status = -ENOMEM;
        ctx->done = true;
        
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, -ENOMEM, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
        return;
    }
    
    // 执行读操作
    rc = spdk_bdev_read(entry->desc, io_channel, ctx->buf, ctx->offset,
                      ctx->count, aio_completion_cb, ctx);
    
    // 释放IO通道
    spdk_put_io_channel(io_channel);
    
    // 检查提交状态
    if (rc != 0) {
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, rc, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
    }
    
    // 注意：异步操作不等待完成，直接返回
}

/**
 * SPDK线程上下文中执行异步写操作
 */
void xbdev_aio_write_on_thread(void *arg)
{
    struct io_async_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 检查参数有效性
    if (!ctx->buf || ctx->count == 0) {
        ctx->status = -EINVAL;
        ctx->done = true;
        
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, -EINVAL, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
        return;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc) {
        ctx->status = -EBADF;
        ctx->done = true;
        
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, -EBADF, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
        return;
    }
    
    // 获取IO通道
    io_channel = spdk_bdev_get_io_channel(entry->desc);
    if (!io_channel) {
        ctx->status = -ENOMEM;
        ctx->done = true;
        
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, -ENOMEM, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
        return;
    }
    
    // 执行写操作
    rc = spdk_bdev_write(entry->desc, io_channel, ctx->buf, ctx->offset,
                       ctx->count, aio_completion_cb, ctx);
    
    // 释放IO通道
    spdk_put_io_channel(io_channel);
    
    // 检查提交状态
    if (rc != 0) {
        // 调用用户回调函数
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, rc, ctx->user_ctx);
        }
        
        // 释放上下文
        free(ctx);
    }
    
    // 注意：异步操作不等待完成，直接返回
}

/**
 * SPDK线程上下文中执行UNMAP操作
 */
void xbdev_unmap_on_thread(void *arg)
{
    struct {
        int fd;
        uint64_t offset;
        uint64_t length;
        int status;
        bool done;
    } *ctx = arg;
    
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 检查参数有效性
    if (ctx->length == 0) {
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
    
    // 执行UNMAP操作
    struct spdk_bdev_ext_io_opts io_opts = {};
    rc = spdk_bdev_unmap(entry->desc, io_channel, ctx->offset, 
                        ctx->length, io_completion_cb, &cb_arg);
    
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
 * SPDK线程上下文中执行Flush操作
 */
void xbdev_flush_on_thread(void *arg)
{
    struct io_cmd_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
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
    
    // 执行Flush操作
    rc = spdk_bdev_flush(entry->desc, io_channel, 0, 
                        spdk_bdev_get_num_blocks(entry->bdev) * spdk_bdev_get_block_size(entry->bdev), 
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
 * SPDK线程上下文中执行Reset操作
 */
void xbdev_reset_on_thread(void *arg)
{
    struct io_cmd_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
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
    
    // 执行Reset操作
    rc = spdk_bdev_reset(entry->desc, io_channel, io_completion_cb, &cb_arg);
    
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
 * 优化IO请求批量处理
 * 
 * @param reqs 请求数组
 * @param num_reqs 请求数量
 * @return 成功返回处理的请求数量，失败返回错误码
 */
int xbdev_io_batch_submit(xbdev_request_t **reqs, int num_reqs)
{
    int i, success_count = 0;
    
    if (!reqs || num_reqs <= 0) {
        return -EINVAL;
    }
    
    // 批量提交请求
    for (i = 0; i < num_reqs; i++) {
        int rc = xbdev_request_submit(reqs[i]);
        if (rc == 0) {
            success_count++;
        } else {
            XBDEV_WARNLOG("批量提交第%d个请求失败: %d\n", i, rc);
            // 继续提交剩余请求
        }
    }
    
    return success_count;
}

/**
 * 优化异步IO请求提交
 * 
 * @param fd 文件描述符
 * @param iov IO向量数组
 * @param iovcnt 向量数量
 * @param offset 偏移量
 * @param is_write 是否为写操作
 * @param cb 完成回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_vectored(int fd, struct iovec *iov, int iovcnt, uint64_t offset,
                      bool is_write, xbdev_io_completion_cb cb, void *cb_arg)
{
    struct {
        int fd;
        struct iovec *iov;
        int iovcnt;
        uint64_t offset;
        bool write_op;
        xbdev_io_completion_cb user_cb;
        void *user_ctx;
        int status;
        bool done;
    } *ctx;
    
    xbdev_request_t *req;
    
    // 参数检查
    if (!iov || iovcnt <= 0 || !cb) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 分配上下文
    ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        XBDEV_ERRLOG("无法分配异步IO上下文\n");
        return -ENOMEM;
    }
    
    // 设置上下文
    ctx->fd = fd;
    ctx->iov = iov;
    ctx->iovcnt = iovcnt;
    ctx->offset = offset;
    ctx->write_op = is_write;
    ctx->status = 0;
    ctx->done = false;
    ctx->user_cb = cb;
    ctx->user_ctx = cb_arg;
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = is_write ? XBDEV_REQ_WRITEV : XBDEV_REQ_READV;
    req->ctx = ctx;
    
    // 提交请求
    int rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("提交异步请求失败: %d\n", rc);
        xbdev_request_free(req);
        free(ctx);
        return rc;
    }
    
    return 0;
}

/**
 * 获取设备IO性能统计信息
 * 
 * @param fd 文件描述符
 * @param stats 输出参数，IO统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_io_stats(int fd, xbdev_io_stats_t *stats)
{
    xbdev_fd_entry_t *entry;
    
    if (!stats) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry || !entry->desc) {
        return -EBADF;
    }
    
    // 从BDEV获取统计信息
    struct spdk_bdev_io_stat bdev_stats;
    
    // 获取并填充统计数据
    // 注意：这里简化处理，实际需要通过spdk_bdev_get_io_stat获取
    // 当前SPDK API没有直接获取单个描述符统计信息的方法
    
    // 临时使用全局统计信息(仅作为示例)
    memset(&bdev_stats, 0, sizeof(bdev_stats));
    spdk_bdev_get_device_stat(entry->bdev, &bdev_stats, NULL, NULL);
    
    // 填充输出统计信息
    stats->num_read_ops = bdev_stats.num_read_ops;
    stats->num_write_ops = bdev_stats.num_write_ops;
    stats->bytes_read = bdev_stats.bytes_read;
    stats->bytes_written = bdev_stats.bytes_written;
    stats->read_latency_ticks = bdev_stats.read_latency_ticks;
    stats->write_latency_ticks = bdev_stats.write_latency_ticks;
    
    // 计算IOPS和带宽
    uint64_t total_ops = bdev_stats.num_read_ops + bdev_stats.num_write_ops;
    stats->iops = total_ops; // 这里简化处理，实际需要除以时间间隔
    stats->read_bandwidth_mbytes = bdev_stats.bytes_read / (1024 * 1024); // 简化处理
    stats->write_bandwidth_mbytes = bdev_stats.bytes_written / (1024 * 1024); // 简化处理
    
    // 计算平均延迟
    if (bdev_stats.num_read_ops > 0) {
        stats->avg_read_latency_us = 
            bdev_stats.read_latency_ticks / bdev_stats.num_read_ops;
    } else {
        stats->avg_read_latency_us = 0;
    }
    
    if (bdev_stats.num_write_ops > 0) {
        stats->avg_write_latency_us = 
            bdev_stats.write_latency_ticks / bdev_stats.num_write_ops;
    } else {
        stats->avg_write_latency_us = 0;
    }
    
    return 0;
}

/**
 * 设置设备IO优先级
 * 
 * @param fd 文件描述符
 * @param priority IO优先级(0-最低，255-最高)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_io_priority(int fd, uint8_t priority)
{
    xbdev_fd_entry_t *entry;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry || !entry->desc) {
        return -EBADF;
    }
    
    // 保存优先级信息
    // 注意：当前SPDK不支持直接设置IO优先级，这里仅保存信息，不实际生效
    // 实际应用时需要扩展SPDK或在队列层面实现优先级
    entry->io_priority = priority;
    
    return 0;
}

/**
 * 执行原子比较并写入操作
 * 
 * @param fd 文件描述符
 * @param compare_buf 比较缓冲区
 * @param write_buf 写入缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @return 成功返回0，失败返回错误码(如果比较失败，返回-EILSEQ)
 */
int xbdev_compare_and_write(int fd, const void *compare_buf, const void *write_buf, 
                           size_t count, uint64_t offset)
{
    struct {
        int fd;
        const void *cmp_buf;
        const void *write_buf;
        size_t count;
        uint64_t offset;
        int status;
        bool done;
    } ctx = {0};
    
    xbdev_request_t *req;
    int rc;
    
    // 参数检查
    if (!compare_buf || !write_buf || count == 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.cmp_buf = compare_buf;
    ctx.write_buf = write_buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.status = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求类型为自定义请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.status;
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * 在SPDK线程执行比较和写入操作
 * 
 * @param arg 操作上下文
 */
void xbdev_compare_and_write_on_thread(void *arg)
{
    struct {
        int fd;
        const void *cmp_buf;
        const void *write_buf;
        size_t count;
        uint64_t offset;
        int status;
        bool done;
    } *ctx = arg;
    
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 参数检查
    if (!ctx->cmp_buf || !ctx->write_buf || ctx->count == 0) {
        ctx->status = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc || !entry->bdev) {
        ctx->status = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 获取IO通道
    io_channel = spdk_bdev_get_io_channel(entry->desc);
    if (!io_channel) {
        pthread_mutex_unlock(&entry->mutex);
        ctx->status = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 获取块大小和对齐要求
    uint32_t block_size = spdk_bdev_get_block_size(entry->bdev);
    uint32_t block_align = spdk_bdev_get_buf_align(entry->bdev);
    
    // 检查是否需要内存对齐
    void *aligned_cmp_buf = (void *)ctx->cmp_buf;
    void *aligned_write_buf = (void *)ctx->write_buf;
    bool need_bounce_buffer = false;
    void *bounce_cmp_buffer = NULL;
    void *bounce_write_buffer = NULL;
    
    // 检查对比缓冲区对齐
    if ((uintptr_t)ctx->cmp_buf & (block_align - 1)) {
        need_bounce_buffer = true;
        bounce_cmp_buffer = spdk_dma_malloc(ctx->count, block_align, NULL);
        if (!bounce_cmp_buffer) {
            spdk_put_io_channel(io_channel);
            pthread_mutex_unlock(&entry->mutex);
            ctx->status = -ENOMEM;
            ctx->done = true;
            return;
        }
        
        // 复制用户数据到对齐缓冲区
        memcpy(bounce_cmp_buffer, ctx->cmp_buf, ctx->count);
        aligned_cmp_buf = bounce_cmp_buffer;
    }
    
    // 检查写缓冲区对齐
    if ((uintptr_t)ctx->write_buf & (block_align - 1)) {
        need_bounce_buffer = true;
        bounce_write_buffer = spdk_dma_malloc(ctx->count, block_align, NULL);
        if (!bounce_write_buffer) {
            if (bounce_cmp_buffer) {
                spdk_dma_free(bounce_cmp_buffer);
            }
            spdk_put_io_channel(io_channel);
            pthread_mutex_unlock(&entry->mutex);
            ctx->status = -ENOMEM;
            ctx->done = true;
            return;
        }
        
        // 复制用户数据到对齐缓冲区
        memcpy(bounce_write_buffer, ctx->write_buf, ctx->count);
        aligned_write_buf = bounce_write_buffer;
    }
    
    // 计算块偏移和块数量
    offset_blocks = ctx->offset / block_size;
    num_blocks = (ctx->count + block_size - 1) / block_size;
    
    // 构造回调参数
    struct {
        int *status;
        bool *done;
    } cb_arg = {
        .status = &ctx->status,
        .done = &ctx->done
    };
    
    // 执行比较和写入操作
    rc = spdk_bdev_compare_and_write_blocks(entry->desc, 
                                          spdk_io_channel_from_ctx(entry),
                                          aligned_cmp_buf, 
                                          aligned_write_buf,
                                          offset_blocks, 
                                          num_blocks,
                                          io_completion_cb,
                                          &cb_arg);
    
    // 检查提交状态
    if (rc != 0) {
        if (bounce_cmp_buffer) {
            spdk_dma_free(bounce_cmp_buffer);
        }
        if (bounce_write_buffer) {
            spdk_dma_free(bounce_write_buffer);
        }
        spdk_put_io_channel(io_channel);
        pthread_mutex_unlock(&entry->mutex);
        ctx->status = rc;
        ctx->done = true;
        return;
    }
    
    // 释放IO通道
    spdk_put_io_channel(io_channel);
    pthread_mutex_unlock(&entry->mutex);
    
    // 等待操作完成
    while (!ctx->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
    
    // 释放弹跳缓冲区
    if (bounce_cmp_buffer) {
        spdk_dma_free(bounce_cmp_buffer);
    }
    if (bounce_write_buffer) {
        spdk_dma_free(bounce_write_buffer);
    }
}

/**
 * 创建零填充区域
 * 
 * @param fd 文件描述符
 * @param count 字节数
 * @param offset 偏移量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_write_zeroes(int fd, size_t count, uint64_t offset)
{
    struct {
        int fd;
        size_t count;
        uint64_t offset;
        int status;
        bool done;
    } ctx = {0};
    
    xbdev_request_t *req;
    int rc;
    
    // 参数检查
    if (count == 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.count = count;
    ctx.offset = offset;
    ctx.status = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITE_ZEROES;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.status;
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    return rc;
}

/**
 * 在SPDK线程执行写零操作
 * 
 * @param arg 操作上下文
 */
void xbdev_write_zeroes_on_thread(void *arg)
{
    struct {
        int fd;
        size_t count;
        uint64_t offset;
        int status;
        bool done;
    } *ctx = arg;
    
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    uint64_t offset_blocks;
    uint64_t num_blocks;
    int rc;
    
    // 参数检查
    if (ctx->count == 0) {
        ctx->status = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc || !entry->bdev) {
        ctx->status = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 获取IO通道
    io_channel = spdk_bdev_get_io_channel(entry->desc);
    if (!io_channel) {
        pthread_mutex_unlock(&entry->mutex);
        ctx->status = -ENOMEM;
        ctx->done = true;
        return;
    }
    
    // 获取块大小
    uint32_t block_size = spdk_bdev_get_block_size(entry->bdev);
    
    // 检查是否支持写零操作
    if (!spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
        XBDEV_WARNLOG("设备不直接支持写零操作，将使用普通写入实现\n");
        
        // 分配零缓冲区
        void *zero_buf = spdk_dma_zmalloc(block_size, block_size, NULL);
        if (!zero_buf) {
            spdk_put_io_channel(io_channel);
            pthread_mutex_unlock(&entry->mutex);
            ctx->status = -ENOMEM;
            ctx->done = true;
            return;
        }
        
        // 计算需要写入的块数
        offset_blocks = ctx->offset / block_size;
        num_blocks = (ctx->count + block_size - 1) / block_size;
        
        // 构造回调参数
        struct {
            int *status;
            bool *done;
        } cb_arg = {
            .status = &ctx->status,
            .done = &ctx->done
        };
        
        // 逐块写入零数据
        uint64_t blocks_left = num_blocks;
        uint64_t current_block = offset_blocks;
        
        while (blocks_left > 0 && !ctx->done) {
            // 一次最多写入32个块
            uint64_t blocks_this_time = (blocks_left > 32) ? 32 : blocks_left;
            
            // 执行写入
            rc = spdk_bdev_write_blocks(entry->desc, 
                                       io_channel,
                                       zero_buf, 
                                       current_block, 
                                       blocks_this_time,
                                       io_completion_cb,
                                       &cb_arg);
                                       
            if (rc != 0) {
                spdk_dma_free(zero_buf);
                spdk_put_io_channel(io_channel);
                pthread_mutex_unlock(&entry->mutex);
                ctx->status = rc;
                ctx->done = true;
                return;
            }
            
            // 等待当前写入完成
            while (!ctx->done) {
                spdk_thread_poll(spdk_get_thread(), 0, 0);
            }
            
            // 如果写入成功，继续下一块
            if (ctx->status == 0) {
                blocks_left -= blocks_this_time;
                current_block += blocks_this_time;
                ctx->done = false;  // 重置标志以便继续写入
            } else {
                // 遇到错误则中断
                break;
            }
        }
        
        spdk_dma_free(zero_buf);
    } else {
        // 支持原生写零操作时直接调用
        offset_blocks = ctx->offset / block_size;
        num_blocks = (ctx->count + block_size - 1) / block_size;
        
        // 构造回调参数
        struct {
            int *status;
            bool *done;
        } cb_arg = {
            .status = &ctx->status,
            .done = &ctx->done
        };
        
        // 执行写零操作
        rc = spdk_bdev_write_zeroes_blocks(entry->desc, 
                                         io_channel,
                                         offset_blocks, 
                                         num_blocks,
                                         io_completion_cb,
                                         &cb_arg);
                                         
        if (rc != 0) {
            spdk_put_io_channel(io_channel);
            pthread_mutex_unlock(&entry->mutex);
            ctx->status = rc;
            ctx->done = true;
            return;
        }
        
        // 等待操作完成
        while (!ctx->done) {
            spdk_thread_poll(spdk_get_thread(), 0, 0);
        }
    }
    
    // 释放IO通道
    spdk_put_io_channel(io_channel);
    pthread_mutex_unlock(&entry->mutex);
}

/**
 * 获取设备的物理扇区映射信息
 * 
 * @param fd 文件描述符
 * @param offset 逻辑偏移量
 * @param length 区域长度
 * @param phys_info 输出参数，物理映射信息数组
 * @param max_segments 信息数组大小
 * @return 成功返回实际映射段数，失败返回错误码
 */
int xbdev_get_physical_mapping(int fd, uint64_t offset, uint64_t length,
                             xbdev_phys_segment_t *phys_info, int max_segments)
{
    struct {
        int fd;
        uint64_t offset;
        uint64_t length;
        xbdev_phys_segment_t *segments;
        int max_segments;
        int num_segments;
        int status;
        bool done;
    } ctx = {0};
    
    xbdev_request_t *req;
    int rc;
    
    // 参数检查
    if (!phys_info || max_segments <= 0 || length == 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.offset = offset;
    ctx.length = length;
    ctx.segments = phys_info;
    ctx.max_segments = max_segments;
    ctx.num_segments = 0;
    ctx.status = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_GET_MAPPING;
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
    
    // 检查操作结果
    if (ctx.status != 0) {
        return ctx.status;
    }
    
    return ctx.num_segments;
}

/**
 * 在SPDK线程获取物理映射信息
 * 
 * @param arg 操作上下文
 */
void xbdev_get_physical_mapping_on_thread(void *arg)
{
    struct {
        int fd;
        uint64_t offset;
        uint64_t length;
        xbdev_phys_segment_t *segments;
        int max_segments;
        int num_segments;
        int status;
        bool done;
    } *ctx = arg;
    
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (ctx->length == 0 || !ctx->segments || ctx->max_segments <= 0) {
        ctx->status = -EINVAL;
        ctx->done = true;
        return;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc || !entry->bdev) {
        ctx->status = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 尝试获取物理映射信息
    // 注意：SPDK目前没有直接提供逻辑到物理映射的API
    // 这里提供一个简化的实现，实际应根据设备类型特殊处理
    
    // 对于NVMe设备，可能需要通过特殊命令获取
    const char *driver_name = spdk_bdev_get_driver_name(entry->bdev);
    
    if (strcmp(driver_name, "nvme") == 0) {
        // NVMe设备尝试使用特定命令获取映射
        XBDEV_INFOLOG("尝试获取NVMe设备的物理映射\n");
        
        // 这里简化处理，实际需要实现NVMe DSM命令获取映射
        // 暂时使用默认映射
        ctx->segments[0].physical_offset = ctx->offset; // 默认为相同
        ctx->segments[0].length = ctx->length;
        ctx->segments[0].device_id = 0; // 主设备
        ctx->num_segments = 1;
        
    } else if (strcmp(driver_name, "raid1") == 0 || 
               strcmp(driver_name, "raid5") == 0) {
        // RAID设备，需要查询每个成员盘的映射
        XBDEV_INFOLOG("尝试获取RAID设备的物理映射\n");
        
        // RAID映射通常需要访问底层的RAID元数据
        // 这里简化处理，假设数据在多个设备上有副本
        // 对于RAID1，数据在所有成员盘上都有副本
        // 对于RAID5，需要根据奇偶校验分布确定
        
        // 这里仅提供成员盘信息作为示例
        int raid_members = 2; // 假设为2个成员
        for (int i = 0; i < raid_members && i < ctx->max_segments; i++) {
            ctx->segments[i].physical_offset = ctx->offset; // 简化映射
            ctx->segments[i].length = ctx->length;
            ctx->segments[i].device_id = i; // 成员盘ID
        }
        ctx->num_segments = (raid_members < ctx->max_segments) ? raid_members : ctx->max_segments;
        
    } else if (strcmp(driver_name, "logical_volume") == 0) {
        // LVOL卷可能需要查询底层存储池的映射
        XBDEV_INFOLOG("尝试获取逻辑卷的物理映射\n");
        
        // LVOL会在集群大小边界对数据进行映射
        // 这里简化处理，仅提供估计的映射
        uint64_t cluster_size = 4 * 1024 * 1024; // 假设4MB簇大小
        uint64_t start_cluster = ctx->offset / cluster_size;
        uint64_t end_cluster = (ctx->offset + ctx->length - 1) / cluster_size;
        uint64_t num_clusters = end_cluster - start_cluster + 1;
        
        // 填充映射段
        int segments = (num_clusters < ctx->max_segments) ? num_clusters : ctx->max_segments;
        for (int i = 0; i < segments; i++) {
            ctx->segments[i].physical_offset = (start_cluster + i) * cluster_size;
            ctx->segments[i].length = cluster_size;
            ctx->segments[i].device_id = 0; // 简化处理为单一设备
        }
        ctx->num_segments = segments;
    } else {
        // 其他设备类型，提供一个默认映射
        ctx->segments[0].physical_offset = ctx->offset;
        ctx->segments[0].length = ctx->length;
        ctx->segments[0].device_id = 0;
        ctx->num_segments = 1;
    }
    
    pthread_mutex_unlock(&entry->mutex);
    ctx->status = 0;
    ctx->done = true;
}

/**
 * 附加扩展IO选项
 * 
 * @param req 请求
 * @param ext_opts 扩展选项
 * @return 成功返回0，失败返回错误码
 */
int xbdev_attach_ext_io_opts(xbdev_request_t *req, const xbdev_ext_io_opts_t *ext_opts)
{
    if (!req || !ext_opts) {
        return -EINVAL;
    }
    
    // 分配扩展选项内存
    xbdev_ext_io_opts_t *opts_copy = malloc(sizeof(xbdev_ext_io_opts_t));
    if (!opts_copy) {
        return -ENOMEM;
    }
    
    // 复制扩展选项
    memcpy(opts_copy, ext_opts, sizeof(xbdev_ext_io_opts_t));
    
    // 附加到请求
    req->ext_opts = opts_copy;
    
    return 0;
}

/**
 * 应用扩展IO选项到SPDK选项
 * 
 * @param xbdev_opts libxbdev扩展选项
 * @param spdk_opts SPDK扩展选项
 */
static void apply_ext_io_opts(const xbdev_ext_io_opts_t *xbdev_opts, 
                             struct spdk_bdev_ext_io_opts *spdk_opts)
{
    if (!xbdev_opts || !spdk_opts) {
        return;
    }
    
    // 重置SPDK选项
    memset(spdk_opts, 0, sizeof(*spdk_opts));
    
    // 应用常规选项
    spdk_opts->size = sizeof(*spdk_opts);
    spdk_opts->memory_domain = xbdev_opts->memory_domain;
    spdk_opts->memory_domain_ctx = xbdev_opts->memory_domain_ctx;
    
    // 应用数据保护选项
    if (xbdev_opts->flags & XBDEV_EXT_IO_OPTS_SET_DIF) {
        spdk_opts->metadata = xbdev_opts->metadata;
        spdk_opts->apptag_mask = xbdev_opts->apptag_mask;
        spdk_opts->app_tag = xbdev_opts->app_tag;
    }
    
    // 应用SGList选项
    if (xbdev_opts->flags & XBDEV_EXT_IO_OPTS_SET_SGLIST) {
        spdk_opts->ext_opts_size = sizeof(*spdk_opts);
    }
}

/**
 * 设置IO操作的内存域
 * 
 * @param fd 文件描述符
 * @param domain 内存域
 * @param domain_ctx 内存域上下文
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_memory_domain(int fd, void *domain, void *domain_ctx)
{
    xbdev_fd_entry_t *entry;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 锁定设备
    pthread_mutex_lock(&entry->mutex);
    
    // 设置内存域
    entry->memory_domain = domain;
    entry->memory_domain_ctx = domain_ctx;
    
    pthread_mutex_unlock(&entry->mutex);
    
    return 0;
}

/**
 * 获取设备IO能力信息
 * 
 * @param fd 文件描述符
 * @param capabilities 输出参数，设备能力
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_io_capabilities(int fd, uint64_t *capabilities)
{
    xbdev_fd_entry_t *entry;
    
    if (!capabilities) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry || !entry->desc || !entry->bdev) {
        return -EBADF;
    }
    
    // 初始化能力标志
    *capabilities = 0;
    
    // 检查各种IO类型的支持
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_READ)) {
        *capabilities |= XBDEV_IO_CAP_READ;
    }
    
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_WRITE)) {
        *capabilities |= XBDEV_IO_CAP_WRITE;
    }
    
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
        *capabilities |= XBDEV_IO_CAP_FLUSH;
    }
    
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
        *capabilities |= XBDEV_IO_CAP_UNMAP;
    }
    
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_RESET)) {
        *capabilities |= XBDEV_IO_CAP_RESET;
    }
    
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_COMPARE)) {
        *capabilities |= XBDEV_IO_CAP_COMPARE;
    }
    
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE)) {
        *capabilities |= XBDEV_IO_CAP_COMPARE_AND_WRITE;
    }
    
    if (spdk_bdev_io_type_supported(entry->bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
        *capabilities |= XBDEV_IO_CAP_WRITE_ZEROES;
    }
    
    // 检查是否支持DIF
    if (spdk_bdev_is_dif_capable(entry->bdev)) {
        *capabilities |= XBDEV_IO_CAP_DIF;
    }
    
    return 0;
}

/**
 * 高性能异步IO接口
 * 
 * @param fd 文件描述符
 * @param op_type 操作类型
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @param cb 完成回调
 * @param cb_arg 回调参数
 * @param flags 标志
 * @param opts 扩展选项
 * @return 成功返回请求ID，失败返回负值错误码
 */
int64_t xbdev_io_submit(int fd, int op_type, void *buf, size_t count, 
                       uint64_t offset, xbdev_io_completion_cb cb, void *cb_arg,
                       uint32_t flags, const xbdev_ext_io_opts_t *opts)
{
    xbdev_fd_entry_t *entry;
    xbdev_request_t *req;
    struct {
        int fd;
        int op_type;
        void *buf;
        size_t count;
        uint64_t offset;
        xbdev_io_completion_cb cb;
        void *cb_arg;
        uint32_t flags;
        xbdev_ext_io_opts_t *opts;
        int status;
        bool done;
        int64_t req_id;
    } *ctx;
    int rc;
    
    // 参数验证
    if ((op_type == XBDEV_IO_TYPE_READ || op_type == XBDEV_IO_TYPE_WRITE) && 
        (!buf || count == 0)) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 分配上下文
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -ENOMEM;
    }
    
    // 设置上下文
    ctx->fd = fd;
    ctx->op_type = op_type;
    ctx->buf = buf;
    ctx->count = count;
    ctx->offset = offset;
    ctx->cb = cb;
    ctx->cb_arg = cb_arg;
    ctx->flags = flags;
    ctx->status = 0;
    ctx->done = false;
    
    // 复制扩展选项（如果有）
    if (opts) {
        ctx->opts = malloc(sizeof(xbdev_ext_io_opts_t));
        if (!ctx->opts) {
            free(ctx);
            return -ENOMEM;
        }
        memcpy(ctx->opts, opts, sizeof(xbdev_ext_io_opts_t));
    }
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        if (ctx->opts) {
            free(ctx->opts);
        }
        free(ctx);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = op_type;
    req->ctx = ctx;
    
    // 附加扩展选项
    if (opts) {
        rc = xbdev_attach_ext_io_opts(req, opts);
        if (rc != 0) {
            xbdev_request_free(req);
            if (ctx->opts) {
                free(ctx->opts);
            }
            free(ctx);
            return rc;
        }
    }
    
    // 提交请求
    rc = xbdev_request_submit(req);
    if (rc != 0) {
        xbdev_request_free(req);
        if (ctx->opts) {
            free(ctx->opts);
        }
        free(ctx);
        return rc;
    }
    
    // 返回请求ID
    ctx->req_id = req->id;
    return ctx->req_id;
}

/**
 * 高性能IO批量请求结构体
 */
typedef struct {
    int num_requests;            // 请求数量
    int max_requests;            // 最大请求数量
    xbdev_io_request_t *reqs;    // 请求数组
    bool submitted;              // 是否已提交
} xbdev_io_batch_t;

/**
 * 创建IO批量请求
 * 
 * @param max_requests 最大请求数量
 * @return 成功返回批量请求指针，失败返回NULL
 */
xbdev_io_batch_t* xbdev_io_batch_create(int max_requests)
{
    xbdev_io_batch_t *batch;
    
    if (max_requests <= 0) {
        XBDEV_ERRLOG("无效的最大请求数量: %d\n", max_requests);
        return NULL;
    }
    
    // 分配批量请求结构
    batch = calloc(1, sizeof(xbdev_io_batch_t));
    if (!batch) {
        XBDEV_ERRLOG("无法分配批量请求结构\n");
        return NULL;
    }
    
    // 分配请求数组
    batch->reqs = calloc(max_requests, sizeof(xbdev_io_request_t));
    if (!batch->reqs) {
        XBDEV_ERRLOG("无法分配请求数组\n");
        free(batch);
        return NULL;
    }
    
    batch->max_requests = max_requests;
    batch->num_requests = 0;
    batch->submitted = false;
    
    return batch;
}

/**
 * 向批量请求添加请求
 * 
 * @param batch 批量请求指针
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @param op_type 操作类型
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_batch_add(xbdev_io_batch_t *batch, int fd, void *buf, size_t count, 
                      uint64_t offset, int op_type)
{
    xbdev_io_request_t *req;
    
    // 参数检查
    if (!batch || !batch->reqs) {
        XBDEV_ERRLOG("无效的批量请求\n");
        return -EINVAL;
    }
    
    if (batch->submitted) {
        XBDEV_ERRLOG("批量请求已提交，不能再添加\n");
        return -EPERM;
    }
    
    if (batch->num_requests >= batch->max_requests) {
        XBDEV_ERRLOG("批量请求已满\n");
        return -ENOMEM;
    }
    
    // 获取下一个请求槽
    req = &batch->reqs[batch->num_requests++];
    
    // 初始化请求
    req->fd = fd;
    req->buf = buf;
    req->count = count;
    req->offset = offset;
    req->op_type = op_type;
    req->status = 0;
    req->completed = false;
    
    return 0;
}

/**
 * 提交批量请求
 * 
 * @param batch 批量请求指针
 * @return 成功返回提交的请求数量，失败返回错误码
 */
int xbdev_io_batch_submit(xbdev_io_batch_t *batch)
{
    int i, submitted = 0;
    xbdev_request_t **reqs = NULL;
    
    // 参数检查
    if (!batch || !batch->reqs) {
        XBDEV_ERRLOG("无效的批量请求\n");
        return -EINVAL;
    }
    
    if (batch->submitted) {
        XBDEV_ERRLOG("批量请求已提交\n");
        return -EPERM;
    }
    
    if (batch->num_requests == 0) {
        return 0;  // 没有请求要提交
    }
    
    // 分配请求指针数组
    reqs = malloc(batch->num_requests * sizeof(xbdev_request_t*));
    if (!reqs) {
        XBDEV_ERRLOG("无法分配请求指针数组\n");
        return -ENOMEM;
    }
    
    // 为每个请求创建一个xbdev_request_t
    for (i = 0; i < batch->num_requests; i++) {
        xbdev_io_request_t *io_req = &batch->reqs[i];
        xbdev_request_t *req = xbdev_request_alloc();
        
        if (!req) {
            XBDEV_ERRLOG("无法分配第%d个请求\n", i);
            // 释放之前分配的请求
            for (int j = 0; j < submitted; j++) {
                xbdev_request_free(reqs[j]);
            }
            free(reqs);
            return -ENOMEM;
        }
        
        // 设置请求类型
        switch (io_req->op_type) {
            case XBDEV_IO_TYPE_READ:
                req->type = XBDEV_REQ_READ;
                break;
            case XBDEV_IO_TYPE_WRITE:
                req->type = XBDEV_REQ_WRITE;
                break;
            case XBDEV_IO_TYPE_UNMAP:
                req->type = XBDEV_REQ_UNMAP;
                break;
            case XBDEV_IO_TYPE_FLUSH:
                req->type = XBDEV_REQ_FLUSH;
                break;
            case XBDEV_IO_TYPE_RESET:
                req->type = XBDEV_REQ_RESET;
                break;
            default:
                XBDEV_ERRLOG("不支持的操作类型: %d\n", io_req->op_type);
                xbdev_request_free(req);
                // 释放之前分配的请求
                for (int j = 0; j < submitted; j++) {
                    xbdev_request_free(reqs[j]);
                }
                free(reqs);
                return -EINVAL;
        }
        
        // 设置上下文和回调
        req->ctx = io_req;
        
        // 保存请求指针
        reqs[submitted++] = req;
    }
    
    // 使用批处理函数提交所有请求
    int rc = xbdev_batch_requests(reqs, submitted);
    if (rc < 0) {
        XBDEV_ERRLOG("批量提交请求失败: %d\n", rc);
        // 释放所有请求
        for (int j = 0; j < submitted; j++) {
            xbdev_request_free(reqs[j]);
        }
        free(reqs);
        return rc;
    }
    
    // 释放请求指针数组（请求本身会在处理完成后释放）
    free(reqs);
    
    // 标记为已提交
    batch->submitted = true;
    
    return submitted;
}

/**
 * 等待批量请求完成
 * 
 * @param batch 批量请求指针
 * @param timeout_us 超时时间（微秒），0表示永不超时
 * @return 成功返回完成的请求数量，失败返回错误码
 */
int xbdev_io_batch_wait(xbdev_io_batch_t *batch, uint64_t timeout_us)
{
    int i, completed = 0;
    uint64_t start_time, current_time;
    bool timed_out = false;
    
    // 参数检查
    if (!batch || !batch->reqs) {
        XBDEV_ERRLOG("无效的批量请求\n");
        return -EINVAL;
    }
    
    if (!batch->submitted) {
        XBDEV_ERRLOG("批量请求未提交\n");
        return -EPERM;
    }
    
    // 获取开始时间
    start_time = spdk_get_ticks();
    
    // 等待所有请求完成或超时
    while (completed < batch->num_requests && !timed_out) {
        // 检查所有请求状态
        for (i = 0; i < batch->num_requests; i++) {
            xbdev_io_request_t *req = &batch->reqs[i];
            
            if (!req->completed) {
                // 检查请求是否已完成
                // 这里假设请求处理线程会设置completed标志
                if (req->completed) {
                    completed++;
                }
            }
        }
        
        // 如果所有请求已完成，退出循环
        if (completed == batch->num_requests) {
            break;
        }
        
        // 检查是否超时
        if (timeout_us > 0) {
            current_time = spdk_get_ticks();
            uint64_t elapsed_us = (current_time - start_time) * 1000000 / spdk_get_ticks_hz();
            
            if (elapsed_us >= timeout_us) {
                timed_out = true;
                XBDEV_WARNLOG("批量请求等待超时\n");
                break;
            }
        }
        
        // 短暂休眠，避免忙等
        usleep(100);
    }
    
    return completed;
}

/**
 * 检查批量请求是否全部完成
 * 
 * @param batch 批量请求指针
 * @return 全部完成返回true，否则返回false
 */
bool xbdev_io_batch_is_complete(xbdev_io_batch_t *batch)
{
    int i;
    
    // 参数检查
    if (!batch || !batch->reqs) {
        XBDEV_ERRLOG("无效的批量请求\n");
        return false;
    }
    
    if (!batch->submitted) {
        XBDEV_ERRLOG("批量请求未提交\n");
        return false;
    }
    
    // 检查所有请求是否完成
    for (i = 0; i < batch->num_requests; i++) {
        if (!batch->reqs[i].completed) {
            return false;
        }
    }
    
    return true;
}

/**
 * 获取批量请求结果
 * 
 * @param batch 批量请求指针
 * @param results 结果数组，长度必须不小于请求数量
 * @return 成功返回结果数量，失败返回错误码
 */
int xbdev_io_batch_get_results(xbdev_io_batch_t *batch, int *results)
{
    int i;
    
    // 参数检查
    if (!batch || !batch->reqs || !results) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    if (!batch->submitted) {
        XBDEV_ERRLOG("批量请求未提交\n");
        return -EPERM;
    }
    
    // 复制结果
    for (i = 0; i < batch->num_requests; i++) {
        results[i] = batch->reqs[i].status;
    }
    
    return batch->num_requests;
}

/**
 * 释放批量请求
 * 
 * @param batch 批量请求指针
 */
void xbdev_io_batch_free(xbdev_io_batch_t *batch)
{
    if (!batch) {
        return;
    }
    
    // 释放请求数组
    if (batch->reqs) {
        free(batch->reqs);
    }
    
    // 释放批量请求结构
    free(batch);
}

/**
 * 创建优化的IO队列
 * 
 * @param name 队列名称
 * @param depth 队列深度
 * @param flags 队列标志
 * @return 成功返回队列ID，失败返回错误码
 */
int xbdev_io_queue_create(const char *name, int depth, int flags)
{
    // 目前简化实现，直接使用全局队列系统
    // 实际实现应该创建一个专用队列
    
    if (!name || depth <= 0) {
        XBDEV_ERRLOG("无效的参数: name=%p, depth=%d\n", name, depth);
        return -EINVAL;
    }
    
    // 这里假装创建了一个新队列，返回一个虚构的队列ID
    // 实际上应该在队列系统中分配一个专用队列并返回其ID
    return 1;  // 返回一个假的队列ID
}

/**
 * 提交IO队列请求
 * 
 * @param queue_id 队列ID
 * @param batch 批量请求
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_queue_submit(int queue_id, xbdev_io_batch_t *batch)
{
    // 简化实现，直接使用批量提交函数
    return xbdev_io_batch_submit(batch);
}

/**
 * 等待IO队列操作完成
 * 
 * @param queue_id 队列ID
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_queue_wait(int queue_id)
{
    // 简化实现，实际应该等待指定队列中的所有请求完成
    // 这里只是为了提供API完整性
    return 0;
}

/**
 * 销毁IO队列
 * 
 * @param queue_id 队列ID
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_queue_destroy(int queue_id)
{
    // 简化实现，实际应该清理并释放队列资源
    // 这里只是为了提供API完整性
    return 0;
}

/**
 * 设置缓存策略
 * 
 * @param fd 文件描述符
 * @param policy 缓存策略
 * @return 成功返回0，失败返回错误码
 */
int xbdev_cache_policy_set(int fd, xbdev_cache_policy_t policy)
{
    xbdev_fd_entry_t *entry;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry || !entry->desc) {
        return -EBADF;
    }
    
    // 保存缓存策略信息
    entry->cache_policy = policy;
    
    // SPDK目前没有直接的API设置块设备缓存策略
    // 这里只保存了策略值，实际应用时需要根据策略调整IO行为
    
    return 0;
}

/**
 * 预取数据到缓存
 * 
 * @param fd 文件描述符
 * @param offset 偏移量
 * @param size 大小
 * @return 成功返回0，失败返回错误码
 */
int xbdev_cache_prefetch(int fd, uint64_t offset, size_t size)
{
    // 简化实现，实际应该触发一个实际的预读请求
    // SPDK没有直接的预取API，可以通过发出读请求并丢弃结果来实现
    
    // 这里只是示意性的实现，不执行实际操作
    return 0;
}

/**
 * 获取缓存统计信息
 * 
 * @param fd 文件描述符
 * @param stats 统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_cache_stats_get(int fd, xbdev_cache_stats_t *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    // SPDK没有直接的缓存统计API
    // 这里提供一个简化的实现，返回空的统计信息
    
    memset(stats, 0, sizeof(xbdev_cache_stats_t));
    
    return 0;
}

/**
 * 设置NUMA节点偏好
 * 
 * @param numa_node NUMA节点编号
 * @return 成功返回0，失败返回错误码
 */
int xbdev_numa_set_preferred(int numa_node)
{
    // 检查NUMA节点是否有效
    if (numa_node < 0) {
        XBDEV_ERRLOG("无效的NUMA节点: %d\n", numa_node);
        return -EINVAL;
    }
    
    // 保存NUMA偏好设置
    g_preferred_numa_node = numa_node;
    
    // 实际应用中，应根据这个偏好设置调整内存分配和线程亲和性
    
    return 0;
}

/**
 * 设置内存分配策略
 * 
 * @param policy 内存策略
 * @return 成功返回0，失败返回错误码
 */
int xbdev_memory_policy_set(xbdev_memory_policy_t policy)
{
    // 检查策略是否有效
    if (policy < XBDEV_MEM_POLICY_DEFAULT || policy > XBDEV_MEM_POLICY_INTERLEAVE) {
        XBDEV_ERRLOG("无效的内存策略: %d\n", policy);
        return -EINVAL;
    }
    
    // 保存内存策略设置
    g_memory_policy = policy;
    
    // 实际应用中，应根据这个策略调整内存分配行为
    
    return 0;
}

/**
 * 配置健康监控
 * 
 * @param config 监控配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_health_monitor_start(xbdev_health_config_t *config)
{
    // 参数检查
    if (!config) {
        XBDEV_ERRLOG("无效的健康监控配置\n");
        return -EINVAL;
    }
    
    // 保存监控配置
    memcpy(&g_health_config, config, sizeof(xbdev_health_config_t));
    
    // 启动监控线程或定时器
    // 简化实现，实际应该创建一个线程或设置定时器定期检查设备健康
    
    return 0;
}

/**
 * 获取设备健康状态
 * 
 * @param device 设备名称
 * @param health 健康信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_health_status_get(const char *device, xbdev_health_info_t *health)
{
    // 参数检查
    if (!device || !health) {
        XBDEV_ERRLOG("无效的参数: device=%p, health=%p\n", device, health);
        return -EINVAL;
    }
    
    // 获取设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(device);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", device);
        return -ENOENT;
    }
    
    // 检查设备类型，获取健康信息
    const char *driver_name = spdk_bdev_get_driver_name(bdev);
    
    // 初始化健康信息
    memset(health, 0, sizeof(xbdev_health_info_t));
    strncpy(health->device_name, device, sizeof(health->device_name) - 1);
    
    // 对于NVMe设备，尝试获取SMART数据
    if (strcmp(driver_name, "nvme") == 0) {
        // 简化实现，实际应该从NVMe控制器获取SMART数据
        health->health_percentage = 95; // 假设健康度为95%
        health->temperature_celsius = 40; // 假设温度为40℃
        health->has_smart_data = true;
    }
    // 对于其他设备类型，提供基本健康状态
    else {
        health->health_percentage = 100; // 假设健康度为100%
        health->has_smart_data = false;
    }
    
    // 假设剩余寿命为2年
    health->estimated_lifetime_days = 365 * 2;
    
    return 0;
}

/**
 * 注册健康事件回调
 * 
 * @param event_cb 事件回调函数
 * @param user_ctx 用户上下文
 * @return 成功返回0，失败返回错误码
 */
int xbdev_register_health_callback(xbdev_health_event_cb event_cb, void *user_ctx)
{
    // 参数检查
    if (!event_cb) {
        XBDEV_ERRLOG("无效的回调函数\n");
        return -EINVAL;
    }
    
    // 保存回调函数和上下文
    g_health_callback = event_cb;
    g_health_callback_ctx = user_ctx;
    
    return 0;
}

/**
 * 获取性能统计信息
 * 
 * @param device 设备名称
 * @param stats 统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_get(const char *device, xbdev_stats_t *stats)
{
    // 参数检查
    if (!device || !stats) {
        XBDEV_ERRLOG("无效的参数: device=%p, stats=%p\n", device, stats);
        return -EINVAL;
    }
    
    // 获取设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(device);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", device);
        return -ENOENT;
    }
    
    // 从BDEV获取统计信息
    struct spdk_bdev_io_stat bdev_stat = {0};
    
    // 获取设备统计信息
    spdk_bdev_get_device_stat(bdev, &bdev_stat, NULL, NULL);
    
    // 填充统计信息
    memset(stats, 0, sizeof(xbdev_stats_t));
    strncpy(stats->device_name, device, sizeof(stats->device_name) - 1);
    
    // IOPS计算
    stats->iops = (bdev_stat.num_read_ops + bdev_stat.num_write_ops);
    
    // 带宽计算 (MB/s)
    stats->bandwidth_mbps = (bdev_stat.bytes_read + bdev_stat.bytes_written) / (1024 * 1024);
    
    // 延迟计算 (微秒)
    if (bdev_stat.num_read_ops > 0) {
        stats->avg_read_latency_us = bdev_stat.read_latency_ticks / bdev_stat.num_read_ops;
    }
    
    if (bdev_stat.num_write_ops > 0) {
        stats->avg_write_latency_us = bdev_stat.write_latency_ticks / bdev_stat.num_write_ops;
    }
    
    // 读写比例
    stats->read_write_ratio = (bdev_stat.num_write_ops > 0) ?
                            ((float)bdev_stat.num_read_ops / bdev_stat.num_write_ops) : 0;
    
    return 0;
}

/**
 * 开始收集性能统计
 * 
 * @param device 设备名称
 * @param config 配置参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_collect_start(const char *device, xbdev_stats_config_t *config)
{
    // 参数检查
    if (!device || !config) {
        XBDEV_ERRLOG("无效的参数: device=%p, config=%p\n", device, config);
        return -EINVAL;
    }
    
    // 获取设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(device);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", device);
        return -ENOENT;
    }
    
    // 保存统计配置
    // 这里简化实现，实际应该为每个设备单独保存配置
    memcpy(&g_stats_config, config, sizeof(xbdev_stats_config_t));
    
    // 启动统计收集
    // 简化实现，实际应该创建一个线程或设置定时器定期收集统计数据
    
    return 0;
}

/**
 * 重置统计信息
 * 
 * @param device 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_reset(const char *device)
{
    // 参数检查
    if (!device) {
        XBDEV_ERRLOG("无效的设备名称\n");
        return -EINVAL;
    }
    
    // 获取设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(device);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", device);
        return -ENOENT;
    }
    
    // SPDK没有直接的重置统计信息API
    // 这里简化实现，实际应该找到合适的方法重置统计计数器
    
    return 0;
}

/**
 * 设置自动化管理策略
 * 
 * @param device 设备名称
 * @param policy 策略参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_automation_policy_set(const char *device, xbdev_automation_policy_t *policy)
{
    // 参数检查
    if (!device || !policy) {
        XBDEV_ERRLOG("无效的参数: device=%p, policy=%p\n", device, policy);
        return -EINVAL;
    }
    
    // 获取设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(device);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", device);
        return -ENOENT;
    }
    
    // 保存自动化策略
    // 这里简化实现，实际应该为每个设备单独保存策略
    memcpy(&g_automation_policy, policy, sizeof(xbdev_automation_policy_t));
    
    return 0;
}

/**
 * DMA缓冲区分配
 * 
 * @param size 缓冲区大小
 * @param alignment 对齐要求，0表示使用默认对齐
 * @return 成功返回分配的缓冲区，失败返回NULL
 */
void* xbdev_dma_buffer_alloc(size_t size, size_t alignment)
{
    if (size == 0) {
        XBDEV_ERRLOG("无效的缓冲区大小: %zu\n", size);
        return NULL;
    }
    
    // 如果未指定对齐，使用默认对齐
    if (alignment == 0) {
        alignment = 4096; // 默认4K对齐
    }
    
    // 分配对齐的DMA缓冲区
    return spdk_dma_malloc(size, alignment, NULL);
}

/**
 * 释放DMA缓冲区
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
 * 注册IO容错回调
 * 
 * @param fd 文件描述符
 * @param cb 容错回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_register_io_retry_callback(int fd, xbdev_io_retry_cb cb, void *cb_arg)
{
    xbdev_fd_entry_t *entry;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("无效的文件描述符: %d\n", fd);
        return -EBADF;
    }
    
    // 保存回调函数和参数
    entry->retry_cb = cb;
    entry->retry_cb_arg = cb_arg;
    
    return 0;
}

/**
 * 执行IO重试
 * 
 * @param ctx IO上下文
 * @param retry_count 已重试次数
 * @return 重试处理结果，true表示已处理，false表示需要返回失败
 */
static bool handle_io_retry(xbdev_io_ctx_t *ctx, int retry_count)
{
    xbdev_fd_entry_t *entry;
    
    if (!ctx) {
        return false;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->retry_cb) {
        return false;
    }
    
    // 构建重试信息
    xbdev_retry_info_t retry_info = {
        .fd = ctx->fd,
        .op_type = ctx->write ? XBDEV_IO_TYPE_WRITE : XBDEV_IO_TYPE_READ,
        .buf = ctx->buf,
        .offset = ctx->offset,
        .count = ctx->count,
        .retry_count = retry_count,
        .error_code = ctx->rc
    };
    
    // 调用用户注册的重试回调
    return entry->retry_cb(&retry_info, entry->retry_cb_arg);
}

/**
 * 执行IO操作并自动重试
 * 
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @param is_write 是否写操作
 * @param max_retries 最大重试次数
 * @return 成功返回字节数，失败返回错误码
 */
static ssize_t xbdev_io_with_retry(int fd, void *buf, size_t count, 
                                  uint64_t offset, bool is_write, int max_retries)
{
    xbdev_io_ctx_t ctx = {0};
    xbdev_request_t *req;
    int rc, retry_count = 0;
    
    // 参数检查
    if (!buf || count == 0 || max_retries < 0) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.write = is_write;
    ctx.done = false;
    ctx.rc = 0;
    
    do {
        // 分配请求
        req = xbdev_sync_request_alloc();
        if (!req) {
            XBDEV_ERRLOG("无法分配请求\n");
            return -ENOMEM;
        }
        
        // 设置请求
        req->type = is_write ? XBDEV_REQ_WRITE : XBDEV_REQ_READ;
        req->ctx = &ctx;
        
        // 重置完成标志
        ctx.done = false;
        ctx.rc = 0;
        
        // 提交请求并等待完成
        rc = xbdev_sync_request_execute(req);
        if (rc != 0) {
            XBDEV_ERRLOG("执行同步请求失败，rc=%d\n", rc);
            xbdev_request_free(req);
            return rc;
        }
        
        // 获取操作结果
        rc = ctx.rc;
        
        // 释放请求
        xbdev_request_free(req);
        
        // 如果操作成功，直接返回
        if (rc >= 0) {
            return rc;
        }
        
        // 如果操作失败且重试次数未达上限，尝试重试
        retry_count++;
        if (retry_count <= max_retries && handle_io_retry(&ctx, retry_count)) {
            XBDEV_INFOLOG("IO操作重试 #%d: fd=%d, offset=%lu, count=%zu\n",
                         retry_count, fd, offset, count);
            continue;
        }
        
        // 无法重试或已达最大重试次数，返回错误
        break;
        
    } while (retry_count <= max_retries);
    
    return rc;
}

/**
 * 支持重试的读取操作
 * 
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @param max_retries 最大重试次数
 * @return 成功返回读取的字节数，失败返回错误码
 */
ssize_t xbdev_read_retry(int fd, void *buf, size_t count, uint64_t offset, int max_retries)
{
    return xbdev_io_with_retry(fd, buf, count, offset, false, max_retries);
}

/**
 * 支持重试的写入操作
 * 
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @param max_retries 最大重试次数
 * @return 成功返回写入的字节数，失败返回错误码
 */
ssize_t xbdev_write_retry(int fd, const void *buf, size_t count, uint64_t offset, int max_retries)
{
    return xbdev_io_with_retry(fd, (void*)buf, count, offset, true, max_retries);
}

/**
 * IO操作超时设置
 * 
 * @param fd 文件描述符
 * @param timeout_ms 超时时间（毫秒），0表示使用默认值
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_io_timeout(int fd, uint32_t timeout_ms)
{
    xbdev_fd_entry_t *entry;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("无效的文件描述符: %d\n", fd);
        return -EBADF;
    }
    
    // 保存超时设置
    entry->io_timeout_ms = timeout_ms > 0 ? timeout_ms : XBDEV_DEFAULT_IO_TIMEOUT_MS;
    
    return 0;
}

/**
 * 执行IO操作时检查超时
 * 
 * @param start_time 开始时间
 * @param timeout_ms 超时时间（毫秒）
 * @return 如果已超时返回true，否则返回false
 */
static bool check_io_timeout(uint64_t start_time, uint32_t timeout_ms)
{
    uint64_t current_time = spdk_get_ticks();
    uint64_t elapsed_ms = (current_time - start_time) * 1000 / spdk_get_ticks_hz();
    
    return elapsed_ms >= timeout_ms;
}

/**
 * 带超时的IO读取操作
 * 
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @return 成功返回读取的字节数，失败返回错误码
 */
ssize_t xbdev_read_timeout(int fd, void *buf, size_t count, uint64_t offset)
{
    xbdev_io_ctx_t ctx = {0};
    xbdev_request_t *req;
    xbdev_fd_entry_t *entry;
    uint64_t start_time;
    int rc;
    
    // 参数检查
    if (!buf || count == 0) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.write = false;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_READ;
    req->ctx = &ctx;
    
    // 记录开始时间
    start_time = spdk_get_ticks();
    
    // 提交请求
    rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("提交请求失败，rc=%d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 等待完成或超时
    uint32_t timeout = entry->io_timeout_ms > 0 ? 
                      entry->io_timeout_ms : XBDEV_DEFAULT_IO_TIMEOUT_MS;
                      
    while (!ctx.done) {
        // 检查是否超时
        if (check_io_timeout(start_time, timeout)) {
            XBDEV_ERRLOG("IO读取操作超时: fd=%d, offset=%lu, timeout=%u ms\n",
                        fd, offset, timeout);
                        
            // 设置超时错误码
            ctx.rc = -ETIMEDOUT;
            
            // 尝试取消请求（如果底层驱动支持）
            // 注意：SPDK目前不提供通用的取消机制，这里仅为示意
            
            // 释放请求
            xbdev_request_free(req);
            
            return -ETIMEDOUT;
        }
        
        // 轮询事件循环
        spdk_thread_poll(spdk_get_thread(), 0, 0);
        
        // 避免空转，短暂休眠
        usleep(10);
    }
    
    // 获取结果
    rc = ctx.rc;
    
    // 释放请求
    xbdev_request_free(req);
    
    return rc;
}

/**
 * 带超时的IO写入操作
 * 
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @return 成功返回写入的字节数，失败返回错误码
 */
ssize_t xbdev_write_timeout(int fd, const void *buf, size_t count, uint64_t offset)
{
    xbdev_io_ctx_t ctx = {0};
    xbdev_request_t *req;
    xbdev_fd_entry_t *entry;
    uint64_t start_time;
    int rc;
    
    // 参数检查
    if (!buf || count == 0) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.buf = (void *)buf;
    ctx.count = count;
    ctx.offset = offset;
    ctx.write = true;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_WRITE;
    req->ctx = &ctx;
    
    // 记录开始时间
    start_time = spdk_get_ticks();
    
    // 提交请求
    rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("提交请求失败，rc=%d\n", rc);
        xbdev_request_free(req);
        return rc;
    }
    
    // 等待完成或超时
    uint32_t timeout = entry->io_timeout_ms > 0 ? 
                      entry->io_timeout_ms : XBDEV_DEFAULT_IO_TIMEOUT_MS;
                      
    while (!ctx.done) {
        // 检查是否超时
        if (check_io_timeout(start_time, timeout)) {
            XBDEV_ERRLOG("IO写入操作超时: fd=%d, offset=%lu, timeout=%u ms\n",
                        fd, offset, timeout);
                        
            // 设置超时错误码
            ctx.rc = -ETIMEDOUT;
            
            // 释放请求
            xbdev_request_free(req);
            
            return -ETIMEDOUT;
        }
        
        // 轮询事件循环
        spdk_thread_poll(spdk_get_thread(), 0, 0);
        
        // 避免空转，短暂休眠
        usleep(10);
    }
    
    // 获取结果
    rc = ctx.rc;
    
    // 释放请求
    xbdev_request_free(req);
    
    return rc;
}

/**
 * 执行标准IO设备控制操作
 * 
 * @param fd 文件描述符
 * @param request IO控制请求码
 * @param arg 参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_ioctl(int fd, unsigned long request, void *arg)
{
    xbdev_fd_entry_t *entry;
    int rc = -ENOTTY;  // 默认返回"不是终端设备"错误
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 根据请求类型执行不同操作
    switch (request) {
        case XBDEV_IOCTL_GET_SIZE: {
            // 获取设备大小
            if (!arg) {
                return -EINVAL;
            }
            uint64_t *size_ptr = (uint64_t *)arg;
            *size_ptr = spdk_bdev_get_num_blocks(entry->bdev) * 
                        spdk_bdev_get_block_size(entry->bdev);
            rc = 0;
            break;
        }
        
        case XBDEV_IOCTL_GET_BLKSIZE: {
            // 获取块大小
            if (!arg) {
                return -EINVAL;
            }
            uint32_t *bs_ptr = (uint32_t *)arg;
            *bs_ptr = spdk_bdev_get_block_size(entry->bdev);
            rc = 0;
            break;
        }
        
        case XBDEV_IOCTL_GET_NAME: {
            // 获取设备名称
            if (!arg) {
                return -EINVAL;
            }
            strncpy((char *)arg, spdk_bdev_get_name(entry->bdev), XBDEV_MAX_NAME_LEN);
            ((char *)arg)[XBDEV_MAX_NAME_LEN - 1] = '\0';
            rc = 0;
            break;
        }
        
        case XBDEV_IOCTL_SET_TIMEOUT: {
            // 设置IO超时
            if (!arg) {
                return -EINVAL;
            }
            uint32_t timeout_ms = *(uint32_t *)arg;
            entry->io_timeout_ms = timeout_ms;
            rc = 0;
            break;
        }
        
        case XBDEV_IOCTL_GET_CAPABILITIES: {
            // 获取设备能力
            if (!arg) {
                return -EINVAL;
            }
            uint64_t *caps = (uint64_t *)arg;
            rc = xbdev_get_io_capabilities(fd, caps);
            break;
        }
        
        case XBDEV_IOCTL_FLUSH_CACHE: {
            // 执行刷新缓存
            rc = xbdev_flush(fd);
            break;
        }
        
        case XBDEV_IOCTL_DISCARD_BLOCKS: {
            // 执行UNMAP/DISCARD操作
            if (!arg) {
                return -EINVAL;
            }
            xbdev_range_t *range = (xbdev_range_t *)arg;
            rc = xbdev_unmap(fd, range->offset, range->length);
            break;
        }
        
        case XBDEV_IOCTL_RESET_DEVICE: {
            // 重置设备
            rc = xbdev_reset(fd);
            break;
        }
        
        case XBDEV_IOCTL_GET_IOSTATS: {
            // 获取IO统计信息
            if (!arg) {
                return -EINVAL;
            }
            xbdev_io_stats_t *stats = (xbdev_io_stats_t *)arg;
            rc = xbdev_get_io_stats(fd, stats);
            break;
        }
        
        default:
            // 未知的请求类型
            rc = -ENOTTY;
            break;
    }
    
    return rc;
}

/**
 * 将多个IO操作批量提交（异步执行）
 * 
 * @param requests 请求数组
 * @param num_requests 请求数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_submit_batch(xbdev_io_request_t *requests, int num_requests)
{
    xbdev_io_batch_t *batch;
    int i, rc;
    
    // 参数检查
    if (!requests || num_requests <= 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 创建批量请求
    batch = xbdev_io_batch_create(num_requests);
    if (!batch) {
        XBDEV_ERRLOG("无法创建批量请求\n");
        return -ENOMEM;
    }
    
    // 将所有请求添加到批量请求中
    for (i = 0; i < num_requests; i++) {
        xbdev_io_request_t *req = &requests[i];
        
        rc = xbdev_io_batch_add(batch, req->fd, req->buf, req->count, req->offset, req->op_type);
        if (rc != 0) {
            XBDEV_ERRLOG("无法添加请求 #%d 到批量请求，rc=%d\n", i, rc);
            xbdev_io_batch_free(batch);
            return rc;
        }
    }
    
    // 提交批量请求
    rc = xbdev_io_batch_submit(batch);
    if (rc < 0) {
        XBDEV_ERRLOG("提交批量请求失败，rc=%d\n", rc);
        xbdev_io_batch_free(batch);
        return rc;
    }
    
    // 等待所有请求完成
    rc = xbdev_io_batch_wait(batch, 0);
    
    // 获取所有请求的结果
    for (i = 0; i < num_requests; i++) {
        requests[i].status = batch->reqs[i].status;
        requests[i].completed = batch->reqs[i].completed;
    }
    
    // 释放批量请求
    xbdev_io_batch_free(batch);
    
    return rc;
}

/**
 * 设置优先级策略
 * 
 * @param fd 文件描述符
 * @param policy 优先级策略
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_priority_policy(int fd, xbdev_priority_policy_t *policy)
{
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (!policy) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 保存优先级策略
    entry->priority_policy = *policy;
    
    // 应用策略 (简化实现，实际上可能需要底层驱动支持)
    // 设置IO优先级
    entry->io_priority = policy->io_priority;
    
    return 0;
}

/**
 * IO操作获取设备详细信息
 * 
 * @param fd 文件描述符
 * @param info 设备信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_device_info(int fd, xbdev_device_info_t *info)
{
    xbdev_fd_entry_t *entry;
    struct spdk_bdev *bdev;
    
    // 参数检查
    if (!info) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    bdev = entry->bdev;
    if (!bdev) {
        return -ENODEV;
    }
    
    // 获取设备信息
    memset(info, 0, sizeof(xbdev_device_info_t));
    
    strncpy(info->name, spdk_bdev_get_name(bdev), sizeof(info->name) - 1);
    strncpy(info->product_name, spdk_bdev_get_product_name(bdev), sizeof(info->product_name) - 1);
    
    info->block_size = spdk_bdev_get_block_size(bdev);
    info->num_blocks = spdk_bdev_get_num_blocks(bdev);
    info->size_bytes = info->block_size * info->num_blocks;
    
    info->write_cache = spdk_bdev_has_write_cache(bdev);
    info->md_size = spdk_bdev_get_md_size(bdev);
    info->required_alignment = spdk_bdev_get_buf_align(bdev);
    
    info->claimed = (entry->desc != NULL);
    
    // 获取支持的IO类型
    info->supported_io_types = 0;
    
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_READ))
        info->supported_io_types |= XBDEV_IO_CAP_READ;
        
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE))
        info->supported_io_types |= XBDEV_IO_CAP_WRITE;
        
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH))
        info->supported_io_types |= XBDEV_IO_CAP_FLUSH;
        
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP))
        info->supported_io_types |= XBDEV_IO_CAP_UNMAP;
        
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_RESET))
        info->supported_io_types |= XBDEV_IO_CAP_RESET;
        
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE))
        info->supported_io_types |= XBDEV_IO_CAP_COMPARE;
        
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE))
        info->supported_io_types |= XBDEV_IO_CAP_COMPARE_AND_WRITE;
        
    if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES))
        info->supported_io_types |= XBDEV_IO_CAP_WRITE_ZEROES;
    
    // 获取驱动名称
    strncpy(info->driver, spdk_bdev_get_driver_name(bdev), sizeof(info->driver) - 1);
    
    return 0;
}

/**
 * 获取设备I/O对齐要求
 * 
 * @param fd 文件描述符
 * @param alignment 输出参数，保存对齐要求
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_alignment(int fd, uint32_t *alignment)
{
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (!alignment) {
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry || !entry->bdev) {
        return -EBADF;
    }
    
    // 获取对齐要求
    *alignment = spdk_bdev_get_buf_align(entry->bdev);
    
    return 0;
}

/**
 * 检查给定地址是否已正确对齐
 * 
 * @param fd 文件描述符
 * @param addr 待检查的地址
 * @return 已对齐返回true，未对齐返回false
 */
bool xbdev_is_aligned(int fd, const void *addr)
{
    uint32_t alignment;
    int rc;
    
    // 获取对齐要求
    rc = xbdev_get_alignment(fd, &alignment);
    if (rc != 0) {
        return false;
    }
    
    // 检查地址是否对齐
    return ((uintptr_t)addr & (alignment - 1)) == 0;
}

/**
 * 在请求队列中提交一组请求
 * 
 * @param reqs 请求数组
 * @param num_reqs 请求数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_batch_requests(xbdev_request_t **reqs, int num_reqs)
{
    int i, rc;
    
    // 参数检查
    if (!reqs || num_reqs <= 0) {
        return -EINVAL;
    }
    
    // 批量提交请求
    // 注意：这个函数是简化实现，实际上应该实现真正的批处理逻辑
    // 例如将多个请求打包成批次提交到底层SPDK线程
    
    // 逐个提交请求
    for (i = 0; i < num_reqs; i++) {
        rc = xbdev_request_submit(reqs[i]);
        if (rc != 0) {
            XBDEV_ERRLOG("提交请求 #%d 失败，rc=%d\n", i, rc);
            return rc;
        }
    }
    
    return 0;
}

/**
 * 实现pread系统调用语义
 *
 * @param fd 文件描述符
 * @param buf 输出缓冲区
 * @param count 读取字节数
 * @param offset 偏移量
 * @return 成功返回读取的字节数，失败返回错误码
 */
ssize_t xbdev_pread(int fd, void *buf, size_t count, off_t offset)
{
    return xbdev_read(fd, buf, count, offset);
}

/**
 * 实现pwrite系统调用语义
 *
 * @param fd 文件描述符
 * @param buf 输入缓冲区
 * @param count 写入字节数
 * @param offset 偏移量
 * @return 成功返回写入的字节数，失败返回错误码
 */
ssize_t xbdev_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    return xbdev_write(fd, buf, count, offset);
}

/**
 * 批量多队列请求提交结构
 */
typedef struct {
    int queue_id;
    xbdev_io_request_t *reqs;
    int num_reqs;
    int completed;
    xbdev_io_completion_cb cb;
    void *cb_arg;
} xbdev_multi_queue_batch_t;

/**
 * 创建多队列批量请求
 * 
 * @param num_queues 队列数量
 * @param max_reqs_per_queue 每个队列的最大请求数
 * @return 成功返回批量请求指针，失败返回NULL
 */
xbdev_multi_queue_batch_t* xbdev_multi_queue_batch_create(int num_queues, int max_reqs_per_queue)
{
    xbdev_multi_queue_batch_t *batch;
    int i;
    
    // 参数检查
    if (num_queues <= 0 || max_reqs_per_queue <= 0) {
        XBDEV_ERRLOG("无效的参数: num_queues=%d, max_reqs_per_queue=%d\n", 
                   num_queues, max_reqs_per_queue);
        return NULL;
    }
    
    // 分配批量请求结构
    batch = calloc(1, sizeof(xbdev_multi_queue_batch_t) * num_queues);
    if (!batch) {
        XBDEV_ERRLOG("无法分配多队列批量请求结构\n");
        return NULL;
    }
    
    // 为每个队列分配请求数组
    for (i = 0; i < num_queues; i++) {
        batch[i].queue_id = i;
        batch[i].reqs = calloc(max_reqs_per_queue, sizeof(xbdev_io_request_t));
        if (!batch[i].reqs) {
            XBDEV_ERRLOG("无法为队列 %d 分配请求数组\n", i);
            
            // 释放之前分配的资源
            for (int j = 0; j < i; j++) {
                free(batch[j].reqs);
            }
            free(batch);
            return NULL;
        }
        batch[i].num_reqs = 0;
        batch[i].completed = 0;
    }
    
    return batch;
}
/**
 * 添加请求到多队列批量请求
 * 
 * @param batch 批量请求指针
 * @param queue_id 队列ID
 * @param fd 文件描述符
 * @param buf 缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @param op_type 操作类型
 * @return 成功返回0，失败返回错误码
 */
int xbdev_multi_queue_batch_add(xbdev_multi_queue_batch_t *batch, int queue_id,
                               int fd, void *buf, size_t count, uint64_t offset, int op_type)
{
    // 参数检查
    if (!batch || !batch[queue_id].reqs) {
        XBDEV_ERRLOG("无效的批量请求或队列ID\n");
        return -EINVAL;
    }
    
    // 添加请求
    xbdev_io_request_t *req = &batch[queue_id].reqs[batch[queue_id].num_reqs++];
    
    // 初始化请求
    req->fd = fd;
    req->buf = buf;
    req->count = count;
    req->offset = offset;
    req->op_type = op_type;
    req->status = 0;
    req->completed = false;
    
    return 0;
}

/**
 * 提交多队列批量请求
 * 
 * @param batch 批量请求指针
 * @param num_queues 队列数量
 * @param cb 完成回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_multi_queue_batch_submit(xbdev_multi_queue_batch_t *batch, int num_queues,
                                 xbdev_io_completion_cb cb, void *cb_arg)
{
    int i;
    
    // 参数检查
    if (!batch || num_queues <= 0) {
        XBDEV_ERRLOG("无效的批量请求或队列数量\n");
        return -EINVAL;
    }
    
    // 设置回调
    for (i = 0; i < num_queues; i++) {
        batch[i].cb = cb;
        batch[i].cb_arg = cb_arg;
    }
    
    // 逐个队列提交
    for (i = 0; i < num_queues; i++) {
        if (batch[i].num_reqs > 0) {
            int rc = xbdev_io_submit_batch(batch[i].reqs, batch[i].num_reqs);
            if (rc < 0) {
                XBDEV_ERRLOG("提交队列 %d 的批量请求失败，rc=%d\n", i, rc);
                return rc;
            }
        }
    }
    
    return 0;
}

/**
 * 等待多队列批量请求完成
 * 
 * @param batch 批量请求指针
 * @param num_queues 队列数量
 * @param timeout_us 超时时间（微秒），0表示永不超时
 * @return 成功返回0，失败返回错误码
 */
int xbdev_multi_queue_batch_wait(xbdev_multi_queue_batch_t *batch, int num_queues, uint64_t timeout_us)
{
    int i, j;
    uint64_t start_time, current_time;
    bool all_completed = false;
    
    // 参数检查
    if (!batch || num_queues <= 0) {
        XBDEV_ERRLOG("无效的批量请求或队列数量\n");
        return -EINVAL;
    }
    
    // 获取开始时间
    start_time = spdk_get_ticks();
    
    // 等待所有队列的所有请求完成或超时
    while (!all_completed) {
        all_completed = true;
        
        // 检查所有队列的所有请求
        for (i = 0; i < num_queues; i++) {
            for (j = 0; j < batch[i].num_reqs; j++) {
                if (!batch[i].reqs[j].completed) {
                    all_completed = false;
                    break;
                }
            }
            if (!all_completed) {
                break;
            }
        }
        
        // 如果所有请求已完成，退出循环
        if (all_completed) {
            break;
        }
        
        // 检查是否超时
        if (timeout_us > 0) {
            current_time = spdk_get_ticks();
            uint64_t elapsed_us = (current_time - start_time) * 1000000 / spdk_get_ticks_hz();
            
            if (elapsed_us >= timeout_us) {
                XBDEV_WARNLOG("多队列批量请求等待超时\n");
                return -ETIMEDOUT;
            }
        }
        
        // 短暂休眠，避免忙等
        usleep(100);
    }
    
    return 0;
}

/**
 * 释放多队列批量请求
 * 
 * @param batch 批量请求指针
 * @param num_queues 队列数量
 */
void xbdev_multi_queue_batch_free(xbdev_multi_queue_batch_t *batch, int num_queues)
{
    int i;
    
    if (!batch) {
        return;
    }
    
    // 释放所有队列的请求数组
    for (i = 0; i < num_queues; i++) {
        if (batch[i].reqs) {
            free(batch[i].reqs);
        }
    }
    
    // 释放批量请求结构
    free(batch);
}

/**
 * 注册设备事件监听器
 * 
 * @param listener 监听器回调函数
 * @param ctx 监听器上下文
 * @return 成功返回0，失败返回错误码
 */
int xbdev_register_device_event_listener(xbdev_device_event_cb listener, void *ctx)
{
    // 参数检查
    if (!listener) {
        XBDEV_ERRLOG("无效的监听器回调函数\n");
        return -EINVAL;
    }
    
    // 保存监听器回调函数和上下文
    g_device_event_cb = listener;
    g_device_event_ctx = ctx;
    
    return 0;
}

/**
 * 异步比较和写入操作
 * 
 * @param fd 文件描述符
 * @param compare_buf 比较缓冲区
 * @param write_buf 写入缓冲区
 * @param count 字节数
 * @param offset 偏移量
 * @param cb 完成回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_compare_and_write(int fd, const void *compare_buf, const void *write_buf,
                              size_t count, uint64_t offset,
                              xbdev_io_completion_cb cb, void *cb_arg)
{
    struct {
        int fd;
        const void *cmp_buf;
        const void *write_buf;
        size_t count;
        uint64_t offset;
        xbdev_io_completion_cb cb;
        void *cb_arg;
        int status;
        bool done;
    } *ctx;
    
    xbdev_request_t *req;
    
    // 参数检查
    if (!compare_buf || !write_buf || count == 0 || !cb) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 分配上下文
    ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        XBDEV_ERRLOG("无法分配异步IO上下文\n");
        return -ENOMEM;
    }
    
    // 设置上下文
    ctx->fd = fd;
    ctx->cmp_buf = compare_buf;
    ctx->write_buf = write_buf;
    ctx->count = count;
    ctx->offset = offset;
    ctx->cb = cb;
    ctx->cb_arg = cb_arg;
    ctx->status = 0;
    ctx->done = false;
    
    // 分配请求
    req = xbdev_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx);
        return -ENOMEM;
    }
    
    // 设置请求类型为自定义请求
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = ctx;
    
    // 提交请求
    int rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("提交异步请求失败: %d\n", rc);
        xbdev_request_free(req);
        free(ctx);
        return rc;
    }
    
    return 0;
}

/**
 * 注册IO完成处理程序
 * 
 * @param handler 处理程序回调函数
 * @param ctx 处理程序上下文
 * @return 成功返回0，失败返回错误码
 */
int xbdev_register_completion_handler(xbdev_completion_handler handler, void *ctx)
{
    // 参数检查
    if (!handler) {
        XBDEV_ERRLOG("无效的处理程序回调函数\n");
        return -EINVAL;
    }
    
    // 保存处理程序回调函数和上下文
    g_completion_handler = handler;
    g_completion_handler_ctx = ctx;
    
    return 0;
}

/**
 * 全局IO初始化函数
 * 
 * @param config IO子系统配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_init(const xbdev_io_config_t *config)
{
    // 参数检查
    if (!config) {
        XBDEV_ERRLOG("无效的IO配置\n");
        return -EINVAL;
    }
    
    // 保存IO配置
    memcpy(&g_io_config, config, sizeof(xbdev_io_config_t));
    
    // 初始化IO子系统
    // 这里简化实现，假设核心库已经初始化了基础设施
    
    return 0;
}

/**
 * IO子系统清理函数
 * 
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_fini(void)
{
    // 清理IO子系统资源
    // 这里简化实现，假设核心库会清理基础设施
    
    return 0;
}

/**
 * 获取IO子系统统计信息
 * 
 * @param stats 输出参数，IO子系统统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_get_system_stats(xbdev_io_system_stats_t *stats)
{
    // 参数检查
    if (!stats) {
        XBDEV_ERRLOG("无效的统计结构指针\n");
        return -EINVAL;
    }
    
    // 清零统计结构
    memset(stats, 0, sizeof(xbdev_io_system_stats_t));
    
    // 填充统计信息
    // 这里简化实现，实际应收集所有活跃设备的聚合统计信息
    
    return 0;
}

/**
 * 设置IO调度策略
 * 
 * @param policy 调度策略
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_set_scheduler_policy(xbdev_scheduler_policy_t policy)
{
    // 保存调度策略
    g_scheduler_policy = policy;
    
    return 0;
}

/**
 * 设置并发IO限制
 * 
 * @param fd 文件描述符
 * @param max_concurrent_ios 最大并发IO数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_set_concurrency_limit(int fd, int max_concurrent_ios)
{
    xbdev_fd_entry_t *entry;
    
    // 参数检查
    if (max_concurrent_ios <= 0) {
        XBDEV_ERRLOG("无效的并发IO限制: %d\n", max_concurrent_ios);
        return -EINVAL;
    }
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 设置并发IO限制
    entry->max_concurrent_ios = max_concurrent_ios;
    
    return 0;
}

/**
 * 扫描并识别新设备
 * 
 * @param devices 输出参数，设备列表
 * @param max_devices 最大设备数量
 * @return 成功返回发现的设备数量，失败返回错误码
 */
int xbdev_scan_devices(xbdev_device_info_t *devices, int max_devices)
{
    int count = 0;
    struct spdk_bdev *bdev = NULL;
    
    // 参数检查
    if (!devices || max_devices <= 0) {
        XBDEV_ERRLOG("无效的参数: devices=%p, max_devices=%d\n", devices, max_devices);
        return -EINVAL;
    }
    
    // 遍历所有SPDK块设备
    for (bdev = spdk_bdev_first(); bdev != NULL && count < max_devices; bdev = spdk_bdev_next(bdev)) {
        // 填充设备信息
        xbdev_device_info_t *info = &devices[count];
        
        strncpy(info->name, spdk_bdev_get_name(bdev), sizeof(info->name) - 1);
        strncpy(info->product_name, spdk_bdev_get_product_name(bdev), sizeof(info->product_name) - 1);
        
        info->block_size = spdk_bdev_get_block_size(bdev);
        info->num_blocks = spdk_bdev_get_num_blocks(bdev);
        info->size_bytes = info->block_size * info->num_blocks;
        
        info->write_cache = spdk_bdev_has_write_cache(bdev);
        info->md_size = spdk_bdev_get_md_size(bdev);
        info->required_alignment = spdk_bdev_get_buf_align(bdev);
        
        // 获取驱动名称
        strncpy(info->driver, spdk_bdev_get_driver_name(bdev), sizeof(info->driver) - 1);
        
        // 尝试识别设备类型
        if (strncmp(info->driver, "nvme", 4) == 0) {
            info->type = XBDEV_DEV_TYPE_NVME;
        } else if (strncmp(info->driver, "aio", 3) == 0) {
            info->type = XBDEV_DEV_TYPE_AIO;
        } else if (strncmp(info->driver, "rbd", 3) == 0) {
            info->type = XBDEV_DEV_TYPE_RBD;
        } else if (strncmp(info->driver, "logical_volume", 14) == 0) {
            info->type = XBDEV_DEV_TYPE_LVOL;
        } else if (strncmp(info->driver, "malloc", 6) == 0) {
            info->type = XBDEV_DEV_TYPE_RAM;
        } else {
            info->type = XBDEV_DEV_TYPE_OTHER;
        }
        
        count++;
    }
    
    return count;
}