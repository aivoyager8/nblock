/**
 * @file xbdev.h
 * @brief libxbdev公共API头文件
 *
 * 本文件定义了libxbdev库的公共API，供应用程序使用。
 */

#ifndef _XBDEV_H_
#define _XBDEV_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/uio.h>

/**
 * IO操作类型
 */
#define XBDEV_OP_READ     0  // 读取操作
#define XBDEV_OP_WRITE    1  // 写入操作

/**
 * IO类型位掩码
 */
#define XBDEV_IO_TYPE_READ               (1ULL << 0)
#define XBDEV_IO_TYPE_WRITE              (1ULL << 1)
#define XBDEV_IO_TYPE_UNMAP              (1ULL << 2)
#define XBDEV_IO_TYPE_FLUSH              (1ULL << 3)
#define XBDEV_IO_TYPE_RESET              (1ULL << 4)
#define XBDEV_IO_TYPE_COMPARE            (1ULL << 5)
#define XBDEV_IO_TYPE_COMPARE_AND_WRITE  (1ULL << 6)
#define XBDEV_IO_TYPE_WRITE_ZEROES       (1ULL << 7)

/**
 * IO操作选项
 */
#define XBDEV_OPT_TIMEOUT_MS       1  // IO超时时间(毫秒)
#define XBDEV_OPT_RETRY_COUNT      2  // IO重试次数
#define XBDEV_OPT_QUEUE_DEPTH      3  // IO队列深度
#define XBDEV_OPT_CACHE_POLICY     4  // 缓存策略

/**
 * 缓存策略
 */
#define XBDEV_CACHE_POLICY_NONE       0  // 无缓存
#define XBDEV_CACHE_POLICY_READ_ONLY  1  // 只读缓存
#define XBDEV_CACHE_POLICY_WRITE_BACK 2  // 写回缓存
#define XBDEV_CACHE_POLICY_MAX        2  // 最大有效策略值

/**
 * 打开文件标志
 */
#define XBDEV_OPEN_RDONLY       0       // 只读模式
#define XBDEV_OPEN_WRONLY       1       // 只写模式
#define XBDEV_OPEN_RDWR         2       // 读写模式
#define XBDEV_OPEN_NONBLOCK     04000   // 非阻塞模式
#define XBDEV_OPEN_DIRECT       040000  // 直接IO模式

/**
 * IO批处理句柄
 */
typedef struct xbdev_io_batch {
    int max_ops;                    // 最大操作数
    int num_ops;                    // 当前操作数
    struct xbdev_io_op {
        int fd;                     // 文件描述符
        void *buf;                  // 数据缓冲区
        size_t count;               // 数据长度
        uint64_t offset;            // 偏移量
        int type;                   // 操作类型(读/写)
    } ops[0];                       // 操作数组(变长)
} xbdev_io_batch_t;

/**
 * 原子操作定义
 */
typedef struct {
    enum {
        XBDEV_ATOMIC_READ,         // 读取操作
        XBDEV_ATOMIC_WRITE,        // 写入操作
        XBDEV_ATOMIC_COMPARE_WRITE // 比较并写入操作
    } op_type;                     // 操作类型
    void *buf;                     // 数据缓冲区
    size_t count;                  // 数据长度
    uint64_t offset;               // 偏移量
    void *compare_buf;             // 比较缓冲区(仅用于比较并写入)
} xbdev_atomic_op_t;

/**
 * 设备信息结构
 */
typedef struct {
    char name[64];               // 设备名称
    char product_name[64];       // 产品名称
    char driver[32];             // 驱动名称
    uint32_t block_size;         // 块大小(字节)
    uint64_t num_blocks;         // 块数量
    uint64_t size_bytes;         // 总大小(字节)
    bool write_cache;            // 是否有写缓存
    uint32_t md_size;            // 元数据大小
    uint32_t optimal_io_boundary; // 最佳IO边界
    uint32_t required_alignment;  // 对齐要求
    bool claimed;                // 是否被占用
    uint64_t supported_io_types;  // 支持的IO类型
} xbdev_device_info_t;

/**
 * IO统计信息结构
 */
