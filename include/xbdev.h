/**
 * @file xbdev.h
 * @brief 主头文件，定义了libxbdev的公共API
 *
 * 此头文件是libxbdev库的主要API接口定义，包含了所有用户可用的函数、
 * 结构体和常量定义。
 */

#ifndef _XBDEV_H_
#define _XBDEV_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 版本定义
 */
#define XBDEV_VERSION_MAJOR 1
#define XBDEV_VERSION_MINOR 0
#define XBDEV_VERSION_PATCH 0

/**
 * 错误码定义
 */
#define XBDEV_SUCCESS      0
#define XBDEV_ERROR      -1
#define XBDEV_EAGAIN     -2
#define XBDEV_EINVAL     -3
#define XBDEV_ENOMEM     -4
#define XBDEV_EIO        -5

/**
 * 设备类型定义
 */
typedef enum {
    XBDEV_DEV_TYPE_NVME,     // NVMe设备
    XBDEV_DEV_TYPE_AIO,      // AIO设备
    XBDEV_DEV_TYPE_RBD,      // RBD设备
    XBDEV_DEV_TYPE_LVOL,     // 逻辑卷设备
    XBDEV_DEV_TYPE_RAM,      // 内存设备
    XBDEV_DEV_TYPE_OTHER     // 其他设备
} xbdev_device_type_t;

/**
 * IO能力标志
 */
#define XBDEV_IO_CAP_READ              (1ULL << 0)
#define XBDEV_IO_CAP_WRITE             (1ULL << 1)
#define XBDEV_IO_CAP_FLUSH             (1ULL << 2)
#define XBDEV_IO_CAP_UNMAP             (1ULL << 3)
#define XBDEV_IO_CAP_RESET             (1ULL << 4)
#define XBDEV_IO_CAP_COMPARE           (1ULL << 5)
#define XBDEV_IO_CAP_COMPARE_AND_WRITE (1ULL << 6)
#define XBDEV_IO_CAP_WRITE_ZEROES      (1ULL << 7)
#define XBDEV_IO_CAP_DIF               (1ULL << 8)

/**
 * IO操作类型
 */
typedef enum {
    XBDEV_IO_TYPE_READ,      // 读操作
    XBDEV_IO_TYPE_WRITE,     // 写操作
    XBDEV_IO_TYPE_FLUSH,     // 刷新操作
    XBDEV_IO_TYPE_UNMAP,     // UNMAP操作
    XBDEV_IO_TYPE_RESET      // 重置操作
} xbdev_io_type_t;

/**
 * IO完成回调函数类型
 *
 * @param fd 文件描述符
 * @param status 完成状态(>=0表示成功，<0表示错误)
 * @param ctx 用户上下文
 */
typedef void (*xbdev_io_completion_cb)(int fd, int status, void *ctx);

/**
 * 重试回调函数类型
 */
typedef bool (*xbdev_io_retry_cb)(void *retry_info, void *arg);

/**
 * 请求结构体
 */
typedef struct {
    uint64_t req_id;                 // 请求ID
    xbdev_req_type_t type;           // 请求类型
    xbdev_req_status_t status;       // 请求状态
    int result;                       // 结果码
    bool sync_req;                   // 是否为同步请求
    bool done;                       // 是否已完成
    void *ctx;                       // 请求上下文
    xbdev_io_completion_cb cb;       // 完成回调
    void *cb_arg;                    // 回调参数
    uint64_t submit_tsc;             // 提交时间戳
    uint64_t complete_tsc;           // 完成时间戳
    uint64_t reserved[4];            // 保留字段
} xbdev_request_t;

/**
 * 设备信息结构体
 */
typedef struct {
    char name[256];                  // 设备名称
    char product_name[256];          // 产品名称
    uint64_t block_size;             // 块大小
    uint64_t num_blocks;             // 块数量
    uint64_t size_bytes;             // 总大小(字节)
    bool write_cache;                // 是否启用写缓存
    uint32_t md_size;                // 元数据大小(每块)
    uint32_t optimal_io_boundary;    // 最优IO边界
    uint32_t required_alignment;     // 所需对齐
    bool claimed;                    // 是否被占用
    uint32_t supported_io_types;     // 支持的IO类型
    char driver[64];                 // 驱动名称
} xbdev_device_info_t;

