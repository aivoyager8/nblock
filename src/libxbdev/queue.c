/**
 * @file queue.c
 * @brief 实现跨线程请求队列系统
 *
 * 该文件实现了一个线程安全的请求队列系统，允许客户端线程
 * 提交请求到SPDK线程进行处理。使用无锁环形缓冲区实现高性能通信。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/env.h>
#include <spdk/util.h>
#include <spdk/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * 请求队列结构体
 */
typedef struct {
    xbdev_request_t **requests;     // 请求数组（环形缓冲区）
    uint32_t size;                  // 队列大小
    volatile uint32_t head;         // 队列头（下一个出队位置）
    volatile uint32_t tail;         // 队列尾（下一个入队位置）
    pthread_spinlock_t lock;        // 自旋锁（轻量级同步）
} xbdev_request_queue_t;

/**
 * 请求处理器映射表
 */
typedef struct {
    xbdev_req_type_t type;          // 请求类型
    xbdev_req_handler_t handler;    // 请求处理函数
} xbdev_request_handler_map_t;

/**
 * 全局队列状态
 */
static struct {
    xbdev_request_queue_t *queues;   // 队列数组
    uint32_t num_queues;             // 队列数量
    uint32_t queue_size;             // 每个队列大小
    bool initialized;                // 是否已初始化
    pthread_mutex_t alloc_lock;      // 分配锁
    xbdev_request_t *free_list;      // 空闲请求链表
} g_queue_state = {0};

/**
 * 请求处理器映射表
 */
static const xbdev_request_handler_map_t g_request_handlers[] = {
    {XBDEV_REQ_OPEN, xbdev_open_on_thread},
    {XBDEV_REQ_CLOSE, xbdev_close_on_thread},
    {XBDEV_REQ_READ, xbdev_read_on_thread},
    {XBDEV_REQ_WRITE, xbdev_write_on_thread},
    {XBDEV_REQ_REGISTER_BDEV, xbdev_nvme_register_on_thread},  // 暂时映射到NVMe处理
    {XBDEV_REQ_REMOVE_BDEV, xbdev_device_remove_on_thread},
    // 其他请求类型处理函数将在各自的模块实现中添加
};

/**
 * 初始化队列系统
 * 
 * @param queue_size 每个队列的大小
 * @param num_queues 队列数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_queue_init(uint32_t queue_size, uint32_t num_queues) {
    int rc = 0;
    
    // 检查参数
    if (queue_size == 0 || num_queues == 0 || num_queues > XBDEV_MAX_QUEUES) {
        XBDEV_ERRLOG("无效的队列参数: size=%u, num=%u\n", queue_size, num_queues);
        return -EINVAL;
    }
    
    // 检查是否已初始化
    if (g_queue_state.initialized) {
        XBDEV_ERRLOG("队列系统已经初始化\n");
        return -EALREADY;
    }
    
    // 初始化分配锁
    rc = pthread_mutex_init(&g_queue_state.alloc_lock, NULL);
    if (rc != 0) {
        XBDEV_ERRLOG("无法初始化分配锁: %d\n", rc);
        return -rc;
    }
    
    // 分配队列数组
    g_queue_state.queues = calloc(num_queues, sizeof(xbdev_request_queue_t));
    if (!g_queue_state.queues) {
        XBDEV_ERRLOG("无法分配队列数组\n");
        pthread_mutex_destroy(&g_queue_state.alloc_lock);
        return -ENOMEM;
    }
    
    // 初始化每个队列
    for (uint32_t i = 0; i < num_queues; i++) {
        xbdev_request_queue_t *queue = &g_queue_state.queues[i];
        
        // 分配请求数组
        queue->requests = calloc(queue_size, sizeof(xbdev_request_t*));
        if (!queue->requests) {
            XBDEV_ERRLOG("无法为队列 %u 分配请求数组\n", i);
            rc = -ENOMEM;
            goto cleanup;
        }
        
        // 初始化队列参数
        queue->size = queue_size;
        queue->head = 0;
        queue->tail = 0;
        
        // 初始化队列锁
        rc = pthread_spin_init(&queue->lock, PTHREAD_PROCESS_PRIVATE);
        if (rc != 0) {
            XBDEV_ERRLOG("无法初始化队列 %u 的自旋锁: %d\n", i, rc);
            free(queue->requests);
            rc = -rc;
            goto cleanup;
        }
    }
    
    // 设置全局状态
    g_queue_state.num_queues = num_queues;
    g_queue_state.queue_size = queue_size;
    g_queue_state.initialized = true;
    g_queue_state.free_list = NULL;
    
    XBDEV_NOTICELOG("队列系统初始化成功: %u 个队列，每个队列 %u 个元素\n", 
                  num_queues, queue_size);
    
    return 0;
    
cleanup:
    // 清理已分配的队列
    for (uint32_t i = 0; i < num_queues; i++) {
        if (g_queue_state.queues[i].requests) {
            free(g_queue_state.queues[i].requests);
        }
        
        // 如果自旋锁已初始化，销毁它
        if (i < num_queues && g_queue_state.queues[i].requests) {
            pthread_spin_destroy(&g_queue_state.queues[i].lock);
        }
    }
    
    free(g_queue_state.queues);
    pthread_mutex_destroy(&g_queue_state.alloc_lock);
    
    return rc;
}

/**
 * 清理队列系统
 */
