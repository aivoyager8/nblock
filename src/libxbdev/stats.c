/**
 * @file stats.c
 * @brief 实现性能统计收集和监控功能
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/bdev.h>
#include <spdk/util.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 性能统计阈值定义
#define DEFAULT_MAX_IOPS          100000
#define DEFAULT_MAX_BANDWIDTH_MB   1000
#define DEFAULT_MAX_LATENCY_US    10000

/**
 * 设备统计上下文
 */
typedef struct {
    char device_name[256];            // 设备名称
    bool active;                      // 是否活动
    uint32_t collection_interval_ms;  // 收集间隔(毫秒)
    
    // 统计数据
    xbdev_stats_t current;           // 当前统计
    xbdev_stats_t previous;          // 上次统计
    xbdev_stats_t avg;               // 平均统计
    xbdev_stats_t peak;              // 峰值统计
    
    // 历史数据
    xbdev_stats_t *history;          // 历史统计数组
    uint32_t history_count;           // 历史统计数量
    uint32_t history_index;           // 当前历史索引
    uint32_t history_capacity;        // 历史容量
    
    // 配置参数
    bool keep_history;                // 是否保留历史
    uint32_t history_size;            // 历史记录大小
    uint64_t theoretical_max_iops;    // 理论最大IOPS
    uint64_t theoretical_max_bandwidth_mb; // 理论最大带宽(MB)
    
    // 热点跟踪
    bool track_hotspots;              // 是否跟踪热点
    uint32_t hotspot_segments;        // 热点分段数
    uint64_t *io_heatmap;             // IO热点计数
    
    // 采样控制
    struct spdk_poller *poller;       // 定期采样轮询器
    uint64_t start_time;              // 开始时间
    uint64_t total_samples;           // 总采样次数
    
    // 告警回调
    xbdev_stats_threshold_t thresholds;    // 性能阈值
    uint32_t threshold_violations;         // 阈值违反标志
    bool alert_triggered;                  // 告警是否已触发
    xbdev_perf_alert_cb alert_callback;    // 告警回调函数
    void *alert_callback_ctx;              // 告警回调上下文
} xbdev_stats_ctx_t;

// 全局统计上下文管理
#define XBDEV_MAX_STAT_DEVICES 128
static xbdev_stats_ctx_t g_stats_contexts[XBDEV_MAX_STAT_DEVICES];
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_stats_initialized = false;

/**
 * 初始化统计模块
 */
int xbdev_stats_init(void) {
    pthread_mutex_lock(&g_stats_mutex);
    
    if (g_stats_initialized) {
        pthread_mutex_unlock(&g_stats_mutex);
        return 0;
    }
    
    // 初始化统计上下文
    memset(g_stats_contexts, 0, sizeof(g_stats_contexts));
    g_stats_initialized = true;
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    XBDEV_NOTICELOG("性能统计模块已初始化\n");
    return 0;
}

/**
 * 清理统计模块
 */
void xbdev_stats_fini(void) {
    pthread_mutex_lock(&g_stats_mutex);
    
    if (!g_stats_initialized) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }
    
    // 清理所有活动的统计上下文
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active) {
            // 停止轮询器
            if (g_stats_contexts[i].poller) {
                spdk_poller_unregister(&g_stats_contexts[i].poller);
            }
            
            // 释放历史数据
            if (g_stats_contexts[i].history) {
                free(g_stats_contexts[i].history);
            }
            
            // 释放热点图
            if (g_stats_contexts[i].io_heatmap) {
                free(g_stats_contexts[i].io_heatmap);
            }
            
            // 清空上下文
            memset(&g_stats_contexts[i], 0, sizeof(xbdev_stats_ctx_t));
        }
    }
    
    g_stats_initialized = false;
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    XBDEV_NOTICELOG("性能统计模块已清理\n");
}

/**
 * 查找或分配设备统计上下文
 */
