/**
 * @file request_handler.c
 * @brief 实现请求处理和分发功能
 *
 * 该文件实现请求分发和处理机制，将请求路由到对应的处理函数。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/bdev.h>
#include <stdlib.h>
#include <string.h>

/**
 * 处理打开请求的函数
 */
void xbdev_open_on_thread(void *ctx)
{
    struct {
        const char *name;
        int fd;
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        bool done;
        int rc;
    } *args = ctx;
    
    // 获取BDEV
    args->bdev = spdk_bdev_get_by_name(args->name);
    if (!args->bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", args->name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // 打开BDEV
    args->rc = spdk_bdev_open_ext(args->name, true, NULL, NULL, &args->desc);
    if (args->rc != 0) {
        XBDEV_ERRLOG("打开设备失败: %s, rc=%d\n", args->name, args->rc);
        args->done = true;
        return;
    }
    
    // 保存设备名称
    strncpy(_xbdev_get_fd_entry(args->fd)->name, args->name, 255);
    
    args->done = true;
}

/**
 * UNMAP操作的处理函数
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
    rc = spdk_bdev_unmap(entry->desc, io_channel, ctx->offset, ctx->length, 
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
 * 重置设备操作的处理函数
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
    
    // 执行重置操作
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
 * 刷新缓存操作的处理函数
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
    
    // 执行刷新操作
    rc = spdk_bdev_flush(entry->desc, io_channel, 0, spdk_bdev_get_num_blocks(entry->bdev) * 
                       spdk_bdev_get_block_size(entry->bdev), io_completion_cb, &cb_arg);
    
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
 * 关闭设备操作的处理函数
 */
void xbdev_close_on_thread(void *arg)
{
    struct {
        int fd;
        bool done;
        int rc;
    } *ctx = arg;
    
    // 获取文件描述符表项
    xbdev_fd_entry_t *entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry) {
        XBDEV_ERRLOG("无效的文件描述符: %d\n", ctx->fd);
        ctx->rc = -EBADF;
        ctx->done = true;
        return;
    }
    
    // 关闭BDEV描述符
    if (entry->desc) {
        spdk_bdev_close(entry->desc);
        entry->desc = NULL;
        entry->bdev = NULL;
    }
    
    ctx->done = true;
}

/**
 * 注册块设备的处理函数 - 通用分发
 */
void xbdev_register_bdev_on_thread(void *ctx)
{
    // 根据设备类型分发到不同的注册函数
    // 通过ctx中的设备类型字段来判断
    
    // 示例：检查第一个字段来确定设备类型，然后转发
    struct {
        xbdev_bdev_type_t type;
        void *params;
        bool done;
        int rc;
    } *args = ctx;
    
    switch (args->type) {
        case XBDEV_BDEV_TYPE_NVME:
            xbdev_nvme_register_on_thread(args->params);
            break;
            
        case XBDEV_BDEV_TYPE_NVMF:
            xbdev_nvmf_register_on_thread(args->params);
            break;
            
        case XBDEV_BDEV_TYPE_AIO:
            xbdev_aio_register_on_thread(args->params);
            break;
            
        case XBDEV_BDEV_TYPE_MALLOC:
            xbdev_malloc_register_on_thread(args->params);
            break;
            
        case XBDEV_BDEV_TYPE_NULL:
            xbdev_null_register_on_thread(args->params);
            break;
            
        default:
            XBDEV_ERRLOG("未知的设备类型: %d\n", args->type);
            args->rc = -EINVAL;
            break;
    }
    
    args->done = true;
}

/**
 * 异步读写完成回调
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
    
    // 释放BDEV IO
    spdk_bdev_free_io(bdev_io);
    
    // 释放上下文
    free(ctx);
}

/**
 * 异步读请求处理函数
 */
void xbdev_aio_read_on_thread(void *arg)
{
    struct io_async_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc) {
        ctx->status = -EBADF;
        ctx->done = true;
        
        // 调用用户回调
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, ctx->status, ctx->user_ctx);
        }
        
        free(ctx);
        return;
    }
    
    // 获取IO通道
    io_channel = spdk_bdev_get_io_channel(entry->desc);
    if (!io_channel) {
        ctx->status = -ENOMEM;
        ctx->done = true;
        
        // 调用用户回调
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, ctx->status, ctx->user_ctx);
        }
        
        free(ctx);
        return;
    }
    
    // 执行异步读操作
    rc = spdk_bdev_read(entry->desc, io_channel, ctx->buf, ctx->offset, ctx->count, 
                      aio_completion_cb, ctx);
    
    // 释放IO通道
    spdk_put_io_channel(io_channel);
    
    // 检查提交状态
    if (rc != 0) {
        ctx->status = rc;
        ctx->done = true;
        
        // 调用用户回调
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, ctx->status, ctx->user_ctx);
        }
        
        free(ctx);
    }
    
    // 注意：成功提交的请求将在完成回调中释放上下文
}

/**
 * 异步写请求处理函数
 */
