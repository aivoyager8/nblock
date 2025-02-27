/**
 * @file core.c
 * @brief 实现libxbdev的核心初始化和清理功能
 *
 * 该文件实现了libxbdev的核心功能，包括初始化、清理、
 * 事件轮询以及全局状态管理。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/init.h>
#include <spdk/bdev.h>
#include <spdk/thread.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * 全局状态
 */
static struct {
    bool initialized;                // 是否已初始化
    struct spdk_thread *thread;      // SPDK线程
    pthread_t poll_thread;           // 轮询线程
    bool poll_thread_stop;           // 轮询线程停止标志
    xbdev_fd_table_t fd_table;       // 文件描述符表
    pthread_mutex_t fd_table_lock;   // 文件描述符表锁
    struct spdk_poller *poller;      // SPDK轮询器
    bool shutdown_requested;         // 是否请求关闭
    int reactor_delay_us;            // 反应器延迟(微秒)，控制轮询频率
    int last_fd;                     // 上次分配的文件描述符
} g_xbdev = {0};

/**
 * 轮询函数
 *
 * 处理SPDK事件和队列中的请求。
 *
 * @param ctx 上下文参数(未使用)
 * @return 持续轮询标志
 */
static int _xbdev_poll(void *ctx)
{
    // 如果请求关闭，停止轮询
    if (g_xbdev.shutdown_requested) {
        return SPDK_POLLER_IDLE;
    }
    
    // 处理队列中的请求
    xbdev_process_request_queues();
    
    // 继续轮询
    return SPDK_POLLER_BUSY;
}

/**
 * 轮询线程函数
 *
 * 持续运行，调用SPDK线程轮询以处理事件。
 *
 * @param arg 线程参数(未使用)
 * @return 总是NULL
 */
static void *_xbdev_poll_thread(void *arg)
{
    while (!g_xbdev.poll_thread_stop) {
        // 在SPDK线程上下文中运行轮询
        spdk_thread_poll(g_xbdev.thread, 0, 0);
        
        // 短暂睡眠以避免过度占用CPU
        if (g_xbdev.reactor_delay_us > 0) {
            usleep(g_xbdev.reactor_delay_us);
        }
    }
    
    return NULL;
}

/**
 * 初始化文件描述符表
 */
static void _xbdev_init_fd_table(void)
{
    memset(&g_xbdev.fd_table, 0, sizeof(g_xbdev.fd_table));
    
    // 初始化文件描述符锁
    pthread_mutex_init(&g_xbdev.fd_table_lock, NULL);
    
    // 文件描述符从3开始，0-2通常为标准输入、输出和错误
    g_xbdev.last_fd = 2;
}

/**
 * 分配文件描述符
 *
 * @return 成功返回文件描述符，失败返回-1
 */
