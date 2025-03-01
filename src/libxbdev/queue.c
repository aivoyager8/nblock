/**
 * @file queue.c
 * @brief 实现请求队列管理
 *
 * 该文件实现请求队列和请求处理系统，用于在应用线程和SPDK线程之间传递请求。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/util.h>
#include <spdk/queue.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/**
 * 请求队列结构
 */
typedef struct {
    pthread_mutex_t mutex;         // 队列互斥锁
    pthread_cond_t cond;           // 条件变量，用于同步请求等待
    xbdev_request_t **entries;     // 队列项数组
    uint32_t capacity;             // 队列容量
    uint32_t head;                 // 队列头指针
    uint32_t tail;                 // 队列尾指针
    uint32_t size;                 // 当前队列大小
    bool initialized;              // 是否已初始化
} xbdev_queue_t;

/**
 * 请求池结构
 */
typedef struct {
    pthread_mutex_t mutex;         // 池互斥锁
    xbdev_request_t *entries;      // 请求项数组
    bool *used;                    // 使用状态数组
    uint32_t capacity;             // 池容量
    uint32_t used_count;           // 已使用的请求数
    bool initialized;              // 是否已初始化
} xbdev_request_pool_t;

/**
 * 全局队列系统
 */
static struct {
    xbdev_queue_t *req_queues;     // 请求队列数组
    uint32_t num_queues;           // 队列数量
    xbdev_request_pool_t req_pool; // 请求池
    uint64_t next_req_id;          // 下一个请求ID
    bool initialized;              // 是否已初始化
} g_queue_system = {0};

/**
 * 初始化请求队列
 *
 * @param queue 队列指针
 * @param capacity 队列容量
 * @return 成功返回0，失败返回错误码
 */
static int init_queue(xbdev_queue_t *queue, uint32_t capacity)
{
    int rc;
    
    if (!queue || capacity == 0) {
        return -EINVAL;
    }
    
    // 初始化互斥锁和条件变量
    rc = pthread_mutex_init(&queue->mutex, NULL);
    if (rc != 0) {
        return -rc;
    }
    
    rc = pthread_cond_init(&queue->cond, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return -rc;
    }
    
    // 分配队列项数组
    queue->entries = calloc(capacity, sizeof(xbdev_request_t *));
    if (!queue->entries) {
        pthread_mutex_destroy(&queue->mutex);
        pthread_cond_destroy(&queue->cond);
        return -ENOMEM;
    }
    
    // 初始化队列状态
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
    queue->initialized = true;
    
    return 0;
}

/**
 * 销毁请求队列
 *
 * @param queue 队列指针
 */
static void destroy_queue(xbdev_queue_t *queue)
{
    if (!queue || !queue->initialized) {
        return;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // 释放队列项数组
    free(queue->entries);
    queue->entries = NULL;
    
    pthread_mutex_unlock(&queue->mutex);
    
    // 销毁互斥锁和条件变量
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
    
    // 重置队列状态
    queue->capacity = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
    queue->initialized = false;
}

/**
 * 入队请求
 *
 * @param queue 队列指针
 * @param req 请求指针
 * @return 成功返回0，队列满返回-EAGAIN，其他错误返回相应错误码
 */
static int enqueue_request(xbdev_queue_t *queue, xbdev_request_t *req)
{
    int rc = 0;
    
    if (!queue || !req) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&queue->mutex);
    
    // 检查队列是否已满
    if (queue->size >= queue->capacity) {
        pthread_mutex_unlock(&queue->mutex);
        return -EAGAIN;
    }
    
    // 将请求加入队列
    queue->entries[queue->tail] = req;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;
    
    // 唤醒等待的线程
    pthread_cond_signal(&queue->cond);
    
    pthread_mutex_unlock(&queue->mutex);
    
    return rc;
}

/**
 * 出队请求
 *
 * @param queue 队列指针
 * @param req_out 输出参数，接收出队的请求
 * @param wait 是否等待队列有请求
 * @return 成功返回0，队列空且不等待返回-EAGAIN，其他错误返回相应错误码
 */