static xbdev_stats_ctx_t *find_or_alloc_stats_ctx(const char *device_name) {
    int i, free_idx = -1;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    // 查找现有上下文
    for (i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active && 
            strcmp(g_stats_contexts[i].device_name, device_name) == 0) {
            pthread_mutex_unlock(&g_stats_mutex);
            return &g_stats_contexts[i];
        }
        
        // 记录第一个空闲位置
        if (free_idx == -1 && !g_stats_contexts[i].active) {
            free_idx = i;
        }
    }
    
    // 如果没有找到，分配新的
    if (free_idx != -1) {
        xbdev_stats_ctx_t *ctx = &g_stats_contexts[free_idx];
        memset(ctx, 0, sizeof(xbdev_stats_ctx_t));
        strncpy(ctx->device_name, device_name, sizeof(ctx->device_name) - 1);
        ctx->active = true;
        ctx->collection_interval_ms = 1000; // 默认每秒采集一次
        
        pthread_mutex_unlock(&g_stats_mutex);
        return ctx;
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    // 没有空闲上下文
    XBDEV_ERRLOG("无法为设备分配性能统计上下文: %s\n", device_name);
    return NULL;
}

/**
 * 计算速率指标（如IOPS和带宽）
 */
static void calculate_rate_stats(xbdev_stats_ctx_t *ctx) {
    // 计算经过的秒数
    float elapsed_sec = ctx->collection_interval_ms / 1000.0f;
    if (elapsed_sec <= 0) elapsed_sec = 1.0f; // 避免除零
    
    // 计算IO操作差值
    uint64_t read_ops_diff = ctx->current.read_ops - ctx->previous.read_ops;
    uint64_t write_ops_diff = ctx->current.write_ops - ctx->previous.write_ops;
    uint64_t unmap_ops_diff = ctx->current.unmap_ops - ctx->previous.unmap_ops;
    
    // 计算数据传输差值
    uint64_t read_bytes_diff = ctx->current.read_bytes - ctx->previous.read_bytes;
    uint64_t write_bytes_diff = ctx->current.write_bytes - ctx->previous.write_bytes;
    
    // 计算IOPS
    ctx->current.read_iops = read_ops_diff / elapsed_sec;
    ctx->current.write_iops = write_ops_diff / elapsed_sec;
    ctx->current.unmap_iops = unmap_ops_diff / elapsed_sec;
    ctx->current.total_iops = ctx->current.read_iops + ctx->current.write_iops + ctx->current.unmap_iops;
    
    // 计算带宽
    ctx->current.read_bandwidth_bytes = read_bytes_diff / elapsed_sec;
    ctx->current.write_bandwidth_bytes = write_bytes_diff / elapsed_sec;
    ctx->current.total_bandwidth_bytes = ctx->current.read_bandwidth_bytes + ctx->current.write_bandwidth_bytes;
    
    // 转换为MB/s
    ctx->current.read_bandwidth_mb = ctx->current.read_bandwidth_bytes / (1024 * 1024);
    ctx->current.write_bandwidth_mb = ctx->current.write_bandwidth_bytes / (1024 * 1024);
    ctx->current.total_bandwidth_mb = ctx->current.total_bandwidth_bytes / (1024 * 1024);
    
    // 计算平均IO大小
    if (read_ops_diff > 0) {
        ctx->current.avg_read_size_bytes = read_bytes_diff / read_ops_diff;
    }
    
    if (write_ops_diff > 0) {
        ctx->current.avg_write_size_bytes = write_bytes_diff / write_ops_diff;
    }
    
    // 计算利用率
    uint64_t max_iops = ctx->theoretical_max_iops > 0 ? ctx->theoretical_max_iops : DEFAULT_MAX_IOPS;
    uint64_t max_bw = ctx->theoretical_max_bandwidth_mb > 0 ? ctx->theoretical_max_bandwidth_mb : DEFAULT_MAX_BANDWIDTH_MB;
    
    ctx->current.iops_utilization = (ctx->current.total_iops * 100.0f) / max_iops;
    ctx->current.bandwidth_utilization = (ctx->current.total_bandwidth_mb * 100.0f) / max_bw;
    
    // 更新峰值统计
    if (ctx->current.total_iops > ctx->peak.total_iops) {
        ctx->peak.total_iops = ctx->current.total_iops;
    }
    
    if (ctx->current.total_bandwidth_mb > ctx->peak.total_bandwidth_mb) {
        ctx->peak.total_bandwidth_mb = ctx->current.total_bandwidth_mb;
    }
    
    if (ctx->current.avg_latency_us > ctx->peak.avg_latency_us) {
        ctx->peak.avg_latency_us = ctx->current.avg_latency_us;
    }
}