static int _xbdev_alloc_fd(void)
{
    int fd = -1;
    
    pthread_mutex_lock(&g_xbdev.fd_table_lock);
    
    // 查找未使用的文件描述符
    for (int i = 0; i < XBDEV_MAX_OPEN_FILES; i++) {
        int idx = (g_xbdev.last_fd + 1 + i) % XBDEV_MAX_OPEN_FILES;
        
        // 跳过小于3的索引，这些通常是标准输入输出
        if (idx < 3) continue;
        
        if (!g_xbdev.fd_table.entries[idx].in_use) {
            g_xbdev.fd_table.entries[idx].in_use = true;
            g_xbdev.fd_table.entries[idx].fd = idx;
            g_xbdev.last_fd = idx;
            fd = idx;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_xbdev.fd_table_lock);
    
    return fd;
}

/**
 * 获取文件描述符对应的表项
 *
 * @param fd 文件描述符
 * @return 成功返回表项指针，失败返回NULL
 */
static xbdev_fd_entry_t *_xbdev_get_fd_entry(int fd)
{
    if (fd < 3 || fd >= XBDEV_MAX_OPEN_FILES) {
        XBDEV_ERRLOG("无效的文件描述符: %d\n", fd);
        return NULL;
    }
    
    pthread_mutex_lock(&g_xbdev.fd_table_lock);
    
    xbdev_fd_entry_t *entry = NULL;
    
    if (g_xbdev.fd_table.entries[fd].in_use) {
        entry = &g_xbdev.fd_table.entries[fd];
    } else {
        XBDEV_ERRLOG("文件描述符未使用: %d\n", fd);
    }
    
    pthread_mutex_unlock(&g_xbdev.fd_table_lock);
    
    return entry;
}

/**
 * 释放文件描述符
 *
 * @param fd 文件描述符
 */
static void _xbdev_free_fd(int fd)
{
    if (fd < 3 || fd >= XBDEV_MAX_OPEN_FILES) {
        XBDEV_ERRLOG("无效的文件描述符: %d\n", fd);
        return;
    }
    
    pthread_mutex_lock(&g_xbdev.fd_table_lock);
    
    // 清空表项
    memset(&g_xbdev.fd_table.entries[fd], 0, sizeof(xbdev_fd_entry_t));
    
    pthread_mutex_unlock(&g_xbdev.fd_table_lock);
}

/**
 * 关闭所有打开的文件描述符
 */
static void _xbdev_close_all_fds(void)
{
    pthread_mutex_lock(&g_xbdev.fd_table_lock);
    
    for (int fd = 3; fd < XBDEV_MAX_OPEN_FILES; fd++) {
        if (g_xbdev.fd_table.entries[fd].in_use) {
            _xbdev_close_desc(&g_xbdev.fd_table.entries[fd]);
            memset(&g_xbdev.fd_table.entries[fd], 0, sizeof(xbdev_fd_entry_t));
        }
    }
    
    pthread_mutex_unlock(&g_xbdev.fd_table_lock);
}

/**
 * 关闭BDEV描述符
 *
 * @param entry 文件描述符表项
 */
void _xbdev_close_desc(xbdev_fd_entry_t *entry)
{
    if (!entry || !entry->desc) {
        return;
    }
    
    // 关闭BDEV描述符
    spdk_bdev_close(entry->desc);
    entry->desc = NULL;
    entry->bdev = NULL;
}

/**
 * 初始化SPDK子系统
 */
static int _xbdev_init_spdk(void)
{
    struct spdk_env_opts env_opts;
    int rc;
    
    // 初始化环境选项
    spdk_env_opts_init(&env_opts);
    env_opts.name = "xbdev";
    env_opts.shm_id = 0;
    env_opts.mem_size = 512;  // 512MB内存
    env_opts.no_pci = false;
    env_opts.hugepage_single_segments = true;
    
    // 初始化SPDK环境
    rc = spdk_env_init(&env_opts);
    if (rc != 0) {
        XBDEV_ERRLOG("spdk_env_init失败: %d\n", rc);
        return rc;
    }
    
    // 初始化SPDK日志
    spdk_log_set_print_level(SPDK_LOG_NOTICE);
    spdk_log_open();
    
    // 初始化SPDK线程
    g_xbdev.thread = spdk_thread_create("xbdev_thread", NULL);
    if (!g_xbdev.thread) {
        XBDEV_ERRLOG("spdk_thread_create失败\n");
        return -ENOMEM;
    }
    
    // 设置当前线程
    spdk_set_thread(g_xbdev.thread);
    
    // 创建轮询器
    g_xbdev.poller = spdk_poller_register(_xbdev_poll, NULL, 0);
    if (!g_xbdev.poller) {
        XBDEV_ERRLOG("spdk_poller_register失败\n");
        spdk_thread_exit(g_xbdev.thread);
        return -ENOMEM;
    }
    
    return 0;
}

/**
 * 清理SPDK子系统
 */
static void _xbdev_fini_spdk(void)
{
    // 注销轮询器
    if (g_xbdev.poller) {
        spdk_poller_unregister(&g_xbdev.poller);
    }
    
    // 退出SPDK线程
    if (g_xbdev.thread) {
        spdk_thread_exit(g_xbdev.thread);
        g_xbdev.thread = NULL;
    }
    
    // 清理SPDK日志
    spdk_log_close();
}

/**
 * 启动轮询线程
 */
static int _xbdev_start_poll_thread(void)
{
    g_xbdev.poll_thread_stop = false;
    g_xbdev.reactor_delay_us = 1000;  // 默认1ms轮询间隔
    
    int rc = pthread_create(&g_xbdev.poll_thread, NULL, _xbdev_poll_thread, NULL);
    if (rc != 0) {
        XBDEV_ERRLOG("pthread_create失败: %d\n", rc);
        return -rc;
    }
    
    return 0;
}

/**
 * 停止轮询线程
 */
static void _xbdev_stop_poll_thread(void)
{
    g_xbdev.poll_thread_stop = true;
    
    if (pthread_join(g_xbdev.poll_thread, NULL) != 0) {
        XBDEV_ERRLOG("pthread_join失败\n");
    }
}

/**
 * 初始化libxbdev库
 *
 * 初始化SPDK环境、线程模型和请求处理系统。
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_init(void)
{
    int rc;
    
    // 检查是否已初始化
    if (g_xbdev.initialized) {
        XBDEV_WARNLOG("libxbdev already initialized\n");
        return 0;
    }
    
    // 初始化SPDK子系统
    rc = _xbdev_init_spdk();
    if (rc != 0) {
        XBDEV_ERRLOG("SPDK initialization failed: %d\n", rc);
        return rc;
    }
    
    // 初始化文件描述符表
    _xbdev_init_fd_table();
    
    // 初始化请求队列系统
    rc = xbdev_queue_init(1024, 4);  // 4个队列，每个队列1024项
    if (rc != 0) {
        XBDEV_ERRLOG("Request queue initialization failed: %d\n", rc);
        _xbdev_fini_spdk();
        return rc;
    }
    
    // 启动轮询线程
    rc = _xbdev_start_poll_thread();
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to start polling thread: %d\n", rc);
        xbdev_queue_fini();
        _xbdev_fini_spdk();
        return rc;
    }
    
    // 标记为已初始化
    g_xbdev.initialized = true;
    g_xbdev.shutdown_requested = false;
    
    XBDEV_NOTICELOG("libxbdev initialized successfully (version %d.%d.%d)\n", 
                  XBDEV_VERSION_MAJOR, XBDEV_VERSION_MINOR, XBDEV_VERSION_PATCH);
    
    return 0;
}

/**
 * 清理libxbdev库
 *
 * 释放所有资源并清理SPDK环境。
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_fini(void)
{
    // 检查是否已初始化
    if (!g_xbdev.initialized) {
        XBDEV_WARNLOG("libxbdev not initialized\n");
        return 0;
    }
    
    // 请求关闭
    g_xbdev.shutdown_requested = true;
    
    // 关闭所有文件描述符
    _xbdev_close_all_fds();
    
    // 停止轮询线程
    _xbdev_stop_poll_thread();
    
    // 清理请求队列系统
    xbdev_queue_fini();
    
    // 清理SPDK子系统
    _xbdev_fini_spdk();
    
    // 销毁文件描述符表锁
    pthread_mutex_destroy(&g_xbdev.fd_table_lock);
    
    // 复位全局状态
    memset(&g_xbdev, 0, sizeof(g_xbdev));
    
    XBDEV_NOTICELOG("libxbdev cleanup completed\n");
    
    return 0;
}

/**
 * 设置轮询线程的延迟时间
 *
 * @param delay_us 延迟时间(微秒)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_reactor_delay(int delay_us)
{
    if (!g_xbdev.initialized) {
        XBDEV_ERRLOG("libxbdev未初始化\n");
        return -EINVAL;
    }
    
    if (delay_us < 0) {
        XBDEV_ERRLOG("无效的延迟值: %d\n", delay_us);
        return -EINVAL;
    }
    
    g_xbdev.reactor_delay_us = delay_us;
    
    return 0;
}

/**
 * 获取设备列表
 *
 * @param names 输出参数，存储设备名称的数组
 * @param max_names 数组大小
 * @return 成功返回找到的设备数量，失败返回错误码
 */
int xbdev_get_device_list(char **names, int max_names)
{
    int count = 0;
    struct spdk_bdev *bdev;
    
    if (!g_xbdev.initialized) {
        XBDEV_ERRLOG("libxbdev未初始化\n");
        return -EINVAL;
    }
    
    if (!names || max_names <= 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 遍历所有BDEV
    for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
        if (count < max_names) {
            names[count] = strdup(spdk_bdev_get_name(bdev));
            if (!names[count]) {
                XBDEV_ERRLOG("内存分配失败\n");
                // 释放已分配的内存
                for (int i = 0; i < count; i++) {
                    free(names[i]);
                }
                return -ENOMEM;
            }
            count++;
        } else {
            break;
        }
    }
    
    return count;
}

/**
 * 获取设备信息
 *
 * @param name 设备名称
 * @param info 输出参数，存储设备信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_device_info(const char *name, xbdev_device_info_t *info)
{
    struct spdk_bdev *bdev;
    
    if (!g_xbdev.initialized) {
        XBDEV_ERRLOG("libxbdev未初始化\n");
        return -EINVAL;
    }
    
    if (!name || !info) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 查找设备
    bdev = spdk_bdev_get_by_name(name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", name);
        return -ENODEV;
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
 * 打开BDEV设备
 *
 * @param bdev_name 设备名称
 * @return 成功返回文件描述符，失败返回-1
 */
int xbdev_open(const char *bdev_name)
{
    int fd;
    xbdev_request_t *req;
    struct {
        const char *name;
        int fd;
        struct spdk_bdev *bdev;
        struct spdk_bdev_desc *desc;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    if (!g_xbdev.initialized) {
        XBDEV_ERRLOG("libxbdev not initialized\n");
        return -EINVAL;
    }
    
    if (!bdev_name) {
        XBDEV_ERRLOG("Invalid device name\n");
        return -EINVAL;
    }
    
    // 分配文件描述符
    fd = _xbdev_alloc_fd();
    if (fd < 0) {
        XBDEV_ERRLOG("Failed to allocate file descriptor\n");
        return -EMFILE;
    }
    
    // 设置上下文
    ctx.name = bdev_name;
    ctx.fd = fd;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        _xbdev_free_fd(fd);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_OPEN;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        xbdev_sync_request_free(req);
        _xbdev_free_fd(fd);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to open device: %s, rc=%d\n", bdev_name, rc);
        xbdev_sync_request_free(req);
        _xbdev_free_fd(fd);
        return rc;
    }
    
    // 更新文件描述符表
    pthread_mutex_lock(&g_xbdev.fd_table_lock);
    g_xbdev.fd_table.entries[fd].bdev = ctx.bdev;
    g_xbdev.fd_table.entries[fd].desc = ctx.desc;
    pthread_mutex_unlock(&g_xbdev.fd_table_lock);
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("Successfully opened device: %s, fd=%d\n", bdev_name, fd);
    
    return fd;
}

/**
 * SPDK线程上下文中执行打开操作
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
    
    args->done = true;
}

/**
 * 关闭BDEV设备
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_close(int fd)
{
    xbdev_request_t *req;
    struct {
        int fd;
        bool done;
        int rc;
    } ctx = {0};
    int rc;
    
    if (!g_xbdev.initialized) {
        XBDEV_ERRLOG("libxbdev未初始化\n");
        return -EINVAL;
    }
    
    // 检查文件描述符
    xbdev_fd_entry_t *entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        XBDEV_ERRLOG("无效的文件描述符: %d\n", fd);
        return -EBADF;
    }
    
    // 设置上下文
    ctx.fd = fd;
    ctx.done = false;
    ctx.rc = 0;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_CLOSE;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("关闭设备失败: fd=%d, rc=%d\n", fd, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放文件描述符
    _xbdev_free_fd(fd);
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("成功关闭设备: fd=%d\n", fd);
    
    return 0;
}

/**
 * SPDK线程上下文中执行关闭操作
 */
void xbdev_close_on_thread(void *ctx)
{
    struct {
        int fd;
        bool done;
        int rc;
    } *args = ctx;
    
    // 获取文件描述符表项
    xbdev_fd_entry_t *entry = _xbdev_get_fd_entry(args->fd);
    if (!entry) {
        XBDEV_ERRLOG("无效的文件描述符: %d\n", args->fd);
        args->rc = -EBADF;
        args->done = true;
        return;
    }
    
    // 关闭BDEV描述符
    if (entry->desc) {
        spdk_bdev_close(entry->desc);
        entry->desc = NULL;
        entry->bdev = NULL;
    }
    
    args->done = true;
}