typedef struct {
    uint64_t bytes_read;         // 读取字节数
    uint64_t bytes_written;      // 写入字节数
    uint64_t num_read_ops;       // 读取操作次数
    uint64_t num_write_ops;      // 写入操作次数
    uint64_t num_unmap_ops;      // UNMAP操作次数
    uint64_t read_latency_ticks;  // 读取延迟总和(时钟周期)
    uint64_t write_latency_ticks; // 写入延迟总和(时钟周期)
    uint64_t unmap_latency_ticks; // UNMAP延迟总和(时钟周期)
    uint64_t avg_read_latency_us;  // 平均读取延迟(微秒)
    uint64_t avg_write_latency_us; // 平均写入延迟(微秒)
    uint64_t avg_unmap_latency_us; // 平均UNMAP延迟(微秒)
    uint64_t read_iops;          // 读IOPS
    uint64_t write_iops;         // 写IOPS
    uint64_t read_bw_mbps;       // 读带宽(MB/s)
    uint64_t write_bw_mbps;      // 写带宽(MB/s)
} xbdev_io_stats_t;

/**
 * 回调函数类型定义
 */
typedef void (*xbdev_cb)(void *arg, int rc);
typedef void (*xbdev_io_cb)(void *arg, int rc);
typedef void (*xbdev_batch_cb)(void *arg, int success_count, int total_count);

/**
 * 初始化libxbdev库
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_init(void);

/**
 * 清理libxbdev库
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_fini(void);

/**
 * 打开BDEV设备
 *
 * @param bdev_name BDEV设备名称
 * @param flags 打开标志(如XBDEV_OPEN_RDWR)
 * @return 成功返回非负的文件描述符，失败返回负的错误码
 */
int xbdev_open(const char *bdev_name, int flags);

/**
 * 关闭BDEV设备
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_close(int fd);

/**
 * 同步读取操作
 *
 * @param fd 文件描述符
 * @param buf 读取缓冲区
 * @param count 读取长度(字节)
 * @param offset 读取偏移(字节)
 * @return 成功返回读取的字节数，失败返回负的错误码
 */
ssize_t xbdev_read(int fd, void *buf, size_t count, uint64_t offset);

/**
 * 同步写入操作
 *
 * @param fd 文件描述符
 * @param buf 写入缓冲区
 * @param count 写入长度(字节)
 * @param offset 写入偏移(字节)
 * @return 成功返回写入的字节数，失败返回负的错误码
 */
ssize_t xbdev_write(int fd, const void *buf, size_t count, uint64_t offset);

/**
 * 异步读取操作
 *
 * @param fd 文件描述符
 * @param buf 读取缓冲区
 * @param count 读取长度(字节)
 * @param offset 读取偏移(字节)
 * @param cb_arg 回调参数
 * @param cb 完成回调函数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_read(int fd, void *buf, size_t count, uint64_t offset, void *cb_arg, xbdev_io_cb cb);

/**
 * 异步写入操作
 *
 * @param fd 文件描述符
 * @param buf 写入缓冲区
 * @param count 写入长度(字节)
 * @param offset 写入偏移(字节)
 * @param cb_arg 回调参数
 * @param cb 完成回调函数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_write(int fd, const void *buf, size_t count, uint64_t offset, void *cb_arg, xbdev_io_cb cb);

/**
 * 同步刷新操作
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_flush(int fd);

/**
 * 同步UNMAP操作
 *
 * @param fd 文件描述符
 * @param offset 起始偏移(字节)
 * @param length 长度(字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_unmap(int fd, uint64_t offset, uint64_t length);

/**
 * 同步设备重置操作
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_reset(int fd);

/**
 * 分配DMA对齐的内存缓冲区
 *
 * @param size 缓冲区大小
 * @return 成功返回缓冲区指针，失败返回NULL
 */
void *xbdev_dma_buffer_alloc(size_t size);

/**
 * 释放DMA对齐的内存缓冲区
 *
 * @param buf 缓冲区指针
 */
void xbdev_dma_buffer_free(void *buf);

/**
 * 同步比较操作
 *
 * @param fd 文件描述符
 * @param buf 比较缓冲区
 * @param count 比较长度(字节)
 * @param offset 比较偏移(字节)
 * @return 成功并且数据相同返回0，数据不同返回1，失败返回负的错误码
 */
int xbdev_compare(int fd, const void *buf, size_t count, uint64_t offset);

/**
 * 同步比较并写入操作
 *
 * @param fd 文件描述符
 * @param compare_buf 比较缓冲区
 * @param write_buf 写入缓冲区
 * @param count 操作长度(字节)
 * @param offset 操作偏移(字节)
 * @return 成功返回0，数据不匹配返回1，失败返回负的错误码
 */