/**
 * 更新平均统计数据
 */
static void update_avg_stats(xbdev_stats_ctx_t *ctx) {
    // 使用指数移动平均值(EMA)计算平均值
    // 简单地说，新的平均值 = alpha * 当前值 + (1-alpha) * 旧平均值
    // 这里我们选择 alpha = 0.2 作为平滑因子
    float alpha = 0.2f;
    
    ctx->avg.total_iops = alpha * ctx->current.total_iops + (1-alpha) * ctx->avg.total_iops;
    ctx->avg.read_iops = alpha * ctx->current.read_iops + (1-alpha) * ctx->avg.read_iops;
    ctx->avg.write_iops = alpha * ctx->current.write_iops + (1-alpha) * ctx->avg.write_iops;
    ctx->avg.unmap_iops = alpha * ctx->current.unmap_iops + (1-alpha) * ctx->avg.unmap_iops;
    
    ctx->avg.total_bandwidth_mb = alpha * ctx->current.total_bandwidth_mb + (1-alpha) * ctx->avg.total_bandwidth_mb;
    ctx->avg.read_bandwidth_mb = alpha * ctx->current.read_bandwidth_mb + (1-alpha) * ctx->avg.read_bandwidth_mb;
    ctx->avg.write_bandwidth_mb = alpha * ctx->current.write_bandwidth_mb + (1-alpha) * ctx->avg.write_bandwidth_mb;
    
    ctx->avg.avg_latency_us = alpha * ctx->current.avg_latency_us + (1-alpha) * ctx->avg.avg_latency_us;
    ctx->avg.avg_queue_depth = alpha * ctx->current.avg_queue_depth + (1-alpha) * ctx->avg.avg_queue_depth;
}

/**
 * 检查性能阈值告警
 */
static void check_threshold_alerts(xbdev_stats_ctx_t *ctx) {
    // 重置违反标志
    ctx->current.threshold_violations = 0;
    bool should_alert = false;
    
    // 检查IOPS阈值
    if (ctx->thresholds.max_iops > 0 && ctx->current.total_iops > ctx->thresholds.max_iops) {
        ctx->current.threshold_violations |= XBDEV_THRESHOLD_IOPS;
        should_alert = true;
    }
    
    // 检查带宽阈值
    if (ctx->thresholds.max_bandwidth_mb > 0 && 
        ctx->current.total_bandwidth_mb > ctx->thresholds.max_bandwidth_mb) {
        ctx->current.threshold_violations |= XBDEV_THRESHOLD_BANDWIDTH;
        should_alert = true;
    }
    
    // 检查延迟阈值
    if (ctx->thresholds.max_latency_us > 0 && 
        ctx->current.avg_latency_us > ctx->thresholds.max_latency_us) {
        ctx->current.threshold_violations |= XBDEV_THRESHOLD_LATENCY;
        should_alert = true;
    }
    
    // 检查队列深度阈值
    if (ctx->thresholds.max_queue_depth > 0 && 
        ctx->current.avg_queue_depth > ctx->thresholds.max_queue_depth) {
        ctx->current.threshold_violations |= XBDEV_THRESHOLD_QUEUE_DEPTH;
        should_alert = true;
    }
    
    // 如果应该触发告警且告警回调已设置
    if (should_alert && ctx->alert_callback && !ctx->alert_triggered) {
        ctx->alert_triggered = true;
        ctx->alert_callback(ctx->device_name, &ctx->current, ctx->alert_callback_ctx);
    } else if (!should_alert && ctx->alert_triggered) {
        // 重置告警标志
        ctx->alert_triggered = false;
    }
}

