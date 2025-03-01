/**
 * @file core.c
 * @brief 实现库的核心初始化、清理和公共功能
 *
 * 该文件提供libxbdev库的初始化、清理和基本功能实现，
 * 包括文件描述符管理、设备打开关闭等函数。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/env.h>
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/queue.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

// 全局变量
static bool g_xbdev_initialized = false;
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, xbdev_fd_entry) g_fd_list = TAILQ_HEAD_INITIALIZER(g_fd_list);
static int g_next_fd = 1;

/**
 * 初始化libxbdev库
 */
int xbdev_init(void)
{
    int rc = 0;
    
    pthread_mutex_lock(&g_init_mutex);
    
    if (g_xbdev_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return 0;  // 已经初始化
    }
    
    // 初始化SPDK环境
    struct spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    rc = spdk_env_init(&opts);
    if (rc != 0) {
        XBDEV_ERRLOG("初始化SPDK环境失败: %d\n", rc);
        pthread_mutex_unlock(&g_init_mutex);
        return rc;
    }
    
    // 初始化SPDK线程库
    rc = spdk_thread_lib_init();
    if (rc != 0) {
        XBDEV_ERRLOG("初始化SPDK线程库失败: %d\n", rc);
        pthread_mutex_unlock(&g_init_mutex);
        return rc;
    }
    
    // 创建默认的SPDK线程
    struct spdk_thread *thread = spdk_thread_create("xbdev_thread", NULL);
    if (!thread) {
        XBDEV_ERRLOG("创建SPDK线程失败\n");
        spdk_thread_lib_fini();
        pthread_mutex_unlock(&g_init_mutex);
        return -ENOMEM;
    }
    
    // 设置为当前线程
    spdk_set_thread(thread);
    
    // 初始化文件描述符列表
    TAILQ_INIT(&g_fd_list);
    
    g_xbdev_initialized = true;
    
    pthread_mutex_unlock(&g_init_mutex);
    return 0;
}

/**
 * 清理libxbdev库
 */
int xbdev_fini(void)
{
    pthread_mutex_lock(&g_init_mutex);
    
    if (!g_xbdev_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return 0;  // 未初始化
    }
    
    // 关闭所有打开的文件描述符
    xbdev_fd_entry_t *entry, *tmp;
    TAILQ_FOREACH_SAFE(entry, &g_fd_list, link, tmp) {
        TAILQ_REMOVE(&g_fd_list, entry, link);
        if (entry->desc) {
            spdk_bdev_close(entry->desc);
        }
        free(entry);
    }
    
    // 清理SPDK线程和线程库
    struct spdk_thread *thread = spdk_get_thread();
    if (thread) {
        spdk_thread_exit(thread);
        while (!spdk_thread_is_exited(thread)) {
            spdk_thread_poll(thread, 0, 0);
        }
        spdk_thread_destroy(thread);
    }
    
    spdk_thread_lib_fini();
    
    g_xbdev_initialized = false;
    
    pthread_mutex_unlock(&g_init_mutex);
    return 0;
}

/**
 * 分配新的文件描述符
 */
static int allocate_fd(void)
{
    int fd = g_next_fd++;
    return fd;
}

/**
 * 创建新的文件描述符表项
 */
static xbdev_fd_entry_t* create_fd_entry(const char *bdev_name)
{
    xbdev_fd_entry_t *entry;
    
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }
    
    entry->fd = allocate_fd();
    strncpy(entry->bdev_name, bdev_name, sizeof(entry->bdev_name) - 1);
    pthread_mutex_init(&entry->mutex, NULL);
    
    return entry;
}

/**
 * 查找文件描述符表项
 */
xbdev_fd_entry_t* _xbdev_get_fd_entry(int fd)
{
    xbdev_fd_entry_t *entry;
    
    TAILQ_FOREACH(entry, &g_fd_list, link) {
        if (entry->fd == fd) {
            return entry;
        }
    }
    
    return NULL;
}

/**
 * 打开bdev设备
 */
int xbdev_open(const char *bdev_name)
{
    int rc = 0;
    struct spdk_bdev *bdev;
    struct spdk_bdev_desc *desc = NULL;
    xbdev_fd_entry_t *entry;
    
    if (!bdev_name) {
        return -EINVAL;
    }
    
    // 获取bdev
    bdev = spdk_bdev_get_by_name(bdev_name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到bdev设备: %s\n", bdev_name);
        return -ENODEV;
    }
    
    // 打开bdev
    rc = spdk_bdev_open(bdev, true, NULL, NULL, &desc);
    if (rc != 0) {
        XBDEV_ERRLOG("打开bdev设备失败: %s, rc=%d\n", bdev_name, rc);
        return rc;
    }
    
    // 创建文件描述符表项
    entry = create_fd_entry(bdev_name);
    if (!entry) {
        spdk_bdev_close(desc);
        return -ENOMEM;
    }
    
    // 初始化表项
    entry->bdev = bdev;
    entry->desc = desc;
    
    // 添加到列表
    TAILQ_INSERT_TAIL(&g_fd_list, entry, link);
    
    return entry->fd;
}

/**
 * 关闭bdev设备
 */
int xbdev_close(int fd)
{
    xbdev_fd_entry_t *entry;
    
    entry = _xbdev_get_fd_entry(fd);
    if (!entry) {
        return -EBADF;
    }
    
    // 从列表中移除
    TAILQ_REMOVE(&g_fd_list, entry, link);
    
    // 关闭设备
    if (entry->desc) {
        spdk_bdev_close(entry->desc);
    }
    
    // 释放资源
    pthread_mutex_destroy(&entry->mutex);
    free(entry);
    
    return 0;
}

/**
 * 分配DMA对齐的内存
 */
void* xbdev_dma_malloc(size_t size, size_t align)
{
    return spdk_dma_malloc(size, align, NULL);
}

/**
 * 释放DMA内存
 */
void xbdev_dma_free(void *buf)
{
    spdk_dma_free(buf);
}