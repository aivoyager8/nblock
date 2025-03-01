/**
 * @file device_list.c
 * @brief 实现设备发现和监控功能
 *
 * 该文件实现设备发现、监控和热插拔检测功能，
 * 包括设备状态变更通知机制。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/bdev.h>
#include <spdk/event.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/**
 * 设备列表条目
 */
typedef struct device_list_entry {
    char name[256];                  // 设备名称
    xbdev_device_info_t info;        // 设备信息
    bool online;                     // 是否在线
    struct device_list_entry *next;  // 链表下一项
} device_list_entry_t;

/**
 * 全局设备列表
 */
static struct {
    device_list_entry_t *head;       // 链表头
    pthread_mutex_t mutex;           // 互斥锁
    bool monitor_running;            // 监控是否运行
    xbdev_device_event_cb event_cb;  // 事件回调函数
    void *cb_arg;                    // 回调参数
    uint32_t scan_interval_ms;       // 扫描间隔(毫秒)
    uint64_t last_scan_time;         // 上次扫描时间
} g_device_list = {0};

/**
 * 初始化设备列表
 */
int xbdev_device_list_init(void)
{
    // 初始化互斥锁
    if (pthread_mutex_init(&g_device_list.mutex, NULL) != 0) {
        XBDEV_ERRLOG("初始化设备列表互斥锁失败\n");
        return -1;
    }
    
    g_device_list.head = NULL;
    g_device_list.monitor_running = false;
    g_device_list.event_cb = NULL;
    g_device_list.cb_arg = NULL;
    g_device_list.scan_interval_ms = 1000; // 默认1秒扫描一次
    
    return 0;
}

/**
 * 清理设备列表
 */
void xbdev_device_list_fini(void)
{
    pthread_mutex_lock(&g_device_list.mutex);
    
    // 停止监控
    g_device_list.monitor_running = false;
    
    // 释放链表
    device_list_entry_t *entry = g_device_list.head;
    while (entry) {
        device_list_entry_t *next = entry->next;
        free(entry);
        entry = next;
    }
    
    g_device_list.head = NULL;
    g_device_list.event_cb = NULL;
    g_device_list.cb_arg = NULL;
    
    pthread_mutex_unlock(&g_device_list.mutex);
    pthread_mutex_destroy(&g_device_list.mutex);
}

/**
 * 查找设备条目
 * 
 * @param name 设备名称
 * @return 找到返回条目指针，否则返回NULL
 * @note 调用前必须获取互斥锁
 */
