/**
 * @file performance.c
 * @brief 性能监控、收集和优化实现
 * 
 * 该文件实现了性能监控、统计收集和自动优化功能。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/event.h>
#include <spdk/log.h>
#include <spdk/util.h>
#include <spdk/bdev.h>
#include <string.h>
#include <time.h>

/**
 * 性能收集器状态
 */
typedef struct {
    const char *device_name;          // 设备名称
    bool active;                      // 是否活动
    int collection_interval_ms;       // 收集间隔(毫秒)
    xbdev_stats_config_t config;      // 统计配置
    xbdev_stats_t current_stats;      // 当前统计数据
    xbdev_stats_t last_stats;         // 上次统计数据
    struct spdk_poller *poller;       // 定期轮询器
    time_t start_time;                // 开始时间
    uint64_t collection_count;        // 收集次数
    
    // 历史数据存储
    xbdev_stats_t *history;           // 历史统计数据
    uint32_t history_size;            // 历史数据大小
    uint32_t history_index;           // 当前历史数据索引
    
    // 热点跟踪
    uint64_t *io_heatmap;             // IO热点图
    uint32_t heatmap_segments;        // 热点图分段数
    
    // 阈值和警报
    xbdev_stats_threshold_t thresholds;   // 性能阈值
    bool threshold_triggered;             // 是否触发阈值
    xbdev_perf_alert_cb alert_cb;         // 告警回调
    void *alert_cb_ctx;                   // 告警回调上下文
} xbdev_perf_collector_t;

// 全局性能收集器数组
#define XBDEV_MAX_PERF_COLLECTORS 128
static xbdev_perf_collector_t g_perf_collectors[XBDEV_MAX_PERF_COLLECTORS] = {0};
static pthread_mutex_t g_perf_collectors_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_active_collectors = 0;

/**
 * 查找或分配性能收集器
 */
static xbdev_perf_collector_t *find_or_alloc_collector(const char *device_name) {
    int i;
    xbdev_perf_collector_t *free_slot = NULL;
    
    pthread_mutex_lock(&g_perf_collectors_mutex);
    
    // 查找已存在的收集器
    for (i = 0; i < XBDEV_MAX_PERF_COLLECTORS; i++) {
        if (g_perf_collectors[i].active && 
            strcmp(g_perf_collectors[i].device_name, device_name) == 0) {
            pthread_mutex_unlock(&g_perf_collectors_mutex);
            return &g_perf_collectors[i];
        }
        
        // 记录第一个空闲槽位
        if (!g_perf_collectors[i].active && !free_slot) {
            free_slot = &g_perf_collectors[i];
        }
    }
    
    // 如果找不到现有收集器，使用空闲槽位
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->device_name = strdup(device_name);
        free_slot->active = true;
        g_active_collectors++;
    }
    
    pthread_mutex_unlock(&g_perf_collectors_mutex);
    return free_slot;
}

/**
 * 释放性能收集器
 */
static void free_collector(xbdev_perf_collector_t *collector) {
    if (!collector || !collector->active) {
        return;
    }
    
    pthread_mutex_lock(&g_perf_collectors_mutex);
    
    // 释放资源
    if (collector->history) {
        free(collector->history);
        collector->history = NULL;
    }
    
    if (collector->io_heatmap) {
        free(collector->io_heatmap);
        collector->io_heatmap = NULL;
    }
    
    if (collector->device_name) {
        free((void *)collector->device_name);
        collector->device_name = NULL;
    }
    
    if (collector->poller) {
        spdk_poller_unregister(&collector->poller);
    }
    
    collector->active = false;
    g_active_collectors--;
    
    pthread_mutex_unlock(&g_perf_collectors_mutex);
}

/**
 * 收集设备统计信息
 */
