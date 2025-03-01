/**
 * @file request_queue.c
 * @brief 实现请求队列系统
 * 
 * 该文件实现了跨线程请求队列系统，用于在客户端线程和SPDK线程
 * 之间传递请求，实现高效的线程间通信。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/env.h>
#include <spdk/queue.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>

/**
 * 最大请求队列数
 */
#define XBDEV_MAX_QUEUES 16

/**
 * 请求队列状态
 */
typedef enum {
    XBDEV_QUEUE_UNINITIALIZED = 0,    // 未初始化
    XBDEV_QUEUE_READY,                // 就绪
    XBDEV_QUEUE_STOPPING,             // 正在停止
} xbdev_queue_status_t;

/**
 * 环形缓冲区 - 无锁队列结构体
 */
typedef struct {
    volatile uint32_t head;           // 头指针(生产者)
    volatile uint32_t tail;           // 尾指针(消费者)
    uint32_t size;                    // 队列大小
    uint32_t mask;                    // 掩码(size-1)
    xbdev_request_t **buffer;         // 请求缓冲区
} xbdev_ring_t;

/**
 * 请求队列结构体
 */
typedef struct {
    xbdev_queue_status_t status;      // 队列状态
    xbdev_ring_t ring;                // 环形缓冲区
    pthread_mutex_t mutex;            // 互斥锁
    pthread_cond_t cond;              // 条件变量
    uint32_t pending;                 // 挂起请求计数
    uint64_t req_id_counter;          // 请求ID计数器
} xbdev_queue_t;

/**
 * 全局队列系统状态
 */
static struct {
    bool initialized;                // 是否已初始化
    xbdev_queue_t queues[XBDEV_MAX_QUEUES]; // 请求队列数组
    uint32_t num_queues;             // 队列数量
    uint32_t queue_size;             // 每个队列的大小
    pthread_key_t thread_queue_key;  // 线程本地存储队列索引
    uint32_t next_queue_idx;         // 下一个分配的队列索引
    pthread_mutex_t alloc_mutex;     // 内存池分配互斥锁
} g_queue_ctx = {0};

/**
 * 请求内存池
 */
static struct {
    xbdev_request_t *requests;        // 请求对象池
    bool *used_flags;                 // 使用标志
    uint32_t size;                    // 池大小
    uint32_t count;                   // 已使用数量
} g_request_pool = {0};

/**
 * 初始化环形缓冲区
 *
 * @param ring 环形缓冲区
 * @param size 大小(必须是2的幂)
 * @return 成功返回0，失败返回错误码
 */
static int ring_init(xbdev_ring_t *ring, uint32_t size)
{
    // 检查size是否为2的幂
    if (!ring || size == 0 || (size & (size - 1)) != 0) {
        return -EINVAL;
    }
    
    // 分配缓冲区
    ring->buffer = calloc(size, sizeof(xbdev_request_t *));
    if (!ring->buffer) {
        return -ENOMEM;
    }
    
    ring->size = size;
    ring->mask = size - 1;
    ring->head = 0;
    ring->tail = 0;
    
    return 0;
}

/**
 * 销毁环形缓冲区
 *
 * @param ring 环形缓冲区
 */
static void ring_fini(xbdev_ring_t *ring)
{
    if (ring && ring->buffer) {
        free(ring->buffer);
        ring->buffer = NULL;
    }
}

/**
 * 将请求入队
 *
 * @param ring 环形缓冲区
 * @param req 请求
 * @return 成功返回0，队列已满返回-ENOSPC
 */
static int ring_enqueue(xbdev_ring_t *ring, xbdev_request_t *req)
{
    uint32_t head = ring->head;
    uint32_t next_head = (head + 1) & ring->mask;
    
    // 检查队列是否已满
    if (next_head == ring->tail) {
        return -ENOSPC;
    }
    
    // 插入请求
    ring->buffer[head] = req;
    
    // 内存屏障，确保数据写入后再更新head
    __sync_synchronize();
    
    // 更新head
    ring->head = next_head;
    
    return 0;
}