static int dequeue_request(xbdev_queue_t *queue, xbdev_request_t **req_out, bool wait)
{
    int rc = 0;
    
    if (!queue || !req_out) {
        return -EINVAL;
    }
    
    *req_out = NULL;
    
    pthread_mutex_lock(&queue->mutex);
    
    // 如果队列为空且需要等待
    while (queue->size == 0 && wait) {
        rc = pthread_cond_wait(&queue->cond, &queue->mutex);
        if (rc != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return -rc;
        }
    }
    
    // 检查队列是否为空
    if (queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return -EAGAIN;
    }
    
    // 取出请求
    *req_out = queue->entries[queue->head];
    queue->entries[queue->head] = NULL;
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    
    pthread_mutex_unlock(&queue->mutex);
    
    return 0;
}

/**
 * 初始化请求池
 *
 * @param pool 池指针
 * @param capacity 池容量
 * @return 成功返回0，失败返回错误码
 */
static int init_request_pool(xbdev_request_pool_t *pool, uint32_t capacity)
{
    int rc;
    
    if (!pool || capacity == 0) {
        return -EINVAL;
    }
    
    // 初始化互斥锁
    rc = pthread_mutex_init(&pool->mutex, NULL);
    if (rc != 0) {
        return -rc;
    }
    
    // 分配请求数组
    pool->entries = calloc(capacity, sizeof(xbdev_request_t));
    if (!pool->entries) {
        pthread_mutex_destroy(&pool->mutex);
        return -ENOMEM;
    }
    
    // 分配使用状态数组
    pool->used = calloc(capacity, sizeof(bool));
    if (!pool->used) {
        free(pool->entries);
        pthread_mutex_destroy(&pool->mutex);
        return -ENOMEM;
    }
    
    // 初始化池状态
    pool->capacity = capacity;
    pool->used_count = 0;
    pool->initialized = true;
    
    // 初始化每个请求
    for (uint32_t i = 0; i < capacity; i++) {
        pool->entries[i].req_id = 0;
        pool->entries[i].status = XBDEV_REQ_STATUS_INIT;
        pool->entries[i].result = 0;
        pool->entries[i].sync_req = false;
        pool->entries[i].done = false;
        pool->entries[i].ctx = NULL;
        pool->entries[i].cb = NULL;
        pool->entries[i].cb_arg = NULL;
        pool->entries[i].submit_tsc = 0;
        pool->entries[i].complete_tsc = 0;
    }
    
    return 0;
}

/**
 * 销毁请求池
 *
 * @param pool 池指针
 */
static void destroy_request_pool(xbdev_request_pool_t *pool)
{
    if (!pool || !pool->initialized) {
        return;
    }
    
    pthread_mutex_lock(&pool->mutex);
    
    // 检查是否所有请求都已返回
    if (pool->used_count > 0) {
        XBDEV_WARNLOG("销毁请求池时有%u个请求未返回\n", pool->used_count);
    }
    
    // 释放资源
    free(pool->entries);
    free(pool->used);
    pool->entries = NULL;
    pool->used = NULL;
    
    pthread_mutex_unlock(&pool->mutex);
    
    // 销毁互斥锁
    pthread_mutex_destroy(&pool->mutex);
    
    // 重置池状态
    pool->capacity = 0;
    pool->used_count = 0;
    pool->initialized = false;
}

/**
 * 从请求池分配请求
 *
 * @param sync 是否为同步请求
 * @return 成功返回请求指针，失败返回NULL
 */
static xbdev_request_t *alloc_request_from_pool(bool sync)
{
    xbdev_request_t *req = NULL;
    
    if (!g_queue_system.initialized) {
        return NULL;
    }
    
    pthread_mutex_lock(&g_queue_system.req_pool.mutex);
    
    // 查找空闲请求
    for (uint32_t i = 0; i < g_queue_system.req_pool.capacity; i++) {
        if (!g_queue_system.req_pool.used[i]) {
            // 分配请求
            g_queue_system.req_pool.used[i] = true;
            req = &g_queue_system.req_pool.entries[i];
            g_queue_system.req_pool.used_count++;
            
            // 初始化请求
            req->req_id = g_queue_system.next_req_id++;
            req->status = XBDEV_REQ_STATUS_INIT;
            req->result = 0;
            req->sync_req = sync;
            req->done = false;
            req->ctx = NULL;
            req->cb = NULL;
            req->cb_arg = NULL;
            req->submit_tsc = 0;
            req->complete_tsc = 0;
            
            break;
        }
    }
    
    pthread_mutex_unlock(&g_queue_system.req_pool.mutex);
    
    return req;
}