static void collect_device_stats(xbdev_perf_collector_t *collector) {
    struct spdk_bdev *bdev;
    struct spdk_bdev_io_stat io_stat;
    
    bdev = spdk_bdev_get_by_name(collector->device_name);
    if (!bdev) {
        XBDEV_WARNLOG("收集性能统计时找不到设备: %s\n", collector->device_name);
        return;
    }
    
    // 使用上次统计作为基准
    memcpy(&collector->last_stats, &collector->current_stats, sizeof(xbdev_stats_t));
    
    // 获取SPDK BDEV IO统计信息
    spdk_bdev_get_io_stat(NULL, bdev, &io_stat);
    
    // 更新当前统计信息
    collector->current_stats.read_ops = io_stat.num_read_ops;
    collector->current_stats.write_ops = io_stat.num_write_ops;
    collector->current_stats.unmap_ops = io_stat.num_unmap_ops;
    collector->current_stats.read_bytes = io_stat.bytes_read;
    collector->current_stats.write_bytes = io_stat.bytes_written;
    
    // 计算间隔统计信息
    if (collector->collection_count > 0) {
        uint64_t interval_read_ops = 
            collector->current_stats.read_ops - collector->last_stats.read_ops;
        uint64_t interval_write_ops = 
            collector->current_stats.write_ops - collector->last_stats.write_ops;
        uint64_t interval_read_bytes = 
            collector->current_stats.read_bytes - collector->last_stats.read_bytes;
        uint64_t interval_write_bytes = 
            collector->current_stats.write_bytes - collector->last_stats.write_bytes;
        
        // 计算IOPS和带宽
        float interval_seconds = collector->collection_interval_ms / 1000.0f;
        collector->current_stats.read_iops = interval_read_ops / interval_seconds;
        collector->current_stats.write_iops = interval_write_ops / interval_seconds;
        collector->current_stats.total_iops = 
            collector->current_stats.read_iops + collector->current_stats.write_iops;
        
        collector->current_stats.read_bandwidth_bytes = interval_read_bytes / interval_seconds;
        collector->current_stats.write_bandwidth_bytes = interval_write_bytes / interval_seconds;
        collector->current_stats.total_bandwidth_bytes = 
            collector->current_stats.read_bandwidth_bytes + 
            collector->current_stats.write_bandwidth_bytes;
        
        // 转换为MB/s
        collector->current_stats.read_bandwidth_mb = 
            collector->current_stats.read_bandwidth_bytes / (1024 * 1024);
        collector->current_stats.write_bandwidth_mb = 
            collector->current_stats.write_bandwidth_bytes / (1024 * 1024);
        collector->current_stats.total_bandwidth_mb = 
            collector->current_stats.total_bandwidth_bytes / (1024 * 1024);
        
        // 计算利用率
        // 假设设备的理论最大IOPS和带宽值已知，这里只是示例
        // 真实实现需要从设备属性获取实际最大值
        uint32_t max_iops = collector->config.theoretical_max_iops > 0 ? 
                           collector->config.theoretical_max_iops : 100000;
        uint64_t max_bandwidth = collector->config.theoretical_max_bandwidth_mb > 0 ?
                               collector->config.theoretical_max_bandwidth_mb : 1000;
        
        collector->current_stats.iops_utilization = 
            (collector->current_stats.total_iops * 100.0f) / max_iops;
        collector->current_stats.bandwidth_utilization =
            (collector->current_stats.total_bandwidth_mb * 100.0f) / max_bandwidth;
        
        // 计算平均IO大小
        if (interval_read_ops > 0) {
            collector->current_stats.avg_read_size_bytes = 
                interval_read_bytes / interval_read_ops;
        }
        
        if (interval_write_ops > 0) {
            collector->current_stats.avg_write_size_bytes = 
                interval_write_bytes / interval_write_ops;
        }
        
        // 检查是否触发阈值告警
        bool should_alert = false;
        
        if (collector->thresholds.max_iops > 0 && 
            collector->current_stats.total_iops > collector->thresholds.max_iops) {
            collector->current_stats.threshold_violations |= XBDEV_THRESHOLD_IOPS;
            should_alert = true;
        }
        
        if (collector->thresholds.max_bandwidth_mb > 0 && 
            collector->current_stats.total_bandwidth_mb > collector->thresholds.max_bandwidth_mb) {
            collector->current_stats.threshold_violations |= XBDEV_THRESHOLD_BANDWIDTH;
            should_alert = true;
        }
        
        if (collector->thresholds.max_latency_us > 0 && 
            collector->current_stats.avg_latency_us > collector->thresholds.max_latency_us) {
            collector->current_stats.threshold_violations |= XBDEV_THRESHOLD_LATENCY;
            should_alert = true;
        }
        
        // 如果触发阈值且配置了回调，则调用回调
        if (should_alert && !collector->threshold_triggered && collector->alert_cb) {
            collector->threshold_triggered = true;
            collector->alert_cb(collector->device_name, 
                              &collector->current_stats, 
                              collector->alert_cb_ctx);
        } else if (!should_alert && collector->threshold_triggered) {
            collector->threshold_triggered = false;
        }
        
        // 更新历史统计数据
        if (collector->history && collector->history_size > 0) {
            memcpy(&collector->history[collector->history_index], 
                   &collector->current_stats, 
                   sizeof(xbdev_stats_t));
            
            // 更新历史索引
            collector->history_index = 
                (collector->history_index + 1) % collector->history_size;
        }
    }
    
    collector->collection_count++;
}

