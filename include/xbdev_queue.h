/**
 * @file xbdev_queue.h
 * @brief 定义队列和请求管理的公共结构和接口
 */

#ifndef XBDEV_QUEUE_H
#define XBDEV_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 队列统计信息结构体
 */
struct xbdev_queue_stats {
    uint32_t num_queues;              // 队列数量
    uint32_t queue_capacity;          // 总队列容量
    uint32_t queue_used;              // 当前使用的队列项数
    uint32_t queue_usage_percent;     // 队列使用率百分比
    uint32_t request_pool_capacity;   // 请求池容量
    uint32_t request_pool_used;       // 当前使用的请求数
    uint32_t pool_usage_percent;      // 请求池使用率百分比
    uint64_t processed_requests;      // 处理的请求总数
    uint64_t request_errors;          // 请求错误数
    uint64_t avg_processing_time_ns;  // 平均请求处理时间(ns)
};

/**
 * 队列初始化和清理
 */
int xbdev_queue_init(uint32_t queue_size, uint32_t num_queues);
void xbdev_queue_fini(void);

/**
 * 队列统计信息
 */
int xbdev_get_queue_stats(struct xbdev_queue_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* XBDEV_QUEUE_H */