int xbdev_compare_and_write(int fd, const void *compare_buf, const void *write_buf, 
                          size_t count, uint64_t offset);

/**
 * 同步写零操作
 *
 * @param fd 文件描述符
 * @param offset 起始偏移(字节)
 * @param length 长度(字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_write_zeroes(int fd, uint64_t offset, uint64_t length);

/**
 * 创建IO批处理
 *
 * @param max_ops 最大操作数
 * @return 成功返回批处理句柄，失败返回NULL
 */
xbdev_io_batch_t *xbdev_io_batch_create(int max_ops);

/**
 * 向批处理添加IO操作
 *
 * @param batch 批处理句柄
 * @param fd 文件描述符
 * @param buf 数据缓冲区
 * @param count 数据长度
 * @param offset 操作偏移量
 * @param type 操作类型(读/写)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_batch_add(xbdev_io_batch_t *batch, int fd, void *buf, size_t count, 
                      uint64_t offset, int type);

/**
 * 提交IO批处理
 *
 * @param batch 批处理句柄
 * @param cb_arg 回调参数
 * @param cb 完成回调函数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_io_batch_submit(xbdev_io_batch_t *batch, void *cb_arg, xbdev_batch_cb cb);

/**
 * 同步执行IO批处理
 *
 * @param batch 批处理句柄
 * @return 成功返回成功的操作数，失败返回负的错误码
 */
int xbdev_io_batch_execute(xbdev_io_batch_t *batch);

/**
 * 释放IO批处理
 *
 * @param batch 批处理句柄
 */
void xbdev_io_batch_free(xbdev_io_batch_t *batch);

/**
 * 轮询IO完成事件
 *
 * @param max_events 最大处理的事件数，0表示无限制
 * @param timeout_us 超时时间(微秒)，0表示非阻塞立即返回
 * @return 成功处理的事件数
 */
int xbdev_poll_completions(uint32_t max_events, uint64_t timeout_us);

/**
 * 从文件描述符获取设备名称
 *
 * @param fd 文件描述符
 * @param name 输出缓冲区
 * @param name_len 缓冲区长度
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_name(int fd, char *name, size_t name_len);

/**
 * 从文件描述符获取设备信息
 *
 * @param fd 文件描述符
 * @param info 输出参数，存储设备信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_device_info(int fd, xbdev_device_info_t *info);

/**
 * 获取设备IO统计信息
 *
 * @param fd 文件描述符
 * @param stats 输出参数，存储IO统计信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_io_stats(int fd, xbdev_io_stats_t *stats);

/**
 * 重置设备IO统计信息
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_reset_io_stats(int fd);

/**
 * 检查设备是否支持指定的IO类型
 *
 * @param fd 文件描述符
 * @param io_type IO类型
 * @return 如果支持返回true，否则返回false
 */
bool xbdev_io_type_supported(int fd, uint32_t io_type);

/**
 * 设置文件描述符的选项
 *
 * @param fd 文件描述符
 * @param option 选项代码
 * @param value 选项值
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_option(int fd, int option, uint64_t value);

/**
 * 获取文件描述符的选项
 *
 * @param fd 文件描述符
 * @param option 选项代码
 * @param value 选项值输出参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_option(int fd, int option, uint64_t *value);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化libxbdev库
 * 
 * @param config_file 可选的JSON配置文件路径，传NULL表示不使用配置文件
 * @return 成功返回0，失败返回错误码
 */
int xbdev_init(const char *config_file);

/**
 * @brief 清理libxbdev库
 * 
 * @return 成功返回0，失败返回错误码
 */
int xbdev_fini(void);

/**
 * @brief 打开BDEV设备
 * 
 * @param bdev_name 要打开的块设备名称
 * @return 成功返回文件描述符，失败返回负的错误码
 */
int xbdev_open(const char *bdev_name);

/**
 * @brief 关闭BDEV设备
 * 
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_close(int fd);

/**
 * @brief 同步读取数据
 * 
 * @param fd 文件描述符
 * @param buf 缓冲区指针
 * @param count 要读取的字节数
 * @param offset 设备内的偏移量
 * @return 成功返回读取的字节数，失败返回负的错误码
 */
ssize_t xbdev_read(int fd, void *buf, size_t count, uint64_t offset);

/**
 * @brief 同步写入数据
 * 
 * @param fd 文件描述符
 * @param buf 缓冲区指针
 * @param count 要写入的字节数
 * @param offset 设备内的偏移量
 * @return 成功返回写入的字节数，失败返回负的错误码
 */
