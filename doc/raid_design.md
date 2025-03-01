# RAID模块设计文档

## 1. 概述

RAID (Redundant Array of Independent Disks) 模块为libxbdev库提供了软件RAID功能,允许用户创建、管理和操作不同类型的RAID设备。该模块基于SPDK的RAID功能实现,提供类似Linux MD设备的使用体验。

## 2. 设计目标

- 提供类似Linux MD设备的RAID管理接口
- 支持核心RAID级别 (RAID0, RAID1, RAID5, Linear) 
- 提供热备盘和故障恢复能力
- 支持设备状态监控和事件通知
- 基于JSON的配置管理
- 与LVOL模块集成

## 3. 核心功能

### 3.1 支持的RAID级别

- Linear (串联) - 简单合并多个设备
- RAID0 (条带) - 提供最高性能
- RAID1 (镜像) - 提供可靠性
- RAID5 (分布式奇偶校验) - 平衡性能和可靠性
- RAID10 (通过RAID1+0组合实现)

### 3.2 核心功能模块

1. RAID设备创建和管理
   - 创建不同级别的RAID设备
   - 组装已有RAID设备
   - 停止和移除RAID设备

2. 磁盘管理
   - 添加/移除磁盘
   - 标记磁盘故障
   - 替换故障磁盘
   - 管理热备盘

3. 状态监控
   - 监控RAID设备状态
   - 跟踪重建进度
   - 生成事件通知

4. 故障恢复
   - 自动故障检测
   - 自动启用热备盘
   - 支持手动替换磁盘

## 4. 架构设计

### 4.1 模块层次结构

### 4.2 关键组件

1. RAID管理API
   - 提供设备创建/删除接口
   - 提供状态查询接口
   - 提供磁盘管理接口

2. RAID状态管理器
   - 维护RAID设备状态
   - 处理设备事件
   - 管理热备盘策略

3. RAID IO处理器
   - 实现各RAID级别的IO路径
   - 处理重建和同步

4. SPDK RAID适配
   - 适配SPDK的RAID接口
   - 转换内部数据结构

## 5. 核心数据结构

### 5.1 RAID信息结构
```c
typedef struct {
    int level;                     // RAID级别
    int raid_disks;                // 阵列磁盘数量
    int active_disks;              // 活动磁盘数量
    int failed_disks;              // 故障磁盘数量
    int spare_disks;               // 热备盘数量
    uint64_t size;                 // 阵列大小(KB)
    uint64_t chunk_size;           // 块大小(KB)
    int state;                     // 阵列状态
    bool degraded;                 // 是否降级模式
    float resync_progress;         // 重建进度
    struct {
        int number;                // 磁盘号码
        char name[64];             // 磁盘名称
        int state;                 // 磁盘状态
        uint64_t size;             // 磁盘大小
    } disks[32];                   // 成员磁盘
} xbdev_md_detail_t;

// 创建RAID设备
int xbdev_md_create(const char *name, int level, 
                    const char **base_bdevs, int num_base_bdevs,
                    xbdev_md_config_t *config);

// 组装已有RAID设备                     
int xbdev_md_assemble(const char *name, int level,
                      const char **base_bdevs, int num_base_bdevs);

// 停止RAID设备
int xbdev_md_stop(const char *name);

// 获取RAID详细信息
int xbdev_md_detail(const char *md_name, xbdev_md_detail_t *detail);