/**
 * LVOL卷信息结构体
 */
typedef struct {
    char lvs_name[256];              // 存储池名称
    char lvol_name[256];             // 逻辑卷名称
    uint64_t size;                   // 大小(字节)
    bool thin_provision;             // 是否精简配置
    bool snapshot;                   // 是否为快照
    bool clone;                      // 是否为克隆
    uint64_t cluster_size;           // 簇大小
    uint64_t allocated_size;         // 已分配大小
    char uuid[64];                   // UUID
} xbdev_lvol_info_t;

/**
 * LVOL存储池信息结构体
 */
typedef struct {
    char pool_name[256];             // 池名称
    uint64_t cluster_size;           // 簇大小
    uint64_t total_clusters;         // 总簇数
    uint64_t free_clusters;          // 可用簇数
    char bdev_name[256];             // 基础设备名称
    char uuid[64];                   // UUID
} xbdev_lvol_pool_info_t;

/**
 * RAID详细信息结构体
 */
typedef struct {
    int level;                       // RAID级别
    int raid_disks;                  // 阵列磁盘数量
    int active_disks;                // 活动磁盘数量
    int working_disks;               // 工作中磁盘数量
    int failed_disks;                // 故障磁盘数量
    int spare_disks;                 // 热备盘数量
    uint64_t size;                   // 阵列大小(KB)
    uint64_t chunk_size;             // 块大小(KB)
    int state;                       // 阵列状态
    char uuid[37];                   // 阵列UUID
    bool degraded;                   // 是否降级模式
    char resync_status[32];          // 重同步状态
    float resync_progress;           // 重建进度 0-100
    int resync_speed;                // 重建速度(KB/s)
    struct {
        int number;                  // 磁盘号码
        char name[64];               // 磁盘名称
        int state;                   // 磁盘状态
        uint64_t size;               // 磁盘大小(KB)
        char role[16];               // 磁盘角色
    } disks[32];                     // 最多支持32个磁盘
} xbdev_md_detail_t;

/**
 * RAID配置结构体
 */
typedef struct {
    uint64_t chunk_size_kb;          // 块大小（KB），对RAID0/5/6/10有效
    bool assume_clean;               // 是否假定磁盘为干净状态
    int layout;                      // RAID5/6布局算法
} xbdev_md_config_t;

/**
 * RAID状态定义
 */
#define XBDEV_MD_STATE_CLEAN       0  // 干净
#define XBDEV_MD_STATE_ACTIVE      1  // 活动
#define XBDEV_MD_STATE_DEGRADED    2  // 降级
#define XBDEV_MD_STATE_RESYNCING   3  // 重同步中
#define XBDEV_MD_STATE_RECOVERING  4  // 恢复中
#define XBDEV_MD_STATE_READONLY    5  // 只读
#define XBDEV_MD_STATE_FAILED      6  // 失败

/**
 * 磁盘状态定义
 */
#define XBDEV_MD_DISK_ACTIVE       0  // 活动
#define XBDEV_MD_DISK_FAULTY       1  // 故障
#define XBDEV_MD_DISK_SPARE        2  // 热备
#define XBDEV_MD_DISK_SYNC         3  // 同步中
#define XBDEV_MD_DISK_REMOVED      4  // 已移除

/**
 * 初始化libxbdev库
 *
 * 初始化SPDK环境、线程模型和请求处理系统。
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_init(void);

/**
 * 清理libxbdev库
 *
 * 释放所有资源并清理SPDK环境。
 *
 * @return 成功返回0，失败返回错误码
 */
int xbdev_fini(void);

/**
 * 设置轮询线程的延迟时间
 *
 * @param delay_us 延迟时间(微秒)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_reactor_delay(int delay_us);

/**
 * 打开BDEV设备
 *
 * @param bdev_name 设备名称
 * @return 成功返回文件描述符，失败返回-1
 */
int xbdev_open(const char *bdev_name);