ssize_t xbdev_write(int fd, const void *buf, size_t count, uint64_t offset);

/**
 * @brief 类型定义
 */
typedef struct xbdev_request_s xbdev_request_t;
typedef void (*xbdev_completion_cb)(void *arg, int rc);

/**
 * @brief RAID级别定义
 */
#define XBDEV_MD_LEVEL_LINEAR  0
#define XBDEV_MD_LEVEL_RAID0   1
#define XBDEV_MD_LEVEL_RAID1   2
#define XBDEV_MD_LEVEL_RAID4   4
#define XBDEV_MD_LEVEL_RAID5   5
#define XBDEV_MD_LEVEL_RAID6   6
#define XBDEV_MD_LEVEL_RAID10 10

/**
 * @brief RAID配置结构体
 */
typedef struct {
    uint64_t chunk_size_kb;       ///< 块大小（KB），对RAID0/5/6/10有效
    bool assume_clean;            ///< 是否假定磁盘为干净状态
    int layout;                   ///< RAID5/6布局算法
    int bitmap_chunk_kb;          ///< 位图块大小
    int write_behind;             ///< 写后缓存（数据块数）
    int stripe_cache_size;        ///< 条带缓存大小
    bool consistency_policy;      ///< 一致性策略
} xbdev_md_config_t;

/**
 * @brief RAID管理命令
 */
#define XBDEV_MD_ADD_DISK        1  ///< 添加磁盘
#define XBDEV_MD_REMOVE_DISK     2  ///< 移除磁盘
#define XBDEV_MD_ADD_SPARE       3  ///< 添加热备盘
#define XBDEV_MD_REMOVE_SPARE    4  ///< 移除热备盘
#define XBDEV_MD_REPLACE_DISK    5  ///< 替换磁盘
#define XBDEV_MD_SET_FAULTY      6  ///< 将磁盘标记为故障
#define XBDEV_MD_SET_READONLY    7  ///< 设置为只读
#define XBDEV_MD_SET_READWRITE   8  ///< 设置为读写
#define XBDEV_MD_START_REBUILD   9  ///< 开始重建
#define XBDEV_MD_STOP_REBUILD   10  ///< 停止重建
#define XBDEV_MD_SET_SYNC_SPEED 11  ///< 设置同步速度

/**
 * @brief RAID阵列状态
 */
#define XBDEV_MD_STATE_CLEAN       0  ///< 干净
#define XBDEV_MD_STATE_ACTIVE      1  ///< 活动
#define XBDEV_MD_STATE_DEGRADED    2  ///< 降级
#define XBDEV_MD_STATE_RESYNCING   3  ///< 重同步中
#define XBDEV_MD_STATE_RECOVERING  4  ///< 恢复中
#define XBDEV_MD_STATE_READONLY    5  ///< 只读
#define XBDEV_MD_STATE_FAILED      6  ///< 失败

/**
 * @brief RAID磁盘状态
 */
#define XBDEV_MD_DISK_ACTIVE       0  ///< 活动
#define XBDEV_MD_DISK_FAULTY       1  ///< 故障
#define XBDEV_MD_DISK_SPARE        2  ///< 热备
#define XBDEV_MD_DISK_SYNC         3  ///< 同步中
#define XBDEV_MD_DISK_REMOVED      4  ///< 已移除

/**
 * @brief RAID详细信息结构体
 */
typedef struct {
    int level;                     ///< RAID级别
    int raid_disks;                ///< 阵列磁盘数量
    int active_disks;              ///< 活动磁盘数量
    int working_disks;             ///< 工作中磁盘数量
    int failed_disks;              ///< 故障磁盘数量
    int spare_disks;               ///< 热备盘数量
    uint64_t size;                 ///< 阵列大小(KB)
    uint64_t chunk_size;           ///< 块大小(KB)
    int state;                     ///< 阵列状态
    char uuid[37];                 ///< 阵列UUID
    bool degraded;                 ///< 是否降级模式
    char resync_status[32];        ///< 重同步状态
    float resync_progress;         ///< 重建进度 0-100
    int resync_speed;              ///< 重建速度(KB/s)
    struct {
        int number;                ///< 磁盘号码
        char name[64];             ///< 磁盘名称
        int state;                 ///< 磁盘状态
        uint64_t size;             ///< 磁盘大小(KB)
        char role[16];             ///< 磁盘角色
    } disks[32];                   ///< 最多支持32个磁盘
} xbdev_md_detail_t;