/**
 * 返回请求到池
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
static int free_request_to_pool(xbdev_request_t *req)
{
    uint32_t idx;
    
    if (!g_queue_system.initialized || !req) {
        return -EINVAL;
    }
    
    // 计算请求在数组中的索引
    idx = req - g_queue_system.req_pool.entries;
    
    // 检查索引是否有效
    if (idx >= g_queue_system.req_pool.capacity) {
        XBDEV_ERRLOG("无效的请求指针\n");
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_queue_system.req_pool.mutex);
    
    // 检查请求是否已被标记为使用中
    if (!g_queue_system.req_pool.used[idx]) {
        XBDEV_ERRLOG("请求未被标记为使用中\n");
        pthread_mutex_unlock(&g_queue_system.req_pool.mutex);
        return -EINVAL;
    }
    
    // 释放请求
    g_queue_system.req_pool.used[idx] = false;
    g_queue_system.req_pool.used_count--;
    
    pthread_mutex_unlock(&g_queue_system.req_pool.mutex);
    
    return 0;
}

/**
 * 获取请求队列索引
 *
 * @return 队列索引
 */
static uint32_t get_queue_index(void)
{
    // 简单的轮询策略，可以根据需要优化为NUMA感知或线程ID模式
    static __thread uint32_t thread_queue_idx = 0;
    
    if (thread_queue_idx == 0) {
        thread_queue_idx = (uint32_t)(pthread_self() % g_queue_system.num_queues);
    }
    
    return thread_queue_idx;
}

/**
 * 初始化请求队列系统
 *
 * @param queue_capacity 每个队列的容量
 * @param num_queues 队列数量
 * @param pool_capacity 请求池容量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_queue_system_init(uint32_t queue_capacity, uint32_t num_queues, uint32_t pool_capacity)
{
    int rc;
    
    if (g_queue_system.initialized) {
        return -EALREADY;
    }
    
    if (queue_capacity == 0 || num_queues == 0 || pool_capacity == 0) {
        return -EINVAL;
    }
    
    // 分配请求队列数组
    g_queue_system.req_queues = calloc(num_queues, sizeof(xbdev_queue_t));
    if (!g_queue_system.req_queues) {
        return -ENOMEM;
    }
    
    // 初始化每个请求队列
    for (uint32_t i = 0; i < num_queues; i++) {
        rc = init_queue(&g_queue_system.req_queues[i], queue_capacity);
        if (rc != 0) {
            for (uint32_t j = 0; j < i; j++) {
                destroy_queue(&g_queue_system.req_queues[j]);
            }
            free(g_queue_system.req_queues);
            g_queue_system.req_queues = NULL;
            return rc;
        }
    }
    
    // 初始化请求池
    rc = init_request_pool(&g_queue_system.req_pool, pool_capacity);
    if (rc != 0) {
        for (uint32_t i = 0; i < num_queues; i++) {
            destroy_queue(&g_queue_system.req_queues[i]);
        }
        free(g_queue_system.req_queues);
        g_queue_system.req_queues = NULL;
        return rc;
    }
    
    // 初始化全局状态
    g_queue_system.num_queues = num_queues;
    g_queue_system.next_req_id = 1;
    g_queue_system.initialized = true;
    
    return 0;
}

/**
 * 销毁请求队列系统
 */