void xbdev_queue_fini(void) {
    // 检查是否已初始化
    if (!g_queue_state.initialized) {
        XBDEV_WARNLOG("队列系统未初始化，无需清理\n");
        return;
    }
    
    // 清理每个队列
    for (uint32_t i = 0; i < g_queue_state.num_queues; i++) {
        xbdev_request_queue_t *queue = &g_queue_state.queues[i];
        
        // 检查队列是否为空
        if (queue->head != queue->tail) {
            XBDEV_WARNLOG("清理非空队列 %u，可能有请求未处理\n", i);
        }
        
        // 销毁队列资源
        pthread_spin_destroy(&queue->lock);
        free(queue->requests);
    }
    
    // 清理空闲请求列表
    pthread_mutex_lock(&g_queue_state.alloc_lock);
    
    xbdev_request_t *req = g_queue_state.free_list;
    while (req) {
        xbdev_request_t *next = req->next;
        free(req);
        req = next;
    }
    
    pthread_mutex_unlock(&g_queue_state.alloc_lock);
    
    // 清理全局资源
    free(g_queue_state.queues);
    pthread_mutex_destroy(&g_queue_state.alloc_lock);
    
    // 重置全局状态
    memset(&g_queue_state, 0, sizeof(g_queue_state));
    
    XBDEV_NOTICELOG("队列系统清理完成\n");
}

/**
 * 检查队列是否满
 * 
 * @param queue 队列指针
 * @return true表示队列已满，false表示队列未满
 */
static inline bool _queue_is_full(xbdev_request_queue_t *queue) {
    return ((queue->tail + 1) % queue->size) == queue->head;
}

/**
 * 检查队列是否为空
 * 
 * @param queue 队列指针
 * @return true表示队列为空，false表示队列非空
 */
static inline bool _queue_is_empty(xbdev_request_queue_t *queue) {
    return queue->head == queue->tail;
}

/**
 * 将请求放入队列
 * 
 * @param queue 队列指针
 * @param req 请求指针
 * @return 成功返回0，队列已满返回-EAGAIN
 */
static int _enqueue_request(xbdev_request_queue_t *queue, xbdev_request_t *req) {
    int rc = 0;
    
    // 加锁
    pthread_spin_lock(&queue->lock);
    
    // 检查队列是否已满
    if (_queue_is_full(queue)) {
        rc = -EAGAIN;
    } else {
        // 将请求放入队列
        queue->requests[queue->tail] = req;
        
        // 更新尾指针
        queue->tail = (queue->tail + 1) % queue->size;
        
        // 记录提交时间戳
        req->submit_tsc = spdk_get_ticks();
        
        // 更新请求状态
        req->status = XBDEV_REQ_STATUS_SUBMITTED;
    }
    
    // 解锁
    pthread_spin_unlock(&queue->lock);
    
    return rc;
}

/**
 * 从队列中取出请求
 * 
 * @param queue 队列指针
 * @param req_ptr 输出参数，存储取出的请求指针
 * @return 成功返回0，队列为空返回-EAGAIN
 */