/**
 * @brief 配置结构体(用于JSON解析)
 */
typedef struct {
    struct spdk_json_val *base_devices;
    struct spdk_json_val *raid_devices;
    struct spdk_json_val *lvol_stores;
    struct spdk_json_val *lvol_volumes;
    struct spdk_json_val *snapshots;
    struct spdk_json_val *clones;
} xbdev_config_t;

/**
 * @brief 通知回调类型定义
 */
typedef void (*xbdev_notification_cb)(int event_type, const char *device_name, void *event_data, void *cb_arg);

/**
 * @brief 创建RAID设备
 * 
 * @param name RAID设备名称
 * @param level RAID级别
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param config RAID配置，NULL表示使用默认配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create(const char *name, int level, const char **base_bdevs, int num_base_bdevs, xbdev_md_config_t *config);

/**
 * @brief 获取RAID设备详细信息
 * 
 * @param md_name RAID设备名称
 * @param detail 输出参数，存储详细信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_detail(const char *md_name, xbdev_md_detail_t *detail);

/**
 * @brief RAID管理操作
 * 
 * @param md_name RAID设备名称
 * @param cmd 管理命令
 * @param arg 命令参数，依命令类型而定
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_manage(const char *md_name, int cmd, void *arg);

/**
 * @brief 创建RAID0设备（条带化）
 * 
 * @param name RAID设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小（KB）
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid0(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb);

/**
 * @brief 创建RAID1设备（镜像）
 * 
 * @param name RAID设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid1(const char *name, const char **base_bdevs, int num_base_bdevs);

/**
 * @brief 创建RAID5设备（分布式奇偶校验）
 * 
 * @param name RAID设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小（KB）
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid5(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb);

/**
 * @brief 创建RAID10设备（RAID1+0）
 * 
 * @param name RAID设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小（KB）
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid10(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb);

/**
 * @brief 停止RAID设备
 * 
 * @param name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_stop(const char *name);

/**
 * @brief 创建LVOL存储池
 * 
 * @param bdev_name 基础设备名称
 * @param lvs_name LVOL存储池名称
 * @param cluster_size 集群大小，0表示使用默认值
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvs_create(const char *bdev_name, const char *lvs_name, uint64_t cluster_size);

/**
 * @brief 创建LVOL卷
 * 
 * @param lvs_name LVOL存储池名称
 * @param lvol_name LVOL卷名称
 * @param size_mb 卷大小（MB）
 * @param thin_provision 是否精简配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create(const char *lvs_name, const char *lvol_name, uint64_t size_mb, bool thin_provision);

/**
 * @brief 创建LVOL卷快照
 * 
 * @param lvol_name 源LVOL卷名称
 * @param snapshot_name 快照名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_snapshot_create(const char *lvol_name, const char *snapshot_name);

/**
 * @brief 解析JSON配置文件
 * 
 * @param json_file 配置文件路径
 * @return 成功返回0，失败返回错误码
 */
int xbdev_parse_config(const char *json_file);

/**
 * @brief 保存当前配置到JSON文件
 * 
 * @param json_file 配置文件路径
 * @return 成功返回0，失败返回错误码
 */
int xbdev_save_config(const char *json_file);

/**
 * @brief 基于JSON配置打开设备
 * 
 * @param json_file 配置文件路径
 * @return 成功返回0，失败返回错误码
 */
int xbdev_open_from_json(const char *json_file);

/**
 * @brief 基于JSON配置创建设备
 * 
 * @param json_file 配置文件路径
 * @return 成功返回0，失败返回错误码
 */
int xbdev_create_from_json(const char *json_file);

/**
 * @brief 初始化管理服务器
 * 
 * @param listen_addr 监听地址
 * @param port 端口号
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_server_init(const char *listen_addr, int port);

/**
 * @brief 清理管理服务器
 * 
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_server_fini(void);

/**
 * @brief 注册管理通知回调
 * 
 * @param cb 回调函数
 * @param cb_arg 回调参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_register_notification_callback(xbdev_notification_cb cb, void *cb_arg);

/**
 * @brief 执行管理命令
 * 
 * @param json_cmd JSON格式的命令
 * @param json_response 存储JSON响应的缓冲区
 * @param response_size 响应缓冲区大小
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_execute_cmd(const char *json_cmd, char *json_response, size_t response_size);

#ifdef __cplusplus
}
#endif

#endif /* _XBDEV_H_ */