void xbdev_queue_system_fini(void)
{
    if (!g_queue_system.initialized) {
        return;
    }
    
    // 销毁请求池
    destroy_request_pool(&g_queue_system.req_pool);
    
    // 销毁每个请求队列
    for (uint32_t i = 0; i < g_queue_system.num_queues; i++) {
        destroy_queue(&g_queue_system.req_queues[i]);
    }
    
    // 释放请求队列数组
    free(g_queue_system.req_queues);
    g_queue_system.req_queues = NULL;
    
    // 重置全局状态
    g_queue_system.num_queues = 0;
    g_queue_system.next_req_id = 0;
    g_queue_system.initialized = false;
}

/**
 * 提交请求到队列
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_submit(xbdev_request_t *req)
{
    if (!req) {
        return -EINVAL;
    }
    
    if (!g_queue_system.initialized) {
        return -EINVAL;
    }
    
    uint32_t queue_idx = get_queue_index();
    return enqueue_request(&g_queue_system.req_queues[queue_idx], req);
}

/**
 * 从请求队列获取请求
 *
 * @param req_out 输出参数，接收出队的请求
 * @param wait 是否等待队列有请求
 * @return 成功返回0，队列空且不等待返回-EAGAIN，其他错误返回相应错误码
 */
int xbdev_request_dequeue(xbdev_request_t **req_out, bool wait)
{
    if (!req_out) {
        return -EINVAL;
    }
    
    if (!g_queue_system.initialized) {
        return -EINVAL;
    }
    
    uint32_t queue_idx = get_queue_index();
    return dequeue_request(&g_queue_system.req_queues[queue_idx], req_out, wait);
}

/**
 * 分配一个请求结构体
 *
 * @return 成功返回请求指针，失败返回NULL
 */
xbdev_request_t *xbdev_request_alloc(void)
{
    return alloc_request_from_pool(false);
}

/**
 * 分配一个同步请求结构体
 *
 * @return 成功返回请求指针，失败返回NULL
 */
xbdev_request_t *xbdev_sync_request_alloc(void)
{
    return alloc_request_from_pool(true);
}

/**
 * 释放一个请求结构体
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_free(xbdev_request_t *req)
{
    return free_request_to_pool(req);
}

/**
 * 执行同步请求并等待完成
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_sync_request_execute(xbdev_request_t *req)
{
    if (!req) {
        return -EINVAL;
    }
    
    // 提交请求
    int rc = xbdev_request_submit(req);
    if (rc != 0) {
        return rc;
    }
    
    // 等待请求完成
    pthread_mutex_lock(&g_queue_system.req_pool.mutex);
    while (!req->done) {
        pthread_cond_wait(&g_queue_system.req_pool.cond, &g_queue_system.req_pool.mutex);
    }
    pthread_mutex_unlock(&g_queue_system.req_pool.mutex);
    
    return req->result;
}

/**
 * 处理请求队列
 * 该函数应在SPDK线程上下文中调用
 */
void xbdev_process_request_queues(void)
{
    if (!g_queue_system.initialized) {
        return;
    }
    
    xbdev_request_t *req;
    
    // 遍历所有请求队列
    for (uint32_t i = 0; i < g_queue_system.num_queues; i++) {
        while (dequeue_request(&g_queue_system.req_queues[i], &req, false) == 0) {
            // 处理请求
            switch (req->type) {
                case XBDEV_REQ_OPEN:
                    xbdev_open_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_CLOSE:
                    xbdev_close_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_READ:
                    xbdev_read_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_WRITE:
                    xbdev_write_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_READV:
                    xbdev_readv_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_WRITEV:
                    xbdev_writev_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_FLUSH:
                    xbdev_flush_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_UNMAP:
                    xbdev_unmap_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_RESET:
                    xbdev_reset_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_REGISTER_BDEV:
                    xbdev_register_bdev_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_REMOVE_BDEV:
                    xbdev_device_remove_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_RAID_CREATE:
                    xbdev_md_dispatch_request(req);
                    break;
                case XBDEV_REQ_RAID_STOP:
                case XBDEV_REQ_RAID_ASSEMBLE:
                case XBDEV_REQ_RAID_EXAMINE:
                case XBDEV_REQ_RAID_DETAIL:
                case XBDEV_REQ_RAID_MANAGE:
                case XBDEV_REQ_RAID_REPLACE_DISK:
                case XBDEV_REQ_RAID_REBUILD_CONTROL:
                case XBDEV_REQ_RAID_SET_DISK_STATE:
                    xbdev_md_dispatch_request(req);
                    break;
                case XBDEV_REQ_LVOL_CREATE_POOL:
                    lvol_create_pool_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_DESTROY_POOL:
                    lvol_destroy_pool_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_CREATE:
                    lvol_create_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_DESTROY:
                    lvol_destroy_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_SNAPSHOT:
                    lvol_create_snapshot_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_CLONE:
                    lvol_create_clone_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_RENAME:
                    lvol_rename_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_RESIZE:
                    lvol_resize_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_GET_INFO:
                    lvol_get_info_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_GET_POOL_INFO:
                    lvol_get_pool_info_on_thread(req->ctx);
                    break;
                default:
                    XBDEV_ERRLOG("未知的请求类型: %d\n", req->type);
                    break;
            }
            
            // 如果是同步请求，通知等待的线程
            if (req->sync_req) {
                pthread_mutex_lock(&g_queue_system.req_pool.mutex);
                req->done = true;
                pthread_cond_broadcast(&g_queue_system.req_pool.cond);
                pthread_mutex_unlock(&g_queue_system.req_pool.mutex);
            }
        }
    }
}