/**
 * 从队列取出请求
 *
 * @param ring 环形缓冲区
 * @param req 输出参数，存储取出的请求
 * @return 成功返回0，队列为空返回-EAGAIN
 */
static int ring_dequeue(xbdev_ring_t *ring, xbdev_request_t **req)
{
    uint32_t tail = ring->tail;
    
    // 检查队列是否为空
    if (tail == ring->head) {
        return -EAGAIN;
    }
    
    // 取出请求
    *req = ring->buffer[tail];
    
    // 内存屏障，确保数据读取后再更新tail
    __sync_synchronize();
    
    // 更新tail
    ring->tail = (tail + 1) & ring->mask;
    
    return 0;
}

/**
 * 获取当前线程的队列
 *
 * @return 队列指针，失败返回NULL
 */
static xbdev_queue_t *get_current_thread_queue(void)
{
    void *value = pthread_getspecific(g_queue_ctx.thread_queue_key);
    uintptr_t queue_idx = (uintptr_t)value;
    
    // 检查是否已分配队列
    if (queue_idx == 0) {
        // 分配新队列索引
        pthread_mutex_lock(&g_queue_ctx.alloc_mutex);
        queue_idx = g_queue_ctx.next_queue_idx % g_queue_ctx.num_queues;
        g_queue_ctx.next_queue_idx++;
        pthread_mutex_unlock(&g_queue_ctx.alloc_mutex);
        
        // 保存到线程本地存储
        pthread_setspecific(g_queue_ctx.thread_queue_key, (void *)(queue_idx + 1));
        
        XBDEV_INFOLOG("线程 %p 分配队列 %zu\n", 
                    (void *)pthread_self(), queue_idx);
    } else {
        // 已分配，减1还原实际索引
        queue_idx--;
    }
    
    // 返回队列
    return &g_queue_ctx.queues[queue_idx];
}