static int _dequeue_request(xbdev_request_queue_t *queue, xbdev_request_t **req_ptr) {
    int rc = 0;
    
    // 加锁
    pthread_spin_lock(&queue->lock);
    
    // 检查队列是否为空
    if (_queue_is_empty(queue)) {
        rc = -EAGAIN;
    } else {
        // 取出请求
        *req_ptr = queue->requests[queue->head];
        
        // 更新头指针
        queue->head = (queue->head + 1) % queue->size;
        
        // 更新请求状态
        (*req_ptr)->status = XBDEV_REQ_STATUS_PROCESSING;
    }
    
    // 解锁
    pthread_spin_unlock(&queue->lock);
    
    return rc;
}

/**
 * 为请求分配一个队列
 * 
 * @param req 请求指针
 * @return 队列索引
 */
static inline uint32_t _select_queue(xbdev_request_t *req) {
    // TODO: 实现更智能的队列选择策略
    // 当前简单实现：基于请求类型和指针值选择队列
    return ((uintptr_t)req ^ (uint32_t)req->type) % g_queue_state.num_queues;
}

/**
 * 从请求类型获取处理函数
 * 
 * @param req_type 请求类型
 * @return 处理函数指针，未找到返回NULL
 */
static xbdev_req_handler_t _get_handler_for_type(xbdev_req_type_t req_type) {
    for (size_t i = 0; i < sizeof(g_request_handlers) / sizeof(g_request_handlers[0]); i++) {
        if (g_request_handlers[i].type == req_type) {
            return g_request_handlers[i].handler;
        }
    }
    
    return NULL;
}

/**
 * 处理单个请求
 * 
 * @param req 请求指针
 */
static void _process_request(xbdev_request_t *req) {
    // 获取请求处理函数
    xbdev_req_handler_t handler = _get_handler_for_type(req->type);
    
    if (handler) {
        // 调用处理函数
        handler(req->ctx);
        
        // 更新请求状态和完成时间戳
        req->status = XBDEV_REQ_STATUS_COMPLETED;
        req->complete_tsc = spdk_get_ticks();
        
        // 如果有回调函数，调用它
        if (req->cb) {
            req->cb(req->cb_arg, req->result);
        }
    } else {
        XBDEV_ERRLOG("无法处理请求类型: %d\n", req->type);
        
        // 更新请求状态为错误
        req->status = XBDEV_REQ_STATUS_ERROR;
        req->result = -ENOTSUP;
        
        // 如果有回调函数，调用它
        if (req->cb) {
            req->cb(req->cb_arg, req->result);
        }
    }
}

/**
 * 处理队列中的请求
 * 
 * 此函数应由SPDK线程定期调用以处理队列中的请求。
 */
void xbdev_process_request_queues(void) {
    // 检查队列系统是否已初始化
    if (!g_queue_state.initialized) {
        return;
    }
    
    xbdev_request_t *req;
    int num_processed = 0;
    const int max_process_per_call = 32;  // 每次调用最多处理的请求数
    
    // 轮询所有队列
    for (uint32_t q_idx = 0; q_idx < g_queue_state.num_queues; q_idx++) {
        xbdev_request_queue_t *queue = &g_queue_state.queues[q_idx];
        
        // 处理队列中的请求
        while (num_processed < max_process_per_call) {
            if (_dequeue_request(queue, &req) != 0) {
                // 队列为空，处理下一个队列
                break;
            }
            
            // 处理请求
            _process_request(req);
            num_processed++;
        }
        
        // 如果已处理足够多的请求，退出
        if (num_processed >= max_process_per_call) {
            break;
        }
    }
}

/**
 * 分配请求对象
 * 
 * @return 请求指针，失败返回NULL
 */
xbdev_request_t* xbdev_request_alloc(void) {
    xbdev_request_t *req = NULL;
    
    // 检查队列系统是否已初始化
    if (!g_queue_state.initialized) {
        XBDEV_ERRLOG("队列系统未初始化\n");
        return NULL;
    }
    
    // 尝试从空闲列表中获取请求对象
    pthread_mutex_lock(&g_queue_state.alloc_lock);
    
    if (g_queue_state.free_list) {
        req = g_queue_state.free_list;
        g_queue_state.free_list = req->next;
        memset(req, 0, sizeof(*req));
    }
    
    pthread_mutex_unlock(&g_queue_state.alloc_lock);
    
    // 如果没有空闲请求，分配新的
    if (!req) {
        req = calloc(1, sizeof(*req));
    }
    
    if (req) {
        req->status = XBDEV_REQ_STATUS_PENDING;
    }
    
    return req;
}