/**
 * 性能统计轮询回调
 */
static int stats_poller_callback(void *arg) {
    xbdev_stats_ctx_t *ctx = (xbdev_stats_ctx_t *)arg;
    
    // 检查上下文状态
    if (!ctx->active) {
        return SPDK_POLLER_BUSY;
    }
    
    // 获取BDEV
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(ctx->device_name);
    if (!bdev) {
        XBDEV_WARNLOG("获取性能统计时找不到设备: %s\n", ctx->device_name);
        return SPDK_POLLER_BUSY;
    }
    
    // 保存上次统计
    memcpy(&ctx->previous, &ctx->current, sizeof(xbdev_stats_t));
    
    // 获取IO统计
    struct spdk_bdev_io_stat io_stat;
    spdk_bdev_get_io_stat(NULL, bdev, &io_stat);
    
    // 更新当前统计
    ctx->current.read_ops = io_stat.num_read_ops;
    ctx->current.write_ops = io_stat.num_write_ops;
    ctx->current.unmap_ops = io_stat.num_unmap_ops;
    ctx->current.read_bytes = io_stat.bytes_read;
    ctx->current.write_bytes = io_stat.bytes_written;
    
    // FIXME: 延迟目前获取不到，需要通过IO追踪模块来计算
    // 这里使用模拟数据
    ctx->current.avg_latency_us = 500;  // 假设平均延迟500微秒
    ctx->current.avg_queue_depth = 4;   // 假设平均队列深度4
    
    // 更新总IO数
    ctx->current.total_io_count = ctx->current.read_ops + 
                                ctx->current.write_ops + 
                                ctx->current.unmap_ops;
    
    // 采样数大于0时才计算速率
    if (ctx->total_samples > 0) {
        // 计算IOPS、带宽等速率指标
        calculate_rate_stats(ctx);
        
        // 更新平均统计
        update_avg_stats(ctx);
        
        // 检查阈值告警
        check_threshold_alerts(ctx);
        
        // 更新历史数据
        if (ctx->keep_history && ctx->history) {
            memcpy(&ctx->history[ctx->history_index], 
                   &ctx->current, 
                   sizeof(xbdev_stats_t));
            
            // 更新索引(环形缓冲区)
            ctx->history_index = (ctx->history_index + 1) % ctx->history_capacity;
            
            // 更新计数
            if (ctx->history_count < ctx->history_capacity) {
                ctx->history_count++;
            }
        }
    }
    
    // 采样计数增加
    ctx->total_samples++;
    
    return SPDK_POLLER_BUSY;
}

/**
 * 启动性能统计收集
 *
 * @param device_name 设备名称
 * @param config 统计配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_collect_start(const char *device_name, xbdev_stats_config_t *config) {
    xbdev_request_t *req;
    struct {
        const char *device_name;
        xbdev_stats_config_t *config;
        xbdev_stats_ctx_t *ctx;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    if (!device_name || !config) {
        return -EINVAL;
    }
    
    // 查找或分配统计上下文
    ctx.ctx = find_or_alloc_stats_ctx(device_name);
    if (!ctx.ctx) {
        return -ENOMEM;
    }
    
    // 设置上下文
    ctx.device_name = device_name;
    ctx.config = config;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_STATS_START;
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
        XBDEV_ERRLOG("启动性能统计失败: %s, rc=%d\n", device_name, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("已启动设备性能统计: %s\n", device_name);
    
    return 0;
}

/**
 * SPDK线程上下文中启动性能统计
 */