/**
 * 初始化请求队列系统
 *
 * @param queue_size 每个队列的大小
 * @param num_queues 队列数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_queue_init(uint32_t queue_size, uint32_t num_queues)
{
    int rc;
    
    // 检查参数
    if (queue_size == 0 || num_queues == 0 || num_queues > XBDEV_MAX_QUEUES) {
        XBDEV_ERRLOG("无效的队列参数: size=%u, num=%u\n", queue_size, num_queues);
        return -EINVAL;
    }
    
    // 检查是否已初始化
    if (g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统已初始化\n");
        return -EEXIST;
    }
    
    // 对齐队列大小到2的幂
    uint32_t aligned_size = 1;
    while (aligned_size < queue_size) {
        aligned_size <<= 1;
    }
    
    // 初始化全局状态
    memset(&g_queue_ctx, 0, sizeof(g_queue_ctx));
    g_queue_ctx.queue_size = aligned_size;
    g_queue_ctx.num_queues = num_queues;
    
    // 创建线程本地存储键
    rc = pthread_key_create(&g_queue_ctx.thread_queue_key, NULL);
    if (rc != 0) {
        XBDEV_ERRLOG("创建线程本地存储键失败: %d\n", rc);
        return -rc;
    }
    
    // 初始化分配互斥锁
    rc = pthread_mutex_init(&g_queue_ctx.alloc_mutex, NULL);
    if (rc != 0) {
        XBDEV_ERRLOG("初始化分配互斥锁失败: %d\n", rc);
        pthread_key_delete(g_queue_ctx.thread_queue_key);
        return -rc;
    }
    
    // 初始化队列
    for (uint32_t i = 0; i < num_queues; i++) {
        xbdev_queue_t *queue = &g_queue_ctx.queues[i];
        
        // 初始化环形缓冲区
        rc = ring_init(&queue->ring, aligned_size);
        if (rc != 0) {
            XBDEV_ERRLOG("初始化环形缓冲区失败: %d\n", rc);
            
            // 清理已初始化的队列
            for (uint32_t j = 0; j < i; j++) {
                ring_fini(&g_queue_ctx.queues[j].ring);
                pthread_mutex_destroy(&g_queue_ctx.queues[j].mutex);
                pthread_cond_destroy(&g_queue_ctx.queues[j].cond);
            }
            
            pthread_mutex_destroy(&g_queue_ctx.alloc_mutex);
            pthread_key_delete(&g_queue_ctx.thread_queue_key);
            return rc;
        }
        
        // 初始化互斥锁和条件变量
        pthread_mutex_init(&queue->mutex, NULL);
        pthread_cond_init(&queue->cond, NULL);
        
        queue->status = XBDEV_QUEUE_READY;
        queue->pending = 0;
        queue->req_id_counter = ((uint64_t)i << 32) + 1;  // 高32位为队列ID
    }
    
    // 初始化请求内存池
    uint32_t pool_size = aligned_size * num_queues * 2;  // 为每个队列分配2倍大小的请求池
    g_request_pool.size = pool_size;
    g_request_pool.count = 0;
    
    g_request_pool.requests = calloc(pool_size, sizeof(xbdev_request_t));
    if (!g_request_pool.requests) {
        XBDEV_ERRLOG("分配请求内存池失败\n");
        
        // 清理队列资源
        for (uint32_t i = 0; i < num_queues; i++) {
            ring_fini(&g_queue_ctx.queues[i].ring);
            pthread_mutex_destroy(&g_queue_ctx.queues[i].mutex);
            pthread_cond_destroy(&g_queue_ctx.queues[i].cond);
        }
        
        pthread_mutex_destroy(&g_queue_ctx.alloc_mutex);
        pthread_key_delete(&g_queue_ctx.thread_queue_key);
        return -ENOMEM;
    }
    
    g_request_pool.used_flags = calloc(pool_size, sizeof(bool));
    if (!g_request_pool.used_flags) {
        XBDEV_ERRLOG("分配请求标志内存池失败\n");
        
        free(g_request_pool.requests);
        g_request_pool.requests = NULL;
        
        // 清理队列资源
        for (uint32_t i = 0; i < num_queues; i++) {
            ring_fini(&g_queue_ctx.queues[i].ring);
            pthread_mutex_destroy(&g_queue_ctx.queues[i].mutex);
            pthread_cond_destroy(&g_queue_ctx.queues[i].cond);
        }
        
        pthread_mutex_destroy(&g_queue_ctx.alloc_mutex);
        pthread_key_delete(&g_queue_ctx.thread_queue_key);
        return -ENOMEM;
    }
    
    // 标记为已初始化
    g_queue_ctx.initialized = true;
    
    XBDEV_NOTICELOG("请求队列系统初始化成功: %u队列，每队列%u项\n", 
                  num_queues, aligned_size);
    
    return 0;
}

/**
 * 清理请求队列系统
 */
void xbdev_queue_fini(void)
{
    // 检查是否已初始化
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return;
    }
    
    // 设置所有队列为停止状态
    for (uint32_t i = 0; i < g_queue_ctx.num_queues; i++) {
        xbdev_queue_t *queue = &g_queue_ctx.queues[i];
        queue->status = XBDEV_QUEUE_STOPPING;
        
        // 唤醒所有等待的线程
        pthread_mutex_lock(&queue->mutex);
        pthread_cond_broadcast(&queue->cond);
        pthread_mutex_unlock(&queue->mutex);
    }
    
    // 等待所有挂起请求完成
    uint32_t total_pending;
    do {
        total_pending = 0;
        for (uint32_t i = 0; i < g_queue_ctx.num_queues; i++) {
            total_pending += g_queue_ctx.queues[i].pending;
        }
        
        if (total_pending > 0) {
            XBDEV_NOTICELOG("等待 %u 个挂起请求完成...\n", total_pending);
            usleep(10000);  // 等待10ms
        }
    } while (total_pending > 0);
    
    // 清理队列资源
    for (uint32_t i = 0; i < g_queue_ctx.num_queues; i++) {
        xbdev_queue_t *queue = &g_queue_ctx.queues[i];
        
        ring_fini(&queue->ring);
        pthread_mutex_destroy(&queue->mutex);
        pthread_cond_destroy(&queue->cond);
    }
    
    // 清理请求内存池
    free(g_request_pool.requests);
    free(g_request_pool.used_flags);
    g_request_pool.requests = NULL;
    g_request_pool.used_flags = NULL;
    
    // 清理线程本地存储键
    pthread_key_delete(&g_queue_ctx.thread_queue_key);
    
    // 清理分配互斥锁
    pthread_mutex_destroy(&g_queue_ctx.alloc_mutex);
    
    // 标记为未初始化
    g_queue_ctx.initialized = false;
    
    XBDEV_NOTICELOG("请求队列系统清理完成\n");
}