/**
 * 获取请求队列索引
 *
 * @return 队列索引
 */
static uint32_t get_queue_index(void)
{
    // 简单的轮询策略，可以根据需要优化为NUMA感知或线程ID模式
    static __thread uint32_t thread_queue_idx = 0;
    
    if (thread_queue_idx == 0) {
        thread_queue_idx = (uint32_t)(pthread_self() % g_queue_system.num_queues);
    }
    
    return thread_queue_idx;
}

/**
 * 初始化请求队列系统
 *
 * @param queue_size 每个队列的容量
 * @param num_queues 队列数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_queue_init(uint32_t queue_size, uint32_t num_queues)
{
    int rc;
    
    if (g_queue_system.initialized) {
        return -EALREADY;
    }
    
    if (queue_size == 0 || num_queues == 0) {
        return -EINVAL;
    }
    
    // 分配请求队列数组
    g_queue_system.req_queues = calloc(num_queues, sizeof(xbdev_queue_t));
    if (!g_queue_system.req_queues) {
        return -ENOMEM;
    }
    
    // 初始化每个请求队列
    for (uint32_t i = 0; i < num_queues; i++) {
        rc = init_queue(&g_queue_system.req_queues[i], queue_size);
        if (rc != 0) {
            for (uint32_t j = 0; j < i; j++) {
                destroy_queue(&g_queue_system.req_queues[j]);
            }
            free(g_queue_system.req_queues);
            g_queue_system.req_queues = NULL;
            return rc;
        }
    }
    
    // 确定请求池大小 - 至少是所有队列容量的2倍
    uint32_t pool_size = queue_size * num_queues * 2;
    
    // 初始化请求池
    rc = init_request_pool(&g_queue_system.req_pool, pool_size);
    if (rc != 0) {
        for (uint32_t i = 0; i < num_queues; i++) {
            destroy_queue(&g_queue_system.req_queues[i]);
        }
        free(g_queue_system.req_queues);
        g_queue_system.req_queues = NULL;
        return rc;
    }
    
    // 初始化全局状态
    g_queue_system.num_queues = num_queues;
    g_queue_system.next_req_id = 1;
    g_queue_system.initialized = true;
    
    XBDEV_NOTICELOG("请求队列系统初始化完成: %u队列，每队列%u容量，总池容量%u\n", 
                  num_queues, queue_size, pool_size);
    
    return 0;
}

/**
 * 销毁请求队列系统
 */