void xbdev_stats_collect_start_on_thread(void *arg) {
    struct {
        const char *device_name;
        xbdev_stats_config_t *config;
        xbdev_stats_ctx_t *ctx;
        int rc;
        bool done;
    } *args = arg;
    
    xbdev_stats_ctx_t *ctx = args->ctx;
    xbdev_stats_config_t *config = args->config;
    
    // 检查设备是否存在
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(args->device_name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", args->device_name);
        args->rc = -ENOENT;
        args->done = true;
        return;
    }
    
    // 如果已经有轮询器，先停止
    if (ctx->poller) {
        spdk_poller_unregister(&ctx->poller);
    }
    
    // 释放旧的历史和热点数据
    if (ctx->history) {
        free(ctx->history);
        ctx->history = NULL;
    }
    
    if (ctx->io_heatmap) {
        free(ctx->io_heatmap);
        ctx->io_heatmap = NULL;
    }
    
    // 初始化统计上下文
    ctx->collection_interval_ms = config->collection_interval_ms;
    ctx->keep_history = config->keep_history;
    ctx->history_size = config->history_size;
    ctx->track_hotspots = config->track_hotspots;
    ctx->hotspot_segments = config->hotspot_segments;
    ctx->theoretical_max_iops = config->theoretical_max_iops;
    ctx->theoretical_max_bandwidth_mb = config->theoretical_max_bandwidth_mb;
    ctx->active = true;
    ctx->total_samples = 0;
    
    // 复制阈值配置
    memcpy(&ctx->thresholds, &config->thresholds, sizeof(xbdev_stats_threshold_t));
    
    // 复制回调信息
    ctx->alert_callback = config->alert_cb;
    ctx->alert_callback_ctx = config->alert_cb_ctx;
    
    // 清空统计数据
    memset(&ctx->current, 0, sizeof(xbdev_stats_t));
    memset(&ctx->previous, 0, sizeof(xbdev_stats_t));
    memset(&ctx->avg, 0, sizeof(xbdev_stats_t));
    memset(&ctx->peak, 0, sizeof(xbdev_stats_t));
    
    // 分配历史数据空间
    if (ctx->keep_history && ctx->history_size > 0) {
        ctx->history = calloc(ctx->history_size, sizeof(xbdev_stats_t));
        if (!ctx->history) {
            XBDEV_ERRLOG("无法分配历史数据内存\n");
            args->rc = -ENOMEM;
            args->done = true;
            return;
        }
        ctx->history_capacity = ctx->history_size;
        ctx->history_count = 0;
        ctx->history_index = 0;
    }
    
    // 分配热点图空间
    if (ctx->track_hotspots && ctx->hotspot_segments > 0) {
        ctx->io_heatmap = calloc(ctx->hotspot_segments, sizeof(uint64_t));
        if (!ctx->io_heatmap) {
            XBDEV_ERRLOG("无法分配热点图内存\n");
            if (ctx->history) free(ctx->history);
            ctx->history = NULL;
            args->rc = -ENOMEM;
            args->done = true;
            return;
        }
    }
    
    // 记录开始时间
    ctx->start_time = time(NULL);
    
    // 注册轮询器
    ctx->poller = spdk_poller_register(stats_poller_callback, ctx, 
                                    ctx->collection_interval_ms * 1000);
    if (!ctx->poller) {
        XBDEV_ERRLOG("无法注册性能统计轮询器\n");
        if (ctx->history) free(ctx->history);
        if (ctx->io_heatmap) free(ctx->io_heatmap);
        ctx->history = NULL;
        ctx->io_heatmap = NULL;
        args->rc = -ENOMEM;
        args->done = true;
        return;
    }
    
    // 完成
    args->rc = 0;
    args->done = true;
}

/**
 * 停止性能统计收集
 * 
 * @param device_name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_collect_stop(const char *device_name) {
    xbdev_request_t *req;
    struct {
        const char *device_name;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    if (!device_name) {
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.device_name = device_name;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_STATS_STOP;
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
        XBDEV_ERRLOG("停止性能统计失败: %s, rc=%d\n", device_name, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放请求
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("已停止设备性能统计: %s\n", device_name);
    
    return 0;
}

/**
 * SPDK线程上下文中停止性能统计
 */