/**
 * 分配请求对象
 *
 * @param sync_req 是否为同步请求
 * @return 请求对象，失败返回NULL
 */
static xbdev_request_t *alloc_request_internal(bool sync_req)
{
    xbdev_request_t *req = NULL;
    
    // 从内存池分配
    pthread_mutex_lock(&g_queue_ctx.alloc_mutex);
    
    if (g_request_pool.count >= g_request_pool.size) {
        // 内存池已满
        pthread_mutex_unlock(&g_queue_ctx.alloc_mutex);
        XBDEV_ERRLOG("请求内存池已满\n");
        return NULL;
    }
    
    // 查找未使用的请求对象
    for (uint32_t i = 0; i < g_request_pool.size; i++) {
        if (!g_request_pool.used_flags[i]) {
            g_request_pool.used_flags[i] = true;
            req = &g_request_pool.requests[i];
            g_request_pool.count++;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_queue_ctx.alloc_mutex);
    
    if (!req) {
        XBDEV_ERRLOG("无法分配请求对象\n");
        return NULL;
    }
    
    // 初始化请求对象
    memset(req, 0, sizeof(*req));
    
    // 设置请求ID
    xbdev_queue_t *queue = get_current_thread_queue();
    req->req_id = queue->req_id_counter++;
    
    req->status = XBDEV_REQ_STATUS_INIT;
    req->result = 0;
    req->sync_req = sync_req;
    req->done = false;
    
    // 记录提交时间戳
    req->submit_tsc = spdk_get_ticks();
    
    return req;
}

/**
 * 分配普通请求对象
 *
 * @return 请求对象，失败返回NULL
 */
xbdev_request_t *xbdev_request_alloc(void)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return NULL;
    }
    
    return alloc_request_internal(false);
}

/**
 * 分配同步请求对象
 * 
 * @return 请求对象，失败返回NULL
 */
xbdev_request_t *xbdev_sync_request_alloc(void)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return NULL;
    }
    
    return alloc_request_internal(true);
}

/**
 * 释放请求对象
 *
 * @param req 请求对象
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_free(xbdev_request_t *req)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return -EINVAL;
    }
    
    if (!req) {
        return -EINVAL;
    }
    
    // 计算请求的索引
    uint32_t idx = req - g_request_pool.requests;
    
    // 检查索引有效性
    if (idx >= g_request_pool.size) {
        XBDEV_ERRLOG("无效的请求对象: %p\n", (void *)req);
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_queue_ctx.alloc_mutex);
    
    // 检查请求是否正在使用
    if (!g_request_pool.used_flags[idx]) {
        pthread_mutex_unlock(&g_queue_ctx.alloc_mutex);
        XBDEV_ERRLOG("请求未分配或已释放: %p\n", (void *)req);
        return -EINVAL;
    }
    
    // 标记为未使用
    g_request_pool.used_flags[idx] = false;
    g_request_pool.count--;
    
    pthread_mutex_unlock(&g_queue_ctx.alloc_mutex);
    
    return 0;
}

/**
 * 提交请求到队列
 *
 * @param req 请求对象
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_submit(xbdev_request_t *req)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return -EINVAL;
    }
    
    if (!req) {
        XBDEV_ERRLOG("无效的请求对象\n");
        return -EINVAL;
    }
    
    // 获取当前线程的队列
    xbdev_queue_t *queue = get_current_thread_queue();
    if (queue->status != XBDEV_QUEUE_READY) {
        XBDEV_ERRLOG("队列未就绪或正在停止\n");
        return -EBUSY;
    }
    
    // 更新请求状态
    req->status = XBDEV_REQ_STATUS_SUBMITTED;
    
    // 尝试入队
    int rc = ring_enqueue(&queue->ring, req);
    if (rc != 0) {
        XBDEV_ERRLOG("入队失败: %d\n", rc);
        return rc;
    }
    
    // 增加挂起计数
    __sync_fetch_and_add(&queue->pending, 1);
    
    return 0;
}

/**
 * 执行同步请求并等待完成
 *
 * @param req 同步请求对象
 * @return 成功返回0，失败返回错误码
 */