void xbdev_queue_fini(void)
{
    if (!g_queue_system.initialized) {
        return;
    }
    
    // 销毁请求池
    destroy_request_pool(&g_queue_system.req_pool);
    
    // 销毁每个请求队列
    for (uint32_t i = 0; i < g_queue_system.num_queues; i++) {
        destroy_queue(&g_queue_system.req_queues[i]);
    }
    
    // 释放请求队列数组
    free(g_queue_system.req_queues);
    g_queue_system.req_queues = NULL;
    
    // 重置全局状态
    g_queue_system.num_queues = 0;
    g_queue_system.next_req_id = 0;
    g_queue_system.initialized = false;
    
    XBDEV_NOTICELOG("请求队列系统已清理\n");
}

/**
 * 提交请求到队列
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_submit(xbdev_request_t *req)
{
    int rc;
    
    if (!req) {
        return -EINVAL;
    }
    
    if (!g_queue_system.initialized) {
        return -EINVAL;
    }
    
    // 记录提交时间戳
    req->submit_tsc = spdk_get_ticks();
    
    // 设置请求状态为已提交
    req->status = XBDEV_REQ_STATUS_SUBMITTED;
    
    // 提交到队列
    uint32_t queue_idx = get_queue_index();
    rc = enqueue_request(&g_queue_system.req_queues[queue_idx], req);
    
    if (rc != 0) {
        XBDEV_ERRLOG("请求入队失败: req_id=%lu, rc=%d\n", req->req_id, rc);
    }
    
    return rc;
}

/**
 * 等待请求完成，带超时控制
 *
 * @param req 请求指针
 * @param timeout_us 超时时间(微秒)，0表示永不超时
 * @return 成功返回0，超时返回-ETIMEDOUT，其他错误返回对应错误码
 */
int xbdev_request_wait(xbdev_request_t *req, uint64_t timeout_us)
{
    if (!req) {
        return -EINVAL;
    }
    
    if (!g_queue_system.initialized) {
        return -EINVAL;
    }
    
    // 检查是否已经完成
    if (req->done) {
        return 0;
    }
    
    struct timespec timeout;
    struct timespec *timeout_ptr = NULL;
    
    // 如果设置了超时时间，计算绝对超时时间
    if (timeout_us > 0) {
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += timeout_us / 1000000;
        timeout.tv_nsec += (timeout_us % 1000000) * 1000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }
        timeout_ptr = &timeout;
    }
    
    // 等待请求完成
    pthread_mutex_lock(&g_queue_system.req_pool.mutex);
    
    int rc = 0;
    while (!req->done && rc == 0) {
        if (timeout_ptr) {
            rc = pthread_cond_timedwait(&g_queue_system.req_pool.cond, 
                                      &g_queue_system.req_pool.mutex, 
                                      timeout_ptr);
        } else {
            rc = pthread_cond_wait(&g_queue_system.req_pool.cond, 
                                 &g_queue_system.req_pool.mutex);
        }
    }
    
    pthread_mutex_unlock(&g_queue_system.req_pool.mutex);
    
    if (rc == ETIMEDOUT) {
        XBDEV_WARNLOG("请求等待超时: req_id=%lu\n", req->req_id);
        return -ETIMEDOUT;
    } else if (rc != 0) {
        XBDEV_ERRLOG("请求等待错误: req_id=%lu, rc=%d\n", req->req_id, rc);
        return -rc;
    }
    
    return 0;
}

/**
 * 执行同步请求并等待完成
 *
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_sync_request_execute(xbdev_request_t *req)
{
    if (!req) {
        return -EINVAL;
    }
    
    if (!req->sync_req) {
        return -EINVAL;
    }
    
    // 提交请求
    int rc = xbdev_request_submit(req);
    if (rc != 0) {
        return rc;
    }
    
    // 等待请求完成
    rc = xbdev_request_wait(req, XBDEV_SYNC_IO_TIMEOUT_US);
    if (rc != 0) {
        return rc;
    }
    
    // 返回请求结果
    return req->result;
}

/**
 * 创建带唤醒机制的完成通知
 * 
 * @param req 请求指针
 * @param status 完成状态
 * @param done 完成标志指针
 */