/**
 * 关闭BDEV设备
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_close(int fd);

/**
 * 同步读取数据
 *
 * @param fd 文件描述符
 * @param buf 输出缓冲区
 * @param count 读取字节数
 * @param offset 偏移量
 * @return 成功返回读取的字节数，失败返回错误码
 */
ssize_t xbdev_read(int fd, void *buf, size_t count, uint64_t offset);

/**
 * 同步写入数据
 *
 * @param fd 文件描述符
 * @param buf 输入缓冲区
 * @param count 写入字节数
 * @param offset 偏移量
 * @return 成功返回写入的字节数，失败返回错误码
 */
ssize_t xbdev_write(int fd, const void *buf, size_t count, uint64_t offset);

/**
 * 向量读取数据
 *
 * @param fd 文件描述符
 * @param iov IO向量数组
 * @param iovcnt 向量数量
 * @param offset 偏移量
 * @return 成功返回读取的字节数，失败返回错误码
 */
ssize_t xbdev_readv(int fd, const struct iovec *iov, int iovcnt, uint64_t offset);

/**
 * 向量写入数据
 *
 * @param fd 文件描述符
 * @param iov IO向量数组
 * @param iovcnt 向量数量
 * @param offset 偏移量
 * @return 成功返回写入的字节数，失败返回错误码
 */
ssize_t xbdev_writev(int fd, const struct iovec *iov, int iovcnt, uint64_t offset);

/**
 * 异步读取数据
 *
 * @param fd 文件描述符
 * @param buf 输出缓冲区
 * @param count 读取字节数
 * @param offset 偏移量
 * @param cb 完成回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_read(int fd, void *buf, size_t count, uint64_t offset,
                  xbdev_io_completion_cb cb, void *cb_arg);

/**
 * 异步写入数据
 *
 * @param fd 文件描述符
 * @param buf 输入缓冲区
 * @param count 写入字节数
 * @param offset 偏移量
 * @param cb 完成回调函数
 * @param cb_arg 回调函数参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_aio_write(int fd, const void *buf, size_t count, uint64_t offset,
                   xbdev_io_completion_cb cb, void *cb_arg);

/**
 * 刷新设备缓存
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_flush(int fd);

/**
 * 释放设备空间(TRIM/UNMAP操作)
 *
 * @param fd 文件描述符
 * @param offset 偏移量
 * @param length 长度
 * @return 成功返回0，失败返回错误码
 */
int xbdev_unmap(int fd, uint64_t offset, uint64_t length);

/**
 * 重置设备
 *
 * @param fd 文件描述符
 * @return 成功返回0，失败返回错误码
 */
int xbdev_reset(int fd);

/**
 * 获取设备列表
 *
 * @param names 输出参数，存储设备名称的数组
 * @param max_names 数组大小
 * @return 成功返回找到的设备数量，失败返回错误码
 */
int xbdev_get_device_list(char **names, int max_names);