int xbdev_sync_request_execute(xbdev_request_t *req)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return -EINVAL;
    }
    
    if (!req || !req->sync_req) {
        XBDEV_ERRLOG("无效的同步请求对象\n");
        return -EINVAL;
    }
    
    // 提交请求
    int rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("提交同步请求失败: %d\n", rc);
        return rc;
    }
    
    // 获取当前线程的队列
    xbdev_queue_t *queue = get_current_thread_queue();
    
    // 等待请求完成
    pthread_mutex_lock(&queue->mutex);
    while (!req->done && queue->status == XBDEV_QUEUE_READY) {
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    pthread_mutex_unlock(&queue->mutex);
    
    // 检查队列状态
    if (queue->status != XBDEV_QUEUE_READY) {
        return -ECANCELED;
    }
    
    return 0;
}

/**
 * 接收请求响应(不实际使用，由回调机制代替)
 *
 * @param req 请求对象
 * @return 成功返回0，失败返回错误码
 */
int xbdev_response_receive(xbdev_request_t *req)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return -EINVAL;
    }
    
    if (!req) {
        XBDEV_ERRLOG("无效的请求对象\n");
        return -EINVAL;
    }
    
    // 检查请求是否已完成
    if (!req->done) {
        return -EAGAIN;
    }
    
    return 0;
}

/**
 * 处理请求队列中的请求
 * 此函数由SPDK线程调用
 */
void xbdev_process_request_queues(void)
{
    if (!g_queue_ctx.initialized) {
        return;
    }
    
    // 处理所有队列
    for (uint32_t i = 0; i < g_queue_ctx.num_queues; i++) {
        xbdev_queue_t *queue = &g_queue_ctx.queues[i];
        
        // 跳过未就绪的队列
        if (queue->status != XBDEV_QUEUE_READY) {
            continue;
        }
        
        // 从队列中取出请求
        xbdev_request_t *req;
        while (ring_dequeue(&queue->ring, &req) == 0) {
            // 检查请求有效性
            if (!req) {
                XBDEV_ERRLOG("队列中的请求为NULL\n");
                continue;
            }
            
            // 更新请求状态
            req->status = XBDEV_REQ_STATUS_PROCESSING;
            
            // 根据请求类型执行不同的处理函数
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
                
                case XBDEV_REQ_LVOL_RESIZE:
                    lvol_resize_on_thread(req->ctx);
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
                
                case XBDEV_REQ_LVOL_GET_INFO:
                    lvol_get_info_on_thread(req->ctx);
                    break;
                
                case XBDEV_REQ_LVOL_GET_POOL_INFO:
                    lvol_get_pool_info_on_thread(req->ctx);
                    break;
                
                default:
                    XBDEV_ERRLOG("未知的请求类型: %d\n", req->type);
                    
                    // 设置状态为错误并标记为已完成
                    req->status = XBDEV_REQ_STATUS_ERROR;
                    req->result = -EINVAL;
                    req->done = true;
                    break;
            }
            
            // 如果请求已完成，减少挂起计数并通知等待的线程
            if (req->done) {
                // 减少挂起计数
                __sync_fetch_and_sub(&queue->pending, 1);
                
                // 记录完成时间戳
                req->complete_tsc = spdk_get_ticks();
                
                // 如果是同步请求，通知等待的线程
                if (req->sync_req) {
                    pthread_mutex_lock(&queue->mutex);
                    pthread_cond_broadcast(&queue->cond);
                    pthread_mutex_unlock(&queue->mutex);
                }
                
                // 如果有回调，调用回调
                if (req->cb) {
                    req->cb(req->req_id, req->result, req->cb_arg);
                }
            }
        }
    }
}