void xbdev_mark_request_complete(xbdev_request_t *req, int status, bool *done)
{
    if (!req) {
        return;
    }
    
    // 记录完成时间
    req->complete_tsc = spdk_get_ticks();
    
    // 设置结果和状态
    req->result = status;
    req->status = (status == 0) ? XBDEV_REQ_STATUS_COMPLETED : XBDEV_REQ_STATUS_ERROR;
    
    // 如果是同步请求，通知等待线程
    if (req->sync_req) {
        pthread_mutex_lock(&g_queue_system.req_pool.mutex);
        req->done = true;
        if (done) *done = true;
        pthread_cond_broadcast(&g_queue_system.req_pool.cond);
        pthread_mutex_unlock(&g_queue_system.req_pool.mutex);
    } else {
        req->done = true;
        if (done) *done = true;
        
        // 如果有回调，则调用它
        if (req->cb) {
            req->cb(req->result, req->cb_arg);
        }
    }
}

/**
 * 批量处理请求(高性能模式)
 * 
 * @param reqs 请求数组
 * @param num_reqs 请求数量
 * @return 成功返回处理的请求数，失败返回错误码
 */
int xbdev_batch_requests(xbdev_request_t **reqs, int num_reqs)
{
    if (!reqs || num_reqs <= 0) {
        return -EINVAL;
    }
    
    if (!g_queue_system.initialized) {
        return -EINVAL;
    }
    
    uint32_t queue_idx = get_queue_index();
    xbdev_queue_t *queue = &g_queue_system.req_queues[queue_idx];
    int submitted = 0;
    
    // 批量锁定队列
    pthread_mutex_lock(&queue->mutex);
    
    // 检查队列容量是否足够
    if (queue->size + num_reqs > queue->capacity) {
        pthread_mutex_unlock(&queue->mutex);
        XBDEV_ERRLOG("队列空间不足,无法批量提交%d个请求\n", num_reqs);
        return -EAGAIN;
    }
    
    // 批量入队
    for (int i = 0; i < num_reqs; i++) {
        xbdev_request_t *req = reqs[i];
        if (!req) continue;
        
        // 记录提交时间戳
        req->submit_tsc = spdk_get_ticks();
        
        // 设置请求状态为已提交
        req->status = XBDEV_REQ_STATUS_SUBMITTED;
        
        // 放入队列
        queue->entries[queue->tail] = req;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->size++;
        submitted++;
    }
    
    // 发送信号唤醒处理线程
    pthread_cond_signal(&queue->cond);
    
    // 解锁队列
    pthread_mutex_unlock(&queue->mutex);
    
    return submitted;
}

/**
 * 获取请求队列统计信息
 * 
 * @param stats 输出参数，统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_queue_stats(struct xbdev_queue_stats *stats)
{
    if (!stats) {
        return -EINVAL;
    }
    
    if (!g_queue_system.initialized) {
        return -EINVAL;
    }
    
    memset(stats, 0, sizeof(*stats));
    
    // 收集统计信息
    stats->num_queues = g_queue_system.num_queues;
    stats->request_pool_capacity = g_queue_system.req_pool.capacity;
    stats->request_pool_used = g_queue_system.req_pool.used_count;
    
    // 统计所有队列的容量和使用情况
    for (uint32_t i = 0; i < g_queue_system.num_queues; i++) {
        xbdev_queue_t *queue = &g_queue_system.req_queues[i];
        pthread_mutex_lock(&queue->mutex);
        stats->queue_capacity += queue->capacity;
        stats->queue_used += queue->size;
        pthread_mutex_unlock(&queue->mutex);
    }
    
    // 计算平均使用率
    if (stats->queue_capacity > 0) {
        stats->queue_usage_percent = (stats->queue_used * 100) / stats->queue_capacity;
    }
    
    if (stats->request_pool_capacity > 0) {
        stats->pool_usage_percent = (stats->request_pool_used * 100) / stats->request_pool_capacity;
    }
    
    return 0;
}

/**
 * 处理下一个请求(单个)
 * 该函数从队列中取出一个请求并处理它
 * 
 * @return 处理了请求返回true，队列为空返回false
 */