/**
 * 提交请求到队列
 * 
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_submit(xbdev_request_t *req) {
    int rc;
    
    // 检查参数
    if (!req) {
        XBDEV_ERRLOG("无效的请求指针\n");
        return -EINVAL;
    }
    
    // 检查队列系统是否已初始化
    if (!g_queue_state.initialized) {
        XBDEV_ERRLOG("队列系统未初始化\n");
        return -EINVAL;
    }
    
    // 选择队列
    uint32_t q_idx = _select_queue(req);
    xbdev_request_queue_t *queue = &g_queue_state.queues[q_idx];
    
    // 提交请求到队列
    rc = _enqueue_request(queue, req);
    if (rc != 0) {
        XBDEV_ERRLOG("无法提交请求到队列 %u: %d\n", q_idx, rc);
        return rc;
    }
    
    return 0;
}

/**
 * 等待请求完成
 * 
 * @param req 请求指针
 * @param timeout_us 超时时间(微秒)，0表示不超时
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_wait(xbdev_request_t *req, uint64_t timeout_us) {
    uint64_t start_time, current_time;
    bool timed_out = false;
    
    // 检查参数
    if (!req) {
        XBDEV_ERRLOG("无效的请求指针\n");
        return -EINVAL;
    }
    
    // 记录开始时间
    start_time = spdk_get_ticks();
    
    // 等待请求完成或超时
    while (req->status != XBDEV_REQ_STATUS_COMPLETED && 
           req->status != XBDEV_REQ_STATUS_ERROR &&
           req->status != XBDEV_REQ_STATUS_CANCELED) {
        
        // 检查是否超时
        if (timeout_us > 0) {
            current_time = spdk_get_ticks();
            uint64_t elapsed_us = (current_time - start_time) * 1000000 / spdk_get_ticks_hz();
            
            if (elapsed_us >= timeout_us) {
                timed_out = true;
                break;
            }
        }
        
        // 短暂休眠，避免忙等待
        usleep(100);
    }
    
    if (timed_out) {
        XBDEV_ERRLOG("请求等待超时\n");
        return -ETIMEDOUT;
    }
    
    return (req->status == XBDEV_REQ_STATUS_COMPLETED) ? 0 : req->result;
}

/**
 * 释放请求对象
 * 
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_request_free(xbdev_request_t *req) {
    // 检查参数
    if (!req) {
        XBDEV_ERRLOG("无效的请求指针\n");
        return -EINVAL;
    }
    
    // 检查队列系统是否已初始化
    if (!g_queue_state.initialized) {
        // 如果队列系统未初始化，直接释放内存
        free(req);
        return 0;
    }
    
    // 将请求对象添加到空闲列表
    pthread_mutex_lock(&g_queue_state.alloc_lock);
    
    req->next = g_queue_state.free_list;
    g_queue_state.free_list = req;
    
    pthread_mutex_unlock(&g_queue_state.alloc_lock);
    
    return 0;
}

/**
 * 分配同步请求对象
 * 
 * @return 请求指针，失败返回NULL
 */
xbdev_request_t* xbdev_sync_request_alloc(void) {
    return xbdev_request_alloc();
}

/**
 * 同步执行请求
 * 
 * @param req 请求指针
 * @return 成功返回0，失败返回错误码
 */
int xbdev_sync_request_execute(xbdev_request_t *req) {
    int rc;
    
    // 检查参数
    if (!req) {
        XBDEV_ERRLOG("无效的请求指针\n");
        return -EINVAL;
    }
    
    // 提交请求
    rc = xbdev_request_submit(req);
    if (rc != 0) {
        XBDEV_ERRLOG("无法提交请求: %d\n", rc);
        return rc;
    }
    
    // 等待请求完成
    rc = xbdev_request_wait(req, 30 * 1000000);  // 默认30秒超时
    if (rc != 0) {
        XBDEV_ERRLOG("等待请求完成失败: %d\n", rc);
        return rc;
    }
    
    return req->result;
}

/**
 * 释放同步请求对象
 * 
 * @param req 请求指针
 */
void xbdev_sync_request_free(xbdev_request_t *req) {
    xbdev_request_free(req);
}