/**
 * 获取设备信息
 *
 * @param name 设备名称
 * @param info 输出参数，存储设备信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_get_device_info(const char *name, xbdev_device_info_t *info);

/**
 * 注册本地NVMe设备
 *
 * @param pci_addr PCIe地址，格式如"0000:01:00.0"
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_nvme(const char *pci_addr, const char *name);

/**
 * 注册NVMe-oF远程设备
 *
 * @param addr 服务器IP地址
 * @param port 服务器端口
 * @param nqn NVMe Qualified Name
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_nvmf(const char *addr, int port, const char *nqn, const char *name);

/**
 * 注册AIO文件设备
 *
 * @param filename 文件路径
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_aio(const char *filename, const char *name);

/**
 * 注册内存模拟块设备
 *
 * @param name 设备名称
 * @param size_mb 设备大小(MB)
 * @param block_size 块大小(字节)，0表示使用默认值(512字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_malloc(const char *name, uint64_t size_mb, uint32_t block_size);

/**
 * 移除设备
 *
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_device_remove(const char *name);

/**
 * 创建LVOL存储池
 *
 * @param bdev_name 基础设备名称
 * @param pool_name 池名称
 * @param cluster_size 簇大小，0表示使用默认值
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create_pool(const char *bdev_name, const char *pool_name, uint64_t cluster_size);

/**
 * 删除LVOL存储池
 *
 * @param pool_name 池名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_destroy_pool(const char *pool_name);

/**
 * 创建LVOL卷
 *
 * @param pool_name 池名称
 * @param lvol_name 卷名称
 * @param size_mb 卷大小(MB)
 * @param thin_provision 是否启用精简配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create(const char *pool_name, const char *lvol_name, uint64_t size_mb, bool thin_provision);

/**
 * 删除LVOL卷
 *
 * @param lvol_name 卷名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_destroy(const char *lvol_name);

/**
 * 创建快照
 *
 * @param lvol_name 源卷名称
 * @param snapshot_name 快照名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create_snapshot(const char *lvol_name, const char *snapshot_name);

/**
 * 创建克隆卷
 *
 * @param snapshot_name 快照名称
 * @param clone_name 克隆卷名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_create_clone(const char *snapshot_name, const char *clone_name);

/**
 * 重命名LVOL卷
 *
 * @param old_name 旧名称
 * @param new_name 新名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_rename(const char *old_name, const char *new_name);

/**
 * 调整LVOL卷大小
 *
 * @param lvol_name 卷名称
 * @param new_size_mb 新大小(MB)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_resize(const char *lvol_name, uint64_t new_size_mb);

/**
 * 获取LVOL卷信息
 *
 * @param lvol_name 卷名称
 * @param info 卷信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_get_info(const char *lvol_name, xbdev_lvol_info_t *info);

/**
 * 获取LVOL存储池信息
 *
 * @param pool_name 池名称
 * @param info 池信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_lvol_get_pool_info(const char *pool_name, xbdev_lvol_pool_info_t *info);

/**
 * 创建RAID设备
 *
 * @param name 设备名称
 * @param level RAID级别
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param config RAID配置参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create(const char *name, int level, const char **base_bdevs, int num_base_bdevs, xbdev_md_config_t *config);

/**
 * 组装已有的RAID设备
 *
 * @param name 设备名称
 * @param level RAID级别
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_assemble(const char *name, int level, const char **base_bdevs, int num_base_bdevs);

/**
 * 停止RAID设备
 *
 * @param name RAID设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_stop(const char *name);

/**
 * 获取RAID设备详细信息
 *
 * @param md_name RAID设备名称
 * @param detail RAID详细信息
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_detail(const char *md_name, xbdev_md_detail_t *detail);

/**
 * 创建RAID0设备
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小(KB)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid0(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb);

/**
 * 创建RAID1设备
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid1(const char *name, const char **base_bdevs, int num_base_bdevs);

/**
 * 创建RAID5设备
 *
 * @param name 设备名称
 * @param base_bdevs 基础设备名称数组
 * @param num_base_bdevs 基础设备数量
 * @param chunk_size_kb 块大小(KB)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_md_create_raid5(const char *name, const char **base_bdevs, int num_base_bdevs, uint64_t chunk_size_kb);

/**
 * 队列初始化和清理
 */
int xbdev_queue_init(uint32_t queue_size, uint32_t num_queues);
void xbdev_queue_fini(void);

/**
 * 队列操作
 */
xbdev_request_t* xbdev_request_alloc(void);
xbdev_request_t* xbdev_sync_request_alloc(void);
int xbdev_request_free(xbdev_request_t *req);
int xbdev_request_submit(xbdev_request_t *req);
int xbdev_sync_request_execute(xbdev_request_t *req);
int xbdev_response_receive(xbdev_request_t *req);

/**
 * JSON配置文件解析
 */
int xbdev_parse_config(const char *json_file);
int xbdev_save_config(const char *json_file);
int xbdev_open_from_json(const char *json_file);
int xbdev_create_from_json(const char *json_file);

/**
 * 内存管理函数
 */
void* xbdev_dma_malloc(size_t size, size_t align);
void xbdev_dma_free(void *buf);

#ifdef __cplusplus
}
#endif

#endif // _XBDEV_H_