bool xbdev_process_next_request(void)
{
    if (!g_queue_system.initialized) {
        return false;
    }
    
    xbdev_request_t *req = NULL;
    int rc;
    bool processed = false;
    
    // 遍历所有队列，寻找请求
    for (uint32_t i = 0; i < g_queue_system.num_queues; i++) {
        rc = dequeue_request(&g_queue_system.req_queues[i], &req, false);
        if (rc == 0 && req != NULL) {
            // 找到请求，处理它
            processed = true;
            req->status = XBDEV_REQ_STATUS_PROCESSING;
            
            // 根据请求类型调度到对应处理函数
            switch (req->type) {
                case XBDEV_REQ_OPEN:
                    xbdev_open_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_CLOSE:
                    xbdev_close_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_READ:
                    xbdev_read_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_WRITE:
                    xbdev_write_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_READV:
                    xbdev_readv_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_WRITEV:
                    xbdev_writev_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_FLUSH:
                    xbdev_flush_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_UNMAP:
                    xbdev_unmap_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_RESET:
                    xbdev_reset_on_thread(req->ctx);
                    break;
                    
                // 注册和移除设备
                case XBDEV_REQ_REGISTER_BDEV:
                    xbdev_register_bdev_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_REMOVE_BDEV:
                    xbdev_device_remove_on_thread(req->ctx);
                    break;
                    
                // RAID相关请求
                case XBDEV_REQ_RAID_CREATE:
                case XBDEV_REQ_RAID_STOP:
                case XBDEV_REQ_RAID_ASSEMBLE:
                case XBDEV_REQ_RAID_EXAMINE:
                case XBDEV_REQ_RAID_DETAIL:
                case XBDEV_REQ_RAID_MANAGE:
                case XBDEV_REQ_RAID_REPLACE_DISK:
                case XBDEV_REQ_RAID_REBUILD_CONTROL:
                case XBDEV_REQ_RAID_SET_DISK_STATE:
                    xbdev_md_dispatch_request(req);
                    break;
                    
                // LVOL相关请求
                case XBDEV_REQ_LVOL_CREATE_POOL:
                    lvol_create_pool_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_DESTROY_POOL:
                    lvol_destroy_pool_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_CREATE:
                    lvol_create_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_DESTROY:
                    lvol_destroy_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_SNAPSHOT:
                    lvol_create_snapshot_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_CLONE:
                    lvol_create_clone_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_RENAME:
                    lvol_rename_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_RESIZE:
                    lvol_resize_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_GET_INFO:
                    lvol_get_info_on_thread(req->ctx);
                    break;
                case XBDEV_REQ_LVOL_GET_POOL_INFO:
                    lvol_get_pool_info_on_thread(req->ctx);
                    break;
                    
                // 自定义请求
                case XBDEV_REQ_CUSTOM:
                    // 对于自定义请求，我们假设处理函数已经由ctx中的函数指针提供
                    if (req->ctx != NULL) {
                        // 调用自定义处理函数
                        void (*custom_fn)(void*) = (void(*)(void*))req->cb;
                        if (custom_fn) {
                            custom_fn(req->ctx);
                        } else {
                            XBDEV_WARNLOG("自定义请求没有处理函数: req_id=%lu\n", req->req_id);
                        }
                    }
                    break;
                    
                default:
                    XBDEV_ERRLOG("未知的请求类型: %d, req_id=%lu\n", req->type, req->req_id);
                    break;
            }
            
            // 如果是同步请求，标记为完成并通知等待线程
            if (req->sync_req) {
                xbdev_mark_request_complete(req, req->result, NULL);
            }
            
            break; // 处理了一个请求后退出循环
        }
    }
    
    return processed;
}

/**
 * 处理请求队列，批量处理一定数量的请求
 * 该函数应在SPDK线程上下文中调用
 */
void xbdev_process_request_queues(void)
{
    if (!g_queue_system.initialized) {
        return;
    }
    
    // 每次调用最多处理16个请求，避免长时间阻塞SPDK线程
    int max_requests = 16;
    int processed = 0;
    
    while (processed < max_requests) {
        if (!xbdev_process_next_request()) {
            break; // 没有更多请求可处理
        }
        processed++;
    }
}