void xbdev_aio_write_on_thread(void *arg)
{
    struct io_async_ctx *ctx = arg;
    xbdev_fd_entry_t *entry;
    struct spdk_io_channel *io_channel;
    int rc;
    
    // 获取文件描述符表项
    entry = _xbdev_get_fd_entry(ctx->fd);
    if (!entry || !entry->desc) {
        ctx->status = -EBADF;
        ctx->done = true;
        
        // 调用用户回调
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, ctx->status, ctx->user_ctx);
        }
        
        free(ctx);
        return;
    }
    
    // 获取IO通道
    io_channel = spdk_bdev_get_io_channel(entry->desc);
    if (!io_channel) {
        ctx->status = -ENOMEM;
        ctx->done = true;
        
        // 调用用户回调
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, ctx->status, ctx->user_ctx);
        }
        
        free(ctx);
        return;
    }
    
    // 执行异步写操作
    rc = spdk_bdev_write(entry->desc, io_channel, ctx->buf, ctx->offset, ctx->count, 
                       aio_completion_cb, ctx);
    
    // 释放IO通道
    spdk_put_io_channel(io_channel);
    
    // 检查提交状态
    if (rc != 0) {
        ctx->status = rc;
        ctx->done = true;
        
        // 调用用户回调
        if (ctx->user_cb) {
            ctx->user_cb(ctx->fd, ctx->status, ctx->user_ctx);
        }
        
        free(ctx);
    }
    
    // 注意：成功提交的请求将在完成回调中释放上下文
}

/**
 * IO完成回调函数（通用）
 */
void io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
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
 * 设备移除处理函数
 */
void xbdev_device_remove_on_thread(void *arg)
{
    struct {
        const char *name;
        int rc;
        bool done;
    } *args = arg;
    
    // 查找设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(args->name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", args->name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // 获取设备的模块
    const struct spdk_bdev_module *module = spdk_bdev_get_module(bdev);
    if (!module) {
        XBDEV_ERRLOG("找不到设备模块: %s\n", args->name);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 根据设备类型执行不同的删除操作
    const char *module_name = spdk_bdev_get_module_name(bdev);
    
    if (strcmp(module_name, "nvme") == 0) {
        // 删除NVMe设备
        args->rc = spdk_bdev_nvme_delete(args->name);
    } else if (strcmp(module_name, "aio") == 0) {
        // 删除AIO设备
        args->rc = spdk_bdev_aio_delete(args->name);
    } else if (strcmp(module_name, "malloc") == 0) {
        // 删除Malloc设备
        args->rc = spdk_bdev_delete_malloc_disk(bdev);
    } else if (strcmp(module_name, "lvol") == 0) {
        // 删除LVOL设备
        struct spdk_lvol *lvol = spdk_lvol_get_from_bdev(bdev);
        if (lvol) {
            args->rc = spdk_lvol_destroy(lvol, NULL, NULL);
        } else {
            args->rc = -EINVAL;
        }
    } else {
        // 尝试通用删除方式
        XBDEV_WARNLOG("未知设备类型: %s, 模块: %s，尝试通用删除方法\n", args->name, module_name);
        args->rc = -ENOTSUP;
    }
    
    if (args->rc != 0) {
        XBDEV_ERRLOG("删除设备失败: %s, rc=%d\n", args->name, args->rc);
    } else {
        XBDEV_NOTICELOG("成功删除设备: %s\n", args->name);
    }
    
    args->done = true;
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
    rc = spdk_bdev_write(entry->desc, io_channel, ctx->buf, ctx->offset, ctx->count, 
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
 * 分发RAID相关请求到对应的处理函数
 */
void xbdev_md_dispatch_request(xbdev_request_t *req)
{
    // 根据请求类型将请求分发到不同的RAID处理函数
    switch (req->type) {
        case XBDEV_REQ_RAID_CREATE:
            // TODO: 实现RAID创建处理
            break;
            
        case XBDEV_REQ_RAID_STOP:
            // TODO: 实现RAID停止处理
            break;
            
        case XBDEV_REQ_RAID_ASSEMBLE:
            // TODO: 实现RAID组装处理
            break;
            
        case XBDEV_REQ_RAID_EXAMINE:
            // TODO: 实现RAID检查处理
            break;
            
        case XBDEV_REQ_RAID_DETAIL:
            // TODO: 实现RAID详细信息处理
            break;
            
        case XBDEV_REQ_RAID_MANAGE:
            // TODO: 实现RAID管理处理
            break;
            
        case XBDEV_REQ_RAID_REPLACE_DISK:
            // TODO: 实现RAID替换磁盘处理
            break;
            
        case XBDEV_REQ_RAID_REBUILD_CONTROL:
            // TODO: 实现RAID重建控制处理
            break;
            
        case XBDEV_REQ_RAID_SET_DISK_STATE:
            // TODO: 实现RAID设置磁盘状态处理
            break;
            
        default:
            XBDEV_ERRLOG("未知的RAID请求类型: %d\n", req->type);
            break;
    }
    
    // 设置请求为已完成
    req->done = true;
}