/**
 * 定期收集器轮询回调
 */
static int perf_collector_poll(void *ctx) {
    xbdev_perf_collector_t *collector = ctx;
    
    if (!collector->active) {
        return SPDK_POLLER_BUSY;
    }
    
    // 收集统计信息
    collect_device_stats(collector);
    
    return SPDK_POLLER_BUSY;
}

/**
 * 启动性能统计收集
 */
int xbdev_stats_collect_start(const char *device_name, xbdev_stats_config_t *config) {
    xbdev_perf_collector_t *collector;
    struct spdk_thread *thread;
    int rc = 0;
    
    if (!device_name || !config) {
        return -EINVAL;
    }
    
    // 获取或分配收集器
    collector = find_or_alloc_collector(device_name);
    if (!collector) {
        XBDEV_ERRLOG("无法为设备分配性能收集器: %s\n", device_name);
        return -ENOMEM;
    }
    
    // 如果已经在收集，先停止
    if (collector->poller) {
        spdk_poller_unregister(&collector->poller);
    }
    
    // 配置收集器
    collector->collection_interval_ms = config->collection_interval_ms;
    memcpy(&collector->config, config, sizeof(xbdev_stats_config_t));
    collector->active = true;
    collector->collection_count = 0;
    collector->start_time = time(NULL);
    memset(&collector->current_stats, 0, sizeof(xbdev_stats_t));
    memset(&collector->last_stats, 0, sizeof(xbdev_stats_t));
    
    // 配置阈值
    memcpy(&collector->thresholds, &config->thresholds, sizeof(xbdev_stats_threshold_t));
    collector->threshold_triggered = false;
    collector->alert_cb = config->alert_cb;
    collector->alert_cb_ctx = config->alert_cb_ctx;
    
    // 配置历史数据收集
    if (config->keep_history && config->history_size > 0) {
        collector->history = calloc(config->history_size, sizeof(xbdev_stats_t));
        if (!collector->history) {
            XBDEV_ERRLOG("无法分配历史数据存储: %s\n", device_name);
            rc = -ENOMEM;
            goto error;
        }
        collector->history_size = config->history_size;
        collector->history_index = 0;
    }
    
    // 配置热点跟踪
    if (config->track_hotspots && config->hotspot_segments > 0) {
        collector->io_heatmap = calloc(config->hotspot_segments, sizeof(uint64_t));
        if (!collector->io_heatmap) {
            XBDEV_ERRLOG("无法分配IO热点图: %s\n", device_name);
            rc = -ENOMEM;
            goto error;
        }
        collector->heatmap_segments = config->hotspot_segments;
    }
    
    // 注册轮询器
    thread = spdk_get_thread();
    if (!thread) {
        XBDEV_ERRLOG("找不到SPDK线程\n");
        rc = -EINVAL;
        goto error;
    }
    
    collector->poller = spdk_poller_register(perf_collector_poll, 
                                          collector, 
                                          collector->collection_interval_ms * 1000);
    if (!collector->poller) {
        XBDEV_ERRLOG("无法注册性能收集轮询器: %s\n", device_name);
        rc = -ENOMEM;
        goto error;
    }
    
    XBDEV_NOTICELOG("已启动设备性能统计收集: %s (间隔: %d ms)\n", 
                  device_name, collector->collection_interval_ms);
    
    return 0;
    
error:
    if (collector->history) {
        free(collector->history);