void xbdev_stats_collect_stop_on_thread(void *arg) {
    struct {
        const char *device_name;
        int rc;
        bool done;
    } *args = arg;
    
    // 查找统计上下文
    xbdev_stats_ctx_t *ctx = NULL;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active && 
            strcmp(g_stats_contexts[i].device_name, args->device_name) == 0) {
            ctx = &g_stats_contexts[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    if (!ctx) {
        XBDEV_ERRLOG("找不到设备统计上下文: %s\n", args->device_name);
        args->rc = -ENOENT;
        args->done = true;
        return;
    }
    
    // 停止轮询器
    if (ctx->poller) {
        spdk_poller_unregister(&ctx->poller);
        ctx->poller = NULL;
    }
    
    // 设置为非活动
    ctx->active = false;
    
    // 完成
    args->rc = 0;
    args->done = true;
}

/**
 * 获取当前性能统计
 * 
 * @param device_name 设备名称
 * @param stats 输出统计结果
 * @param get_avg 是否获取平均值（true）或当前值（false）
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_get(const char *device_name, xbdev_stats_t *stats, bool get_avg) {
    if (!device_name || !stats) {
        return -EINVAL;
    }
    
    // 查找统计上下文
    xbdev_stats_ctx_t *ctx = NULL;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active && 
            strcmp(g_stats_contexts[i].device_name, device_name) == 0) {
            ctx = &g_stats_contexts[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    if (!ctx) {
        XBDEV_ERRLOG("找不到设备统计上下文: %s\n", device_name);
        return -ENOENT;
    }
    
    // 复制统计数据
    if (get_avg) {
        memcpy(stats, &ctx->avg, sizeof(xbdev_stats_t));
    } else {
        memcpy(stats, &ctx->current, sizeof(xbdev_stats_t));
    }
    
    return 0;
}

/**
 * 获取峰值性能统计
 * 
 * @param device_name 设备名称
 * @param stats 输出峰值统计结果
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_get_peak(const char *device_name, xbdev_stats_t *stats) {
    if (!device_name || !stats) {
        return -EINVAL;
    }
    
    // 查找统计上下文
    xbdev_stats_ctx_t *ctx = NULL;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active && 
            strcmp(g_stats_contexts[i].device_name, device_name) == 0) {
            ctx = &g_stats_contexts[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    if (!ctx) {
        XBDEV_ERRLOG("找不到设备统计上下文: %s\n", device_name);
        return -ENOENT;
    }
    
    // 复制峰值统计数据
    memcpy(stats, &ctx->peak, sizeof(xbdev_stats_t));
    
    return 0;
}

/**
 * 获取历史性能数据
 * 
 * @param device_name 设备名称
 * @param history 输出历史数据数组
 * @param max_entries 数组最大条目数
 * @param actual_entries 输出实际条目数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_get_history(const char *device_name, xbdev_stats_t *history, 
                          uint32_t max_entries, uint32_t *actual_entries) {
    if (!device_name || !history || !actual_entries) {
        return -EINVAL;
    }
    
    // 查找统计上下文
    xbdev_stats_ctx_t *ctx = NULL;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active && 
            strcmp(g_stats_contexts[i].device_name, device_name) == 0) {
            ctx = &g_stats_contexts[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    if (!ctx) {
        XBDEV_ERRLOG("找不到设备统计上下文: %s\n", device_name);
        return -ENOENT;
    }
    
    // 检查是否有历史数据
    if (!ctx->keep_history || !ctx->history || ctx->history_count == 0) {
        *actual_entries = 0;
        return 0;
    }
    
    // 计算实际要返回的条目数
    uint32_t entries = ctx->history_count;
    if (entries > max_entries) {
        entries = max_entries;
    }
    
    // 复制历史数据，从最新的开始
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t idx = 0;
        if (ctx->history_count >= ctx->history_capacity) {
            // 如果历史已满，从当前位置往前取
            idx = (ctx->history_index - 1 - i + ctx->history_capacity) % ctx->history_capacity;
        } else {
            // 如果历史未满，从已有数据的末尾往前取
            if (i < ctx->history_index) {
                idx = ctx->history_index - 1 - i;
            } else {
                // 不应该到这里
                break;
            }
        }
        
        memcpy(&history[i], &ctx->history[idx], sizeof(xbdev_stats_t));
    }
    
    *actual_entries = entries;
    
    return 0;
}

/**
 * 获取热点图数据
 * 
 * @param device_name 设备名称
 * @param heatmap 输出热点图数据
 * @param max_segments 最大段数
 * @param actual_segments 输出实际段数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_get_heatmap(const char *device_name, uint64_t *heatmap, 
                          uint32_t max_segments, uint32_t *actual_segments) {
    if (!device_name || !heatmap || !actual_segments) {
        return -EINVAL;
    }
    
    // 查找统计上下文
    xbdev_stats_ctx_t *ctx = NULL;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active && 
            strcmp(g_stats_contexts[i].device_name, device_name) == 0) {
            ctx = &g_stats_contexts[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    if (!ctx) {
        XBDEV_ERRLOG("找不到设备统计上下文: %s\n", device_name);
        return -ENOENT;
    }
    
    // 检查是否有热点图数据
    if (!ctx->track_hotspots || !ctx->io_heatmap || ctx->hotspot_segments == 0) {
        *actual_segments = 0;
        return 0;
    }
    
    // 计算实际要返回的段数
    uint32_t segments = ctx->hotspot_segments;
    if (segments > max_segments) {
        segments = max_segments;
    }
    
    // 复制热点图数据
    memcpy(heatmap, ctx->io_heatmap, segments * sizeof(uint64_t));
    
    *actual_segments = segments;
    
    return 0;
}

/**
 * 重置性能统计
 * 
 * @param device_name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_reset(const char *device_name) {
    if (!device_name) {
        return -EINVAL;
    }
    
    // 查找统计上下文
    xbdev_stats_ctx_t *ctx = NULL;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES; i++) {
        if (g_stats_contexts[i].active && 
            strcmp(g_stats_contexts[i].device_name, device_name) == 0) {
            ctx = &g_stats_contexts[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    if (!ctx) {
        XBDEV_ERRLOG("找不到设备统计上下文: %s\n", device_name);
        return -ENOENT;
    }
    
    // 重置统计数据
    memset(&ctx->current, 0, sizeof(xbdev_stats_t));
    memset(&ctx->previous, 0, sizeof(xbdev_stats_t));
    memset(&ctx->avg, 0, sizeof(xbdev_stats_t));
    memset(&ctx->peak, 0, sizeof(xbdev_stats_t));
    
    // 重置历史数据
    if (ctx->history) {
        memset(ctx->history, 0, ctx->history_capacity * sizeof(xbdev_stats_t));
        ctx->history_count = 0;
        ctx->history_index = 0;
    }
    
    // 重置热点图
    if (ctx->io_heatmap) {
        memset(ctx->io_heatmap, 0, ctx->hotspot_segments * sizeof(uint64_t));
    }
    
    // 重置计数和时间
    ctx->start_time = time(NULL);
    ctx->total_samples = 0;
    
    return 0;
}

/**
 * 获取所有活动监控设备名称
 * 
 * @param names 输出设备名称数组
 * @param max_devices 最大设备数
 * @param actual_devices 输出实际设备数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_stats_get_devices(char **names, uint32_t max_devices, uint32_t *actual_devices) {
    if (!names || !actual_devices) {
        return -EINVAL;
    }
    
    uint32_t count = 0;
    
    pthread_mutex_lock(&g_stats_mutex);
    
    // 遍历统计上下文数组
    for (int i = 0; i < XBDEV_MAX_STAT_DEVICES && count < max_devices; i++) {
        if (g_stats_contexts[i].active) {
            names[count] = strdup(g_stats_contexts[i].device_name);
            if (!names[count]) {
                XBDEV_ERRLOG("内存分配失败\n");
                // 释放已分配的内存
                for (uint32_t j = 0; j < count; j++) {
                    free(names[j]);
                }
                *actual_devices = 0;
                pthread_mutex_unlock(&g_stats_mutex);
                return -ENOMEM;
            }
            count++;
        }
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
    
    *actual_devices = count;
    
    return 0;
}