static device_list_entry_t *find_device_entry(const char *name)
{
    device_list_entry_t *entry = g_device_list.head;
    
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

/**
 * 添加设备到列表
 * 
 * @param name 设备名称
 * @param info 设备信息
 * @return 成功返回0，失败返回错误码
 */
static int add_device_entry(const char *name, const xbdev_device_info_t *info)
{
    device_list_entry_t *entry;
    
    // 检查是否已存在
    if (find_device_entry(name) != NULL) {
        return 0; // 已存在，不需要添加
    }
    
    // 分配新条目
    entry = malloc(sizeof(device_list_entry_t));
    if (!entry) {
        XBDEV_ERRLOG("内存分配失败\n");
        return -ENOMEM;
    }
    
    // 初始化条目
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    
    if (info) {
        memcpy(&entry->info, info, sizeof(xbdev_device_info_t));
    } else {
        memset(&entry->info, 0, sizeof(xbdev_device_info_t));
        strncpy(entry->info.name, name, sizeof(entry->info.name) - 1);
    }
    
    entry->online = true;
    
    // 添加到链表头
    entry->next = g_device_list.head;
    g_device_list.head = entry;
    
    // 触发事件回调
    if (g_device_list.event_cb) {
        pthread_mutex_unlock(&g_device_list.mutex);
        g_device_list.event_cb(XBDEV_EVENT_DEVICE_ADDED, name, &entry->info, g_device_list.cb_arg);
        pthread_mutex_lock(&g_device_list.mutex);
    }
    
    return 0;
}

/**
 * 标记设备为离线
 * 
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
static int mark_device_offline(const char *name)
{
    device_list_entry_t *entry = find_device_entry(name);
    
    if (!entry) {
        return -ENOENT;
    }
    
    if (entry->online) {
        entry->online = false;
        
        // 触发事件回调
        if (g_device_list.event_cb) {
            pthread_mutex_unlock(&g_device_list.mutex);
            g_device_list.event_cb(XBDEV_EVENT_DEVICE_REMOVED, name, &entry->info, g_device_list.cb_arg);
            pthread_mutex_lock(&g_device_list.mutex);
        }
    }
    
    return 0;
}

/**
 * 标记设备为在线
 * 
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
static int mark_device_online(const char *name)
{
    device_list_entry_t *entry = find_device_entry(name);
    
    if (!entry) {
        return -ENOENT;
    }
    
    if (!entry->online) {
        entry->online = true;
        
        // 触发事件回调
        if (g_device_list.event_cb) {
            pthread_mutex_unlock(&g_device_list.mutex);
            g_device_list.event_cb(XBDEV_EVENT_DEVICE_ADDED, name, &entry->info, g_device_list.cb_arg);
            pthread_mutex_lock(&g_device_list.mutex);
        }
    }
    
    return 0;
}

/**
 * 扫描设备并更新列表
 */
int xbdev_scan_devices(void)
{
    struct spdk_bdev *bdev;
    char **current_devices = NULL;
    int max_devices = 256; // 最大设备数
    int device_count = 0;
    int rc = 0;
    
    // 分配当前设备名称数组
    current_devices = calloc(max_devices, sizeof(char *));
    if (!current_devices) {
        XBDEV_ERRLOG("内存分配失败\n");
        return -ENOMEM;
    }
    
    // 遍历所有BDEV，记录当前设备名称
    for (bdev = spdk_bdev_first(); bdev != NULL; bdev = spdk_bdev_next(bdev)) {
        if (device_count >= max_devices) {
            break;
        }
        
        const char *name = spdk_bdev_get_name(bdev);
        if (name) {
            current_devices[device_count] = strdup(name);
            if (!current_devices[device_count]) {
                XBDEV_ERRLOG("内存分配失败\n");
                rc = -ENOMEM;
                goto cleanup;
            }
            device_count++;
        }
    }
    
    // 锁定设备列表
    pthread_mutex_lock(&g_device_list.mutex);
    
    // 第一步：标记所有现有设备为离线
    device_list_entry_t *entry = g_device_list.head;
    while (entry) {
        entry->online = false;
        entry = entry->next;
    }
    
    // 第二步：处理当前设备
    for (int i = 0; i < device_count; i++) {
        const char *name = current_devices[i];
        
        // 获取设备信息
        xbdev_device_info_t info;
        if (xbdev_get_device_info(name, &info) == 0) {
            // 查找设备是否已在列表中
            entry = find_device_entry(name);
            
            if (entry) {
                // 设备已存在，标记为在线并更新信息
                entry->online = true;
                memcpy(&entry->info, &info, sizeof(xbdev_device_info_t));
            } else {
                // 新设备，添加到列表
                add_device_entry(name, &info);
            }
        }
    }
    
    // 第三步：触发离线设备的移除事件
    entry = g_device_list.head;
    device_list_entry_t *prev = NULL;
    
    while (entry) {
        if (!entry->online) {
            // 设备已离线，触发事件
            if (g_device_list.event_cb) {
                pthread_mutex_unlock(&g_device_list.mutex);
                g_device_list.event_cb(XBDEV_EVENT_DEVICE_REMOVED, entry->name, &entry->info, g_device_list.cb_arg);
                pthread_mutex_lock(&g_device_list.mutex);
            }
            
            // 从链表中移除
            if (prev) {
                prev->next = entry->next;
                device_list_entry_t *to_free = entry;
                entry = entry->next;
                free(to_free);
            } else {
                g_device_list.head = entry->next;
                device_list_entry_t *to_free = entry;
                entry = entry->next;
                free(to_free);
            }
        } else {
            prev = entry;
            entry = entry->next;
        }
    }
    
    // 释放互斥锁
    pthread_mutex_unlock(&g_device_list.mutex);
    
cleanup:
    // 释放设备名称数组
    for (int i = 0; i < device_count; i++) {
        free(current_devices[i]);
    }
    free(current_devices);
    
    return rc;
}

/**
 * 获取设备列表条目数量
 * 
 * @return 条目数量
 */
int xbdev_get_device_count(void)
{
    int count = 0;
    
    pthread_mutex_lock(&g_device_list.mutex);
    
    device_list_entry_t *entry = g_device_list.head;
    while (entry) {
        count++;
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_device_list.mutex);
    
    return count;
}

/**
 * 监控回调函数
 * 
 * @param arg 未使用
 */
static void device_monitor_poll(void *arg)
{
    // 检查是否应该扫描设备
    uint64_t now = spdk_get_ticks();
    uint64_t interval_ticks = g_device_list.scan_interval_ms * spdk_get_ticks_hz() / 1000;
    
    if ((now - g_device_list.last_scan_time) >= interval_ticks) {
        xbdev_scan_devices();
        g_device_list.last_scan_time = now;
    }
}

/**
 * 注册设备事件回调
 * 
 * @param cb 回调函数
 * @param cb_arg 回调函数的用户参数
 * @return 成功返回0，失败返回错误码
 */
int xbdev_register_device_event_callback(xbdev_device_event_cb cb, void *cb_arg)
{
    pthread_mutex_lock(&g_device_list.mutex);
    
    g_device_list.event_cb = cb;
    g_device_list.cb_arg = cb_arg;
    
    pthread_mutex_unlock(&g_device_list.mutex);
    
    return 0;
}

/**
 * 设置设备扫描间隔
 * 
 * @param interval_ms 扫描间隔(毫秒)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_set_device_scan_interval(uint32_t interval_ms)
{
    if (interval_ms == 0) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_device_list.mutex);
    g_device_list.scan_interval_ms = interval_ms;
    pthread_mutex_unlock(&g_device_list.mutex);
    
    return 0;
}

/**
 * 启动设备监控
 * 
 * @param interval_ms 扫描间隔(毫秒)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_start_device_monitoring(uint32_t interval_ms)
{
    pthread_mutex_lock(&g_device_list.mutex);
    
    if (g_device_list.monitor_running) {
        pthread_mutex_unlock(&g_device_list.mutex);
        return 0; // 已经在运行
    }
    
    // 设置扫描间隔
    if (interval_ms > 0) {
        g_device_list.scan_interval_ms = interval_ms;
    }
    
    // 立即进行一次扫描
    g_device_list.last_scan_time = spdk_get_ticks();
    
    g_device_list.monitor_running = true;
    
    pthread_mutex_unlock(&g_device_list.mutex);
    
    // 启动初次扫描
    xbdev_scan_devices();
    
    return 0;
}

/**
 * 停止设备监控
 */
void xbdev_stop_device_monitoring(void)
{
    pthread_mutex_lock(&g_device_list.mutex);
    g_device_list.monitor_running = false;
    pthread_mutex_unlock(&g_device_list.mutex);
}

/**
 * 检查设备是否存在
 * 
 * @param name 设备名称
 * @return 存在返回true，否则返回false
 */
bool xbdev_device_exists(const char *name)
{
    bool exists = false;
    
    pthread_mutex_lock(&g_device_list.mutex);
    
    device_list_entry_t *entry = find_device_entry(name);
    exists = (entry != NULL && entry->online);
    
    pthread_mutex_unlock(&g_device_list.mutex);
    
    return exists;
}