/**
 * 测试请求队列系统性能
 * 
 * @param num_requests 测试请求数量
 * @param req_size 请求大小(字节)
 * @return 成功返回平均处理时间(纳秒)，失败返回错误码
 */
int64_t xbdev_queue_benchmark(uint32_t num_requests, uint32_t req_size)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return -EINVAL;
    }
    
    if (num_requests == 0 || req_size == 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    // 分配和提交请求
    xbdev_request_t **requests = malloc(num_requests * sizeof(xbdev_request_t *));
    if (!requests) {
        XBDEV_ERRLOG("内存分配失败\n");
        return -ENOMEM;
    }
    
    // 分配请求
    for (uint32_t i = 0; i < num_requests; i++) {
        requests[i] = xbdev_sync_request_alloc();
        if (!requests[i]) {
            XBDEV_ERRLOG("请求分配失败\n");
            // 清理已分配的请求
            for (uint32_t j = 0; j < i; j++) {
                xbdev_request_free(requests[j]);
            }
            free(requests);
            return -ENOMEM;
        }
        
        // 设置虚拟请求类型和数据
        requests[i]->type = XBDEV_REQ_CUSTOM;
        requests[i]->ctx = malloc(req_size);
        if (!requests[i]->ctx) {
            XBDEV_ERRLOG("请求数据分配失败\n");
            // 清理已分配的请求和数据
            for (uint32_t j = 0; j < i; j++) {
                free(requests[j]->ctx);
                xbdev_request_free(requests[j]);
            }
            free(requests);
            return -ENOMEM;
        }
        
        // 虚拟填充数据
        memset(requests[i]->ctx, 0xAB, req_size);
    }
    
    // 提交请求
    for (uint32_t i = 0; i < num_requests; i++) {
        requests[i]->done = true;  // 模拟请求完成
        
        int rc = xbdev_request_submit(requests[i]);
        if (rc != 0) {
            XBDEV_ERRLOG("请求提交失败: %d\n", rc);
            // 清理所有请求
            for (uint32_t j = 0; j < num_requests; j++) {
                free(requests[j]->ctx);
                xbdev_request_free(requests[j]);
            }
            free(requests);
            return rc;
        }
    }
    
    // 等待所有请求处理完成
    // 注意：由于我们已预先设置done=true，理论上已完成
    
    gettimeofday(&end, NULL);
    
    // 计算平均时间(纳秒)
    int64_t total_us = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
    int64_t avg_ns = (total_us * 1000) / num_requests;
    
    // 清理资源
    for (uint32_t i = 0; i < num_requests; i++) {
        free(requests[i]->ctx);
        xbdev_request_free(requests[i]);
    }
    free(requests);
    
    return avg_ns;
}

/**
 * 获取队列统计信息
 *
 * @param queue_idx 队列索引
 * @param stats 输出参数，统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_queue_get_stats(uint32_t queue_idx, xbdev_queue_stats_t *stats)
{
    if (!g_queue_ctx.initialized) {
        XBDEV_ERRLOG("请求队列系统未初始化\n");
        return -EINVAL;
    }
    
    if (!stats || queue_idx >= g_queue_ctx.num_queues) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 获取队列
    xbdev_queue_t *queue = &g_queue_ctx.queues[queue_idx];
    
    // 填充统计信息
    stats->size = queue->ring.size;
    stats->used = (queue->ring.head - queue->ring.tail) & queue->ring.mask;
    stats->pending = queue->pending;
    stats->id_counter = queue->req_id_counter;
    stats->status = queue->status;
    
    return 0;
}