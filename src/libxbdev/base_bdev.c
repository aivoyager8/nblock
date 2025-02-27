/**
 * @file base_bdev.c
 * @brief 实现基础BDEV设备的注册和管理
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/env.h>
#include <spdk/bdev.h>
#include <spdk/nvme.h>
#include <spdk/thread.h>
#include <spdk/string.h>
#include <spdk/util.h>
#include <spdk/rpc.h>
#include <spdk/jsonrpc.h>
#include <stdlib.h>
#include <string.h>

/**
 * NVMe设备注册上下文
 */
struct nvme_register_ctx {
    char *name;
    char *pci_addr;
    int rc;
    bool done;
};

/**
 * AIO文件设备注册上下文
 */
struct aio_register_ctx {
    char *name;
    char *filename;
    int rc;
    bool done;
};

/**
 * NVMe-oF设备注册上下文
 */
struct nvmf_register_ctx {
    char *name;
    char *addr;
    int port;
    char *nqn;
    int rc;
    bool done;
};

/**
 * Malloc内存设备注册上下文
 */
struct malloc_register_ctx {
    char *name;
    uint64_t size_mb;
    uint32_t block_size;
    int rc;
    bool done;
};

/**
 * 完成回调通用函数
 */
static void _complete_register(void *ctx, int rc)
{
    struct {
        bool *done;
        int *rc;
    } *args = ctx;
    
    *args->rc = rc;
    *args->done = true;
}

/**
 * 注册本地NVMe设备
 *
 * @param pci_addr PCIe地址，格式如"0000:01:00.0"
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_nvme(const char *pci_addr, const char *name)
{
    xbdev_request_t *req;
    struct nvme_register_ctx ctx = {0};
    int rc;
    
    if (!pci_addr || !name) {
        XBDEV_ERRLOG("Invalid parameters\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.name = strdup(name);
    ctx.pci_addr = strdup(pci_addr);
    ctx.rc = 0;
    ctx.done = false;
    
    if (!ctx.name || !ctx.pci_addr) {
        XBDEV_ERRLOG("Memory allocation failed\n");
        if (ctx.name) free(ctx.name);
        if (ctx.pci_addr) free(ctx.pci_addr);
        return -ENOMEM;
    }
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("Failed to allocate request\n");
        free(ctx.name);
        free(ctx.pci_addr);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_REGISTER_BDEV; // 假设已定义此请求类型
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to execute sync request: %d\n", rc);
        free(ctx.name);
        free(ctx.pci_addr);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to register NVMe device: %s (PCIe %s), rc=%d\n", name, pci_addr, rc);
        free(ctx.name);
        free(ctx.pci_addr);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放资源
    free(ctx.name);
    free(ctx.pci_addr);
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("Successfully registered NVMe device: %s (PCIe %s)\n", name, pci_addr);
    
    return 0;
}

/**
 * SPDK线程上下文中执行NVMe设备注册
 */
void xbdev_nvme_register_on_thread(void *ctx)
{
    struct nvme_register_ctx *args = ctx;
    struct {
        bool *done;
        int *rc;
    } cb_args = {
        .done = &args->done,
        .rc = &args->rc
    };
    
    // 构造NVMe控制器地址
    struct spdk_nvme_transport_id trid = {0};
    trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
    snprintf(trid.traddr, sizeof(trid.traddr), "%s", args->pci_addr);
    
    // 注册NVMe设备
    args->rc = spdk_bdev_nvme_create(&trid, args->name, NULL, 0, _complete_register, &cb_args);
    if (args->rc != 0) {
        XBDEV_ERRLOG("创建NVMe BDEV失败: %s (PCIe %s), rc=%d\n", 
                   args->name, args->pci_addr, args->rc);
        args->done = true;
        return;
    }
    
    // 等待操作完成
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 注册AIO文件设备
 *
 * @param filename 文件路径
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_aio(const char *filename, const char *name)
{
    xbdev_request_t *req;
    struct aio_register_ctx ctx = {0};
    int rc;
    
    if (!filename || !name) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.name = strdup(name);
    ctx.filename = strdup(filename);
    ctx.rc = 0;
    ctx.done = false;
    
    if (!ctx.name || !ctx.filename) {
        XBDEV_ERRLOG("内存分配失败\n");
        if (ctx.name) free(ctx.name);
        if (ctx.filename) free(ctx.filename);
        return -ENOMEM;
    }
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx.name);
        free(ctx.filename);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_REGISTER_BDEV;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        free(ctx.name);
        free(ctx.filename);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("注册AIO设备失败: %s (文件 %s), rc=%d\n", name, filename, rc);
        free(ctx.name);
        free(ctx.filename);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放资源
    free(ctx.name);
    free(ctx.filename);
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("成功注册AIO设备: %s (文件 %s)\n", name, filename);
    
    return 0;
}

/**
 * SPDK线程上下文中执行AIO设备注册
 */
void xbdev_aio_register_on_thread(void *ctx)
{
    struct aio_register_ctx *args = ctx;
    
    // 注册AIO文件设备
    args->rc = spdk_bdev_aio_create(args->name, args->filename, 512);
    if (args->rc != 0) {
        XBDEV_ERRLOG("创建AIO BDEV失败: %s (文件 %s), rc=%d\n", 
                   args->name, args->filename, args->rc);
    } else {
        XBDEV_NOTICELOG("成功创建AIO BDEV: %s (文件 %s)\n", 
                      args->name, args->filename);
    }
    
    args->done = true;
}

/**
 * 注册NVMe-oF设备
 *
 * @param addr 服务器IP地址
 * @param port 服务器端口
 * @param nqn NVMe Qualified Name
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_nvmf(const char *addr, int port, const char *nqn, const char *name)
{
    xbdev_request_t *req;
    struct nvmf_register_ctx ctx = {0};
    int rc;
    
    if (!addr || !nqn || !name || port <= 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.name = strdup(name);
    ctx.addr = strdup(addr);
    ctx.nqn = strdup(nqn);
    ctx.port = port;
    ctx.rc = 0;
    ctx.done = false;
    
    if (!ctx.name || !ctx.addr || !ctx.nqn) {
        XBDEV_ERRLOG("内存分配失败\n");
        if (ctx.name) free(ctx.name);
        if (ctx.addr) free(ctx.addr);
        if (ctx.nqn) free(ctx.nqn);
        return -ENOMEM;
    }
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx.name);
        free(ctx.addr);
        free(ctx.nqn);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_REGISTER_BDEV;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        free(ctx.name);
        free(ctx.addr);
        free(ctx.nqn);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("注册NVMe-oF设备失败: %s (%s:%d %s), rc=%d\n", 
                   name, addr, port, nqn, rc);
        free(ctx.name);
        free(ctx.addr);
        free(ctx.nqn);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放资源
    free(ctx.name);
    free(ctx.addr);
    free(ctx.nqn);
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("成功注册NVMe-oF设备: %s (%s:%d %s)\n", name, addr, port, nqn);
    
    return 0;
}

/**
 * SPDK线程上下文中执行NVMe-oF设备注册
 */
void xbdev_nvmf_register_on_thread(void *ctx)
{
    struct nvmf_register_ctx *args = ctx;
    struct {
        bool *done;
        int *rc;
    } cb_args = {
        .done = &args->done,
        .rc = &args->rc
    };
    
    // 构造NVMe-oF控制器地址
    struct spdk_nvme_transport_id trid = {0};
    trid.trtype = SPDK_NVME_TRANSPORT_TCP; // 假设使用TCP传输，也可以是RDMA
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    snprintf(trid.traddr, sizeof(trid.traddr), "%s", args->addr);
    snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%d", args->port);
    snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", args->nqn);
    
    // 注册NVMe-oF设备
    args->rc = spdk_bdev_nvme_create(&trid, args->name, NULL, 0, _complete_register, &cb_args);
    if (args->rc != 0) {
        XBDEV_ERRLOG("创建NVMe-oF BDEV失败: %s (%s:%d %s), rc=%d\n", 
                   args->name, args->addr, args->port, args->nqn, args->rc);
        args->done = true;
        return;
    }
    
    // 等待操作完成
    while (!args->done) {
        spdk_thread_poll(spdk_get_thread(), 0, 0);
    }
}

/**
 * 注册内存模拟块设备
 *
 * @param name 设备名称
 * @param size_mb 设备大小(MB)
 * @param block_size 块大小(字节)，0表示使用默认值(512字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_malloc(const char *name, uint64_t size_mb, uint32_t block_size)
{
    xbdev_request_t *req;
    struct malloc_register_ctx ctx = {0};
    int rc;
    
    if (!name || size_mb == 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 如果未指定块大小，使用默认值
    if (block_size == 0) {
        block_size = 512;
    }
    
    // 设置上下文
    ctx.name = strdup(name);
    ctx.size_mb = size_mb;
    ctx.block_size = block_size;
    ctx.rc = 0;
    ctx.done = false;
    
    if (!ctx.name) {
        XBDEV_ERRLOG("内存分配失败\n");
        return -ENOMEM;
    }
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx.name);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_REGISTER_BDEV;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        free(ctx.name);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("注册Malloc设备失败: %s (大小: %"PRIu64"MB), rc=%d\n", 
                   name, size_mb, rc);
        free(ctx.name);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放资源
    free(ctx.name);
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("成功注册Malloc设备: %s (大小: %"PRIu64"MB)\n", name, size_mb);
    
    return 0;
}

/**
 * SPDK线程上下文中执行Malloc设备注册
 */
void xbdev_malloc_register_on_thread(void *ctx)
{
    struct malloc_register_ctx *args = ctx;
    
    // 注册Malloc内存设备
    args->rc = spdk_bdev_create_malloc_disk(args->name, 
                                          args->size_mb * 1024 * 1024, // 转换为字节
                                          args->block_size);
    if (args->rc != 0) {
        XBDEV_ERRLOG("创建Malloc BDEV失败: %s (大小: %"PRIu64"MB), rc=%d\n", 
                   args->name, args->size_mb, args->rc);
    } else {
        XBDEV_NOTICELOG("成功创建Malloc BDEV: %s (大小: %"PRIu64"MB)\n", 
                      args->name, args->size_mb);
    }
    
    args->done = true;
}

/**
 * 注册Null设备(丢弃所有写入，返回零填充的读取)
 *
 * @param name 设备名称
 * @param size_mb 设备大小(MB)
 * @param block_size 块大小(字节)，0表示使用默认值(512字节)
 * @return 成功返回0，失败返回错误码
 */
int xbdev_config_null(const char *name, uint64_t size_mb, uint32_t block_size)
{
    xbdev_request_t *req;
    struct malloc_register_ctx ctx = {0};  // 重用malloc的上下文结构
    int rc;
    
    if (!name || size_mb == 0) {
        XBDEV_ERRLOG("无效的参数\n");
        return -EINVAL;
    }
    
    // 如果未指定块大小，使用默认值
    if (block_size == 0) {
        block_size = 512;
    }
    
    // 设置上下文
    ctx.name = strdup(name);
    ctx.size_mb = size_mb;
    ctx.block_size = block_size;
    ctx.rc = 0;
    ctx.done = false;
    
    if (!ctx.name) {
        XBDEV_ERRLOG("内存分配失败\n");
        return -ENOMEM;
    }
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        free(ctx.name);
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_REGISTER_BDEV;
    req->ctx = &ctx;
    
    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        free(ctx.name);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 检查操作结果
    rc = ctx.rc;
    if (rc != 0) {
        XBDEV_ERRLOG("注册Null设备失败: %s (大小: %"PRIu64"MB), rc=%d\n", 
                   name, size_mb, rc);
        free(ctx.name);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放资源
    free(ctx.name);
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("成功注册Null设备: %s (大小: %"PRIu64"MB)\n", name, size_mb);
    
    return 0;
}

/**
 * SPDK线程上下文中执行Null设备注册
 */
void xbdev_null_register_on_thread(void *ctx)
{
    struct malloc_register_ctx *args = ctx;
    
    // 注册Null设备
    args->rc = spdk_bdev_create_null_disk(args->name, 
                                        args->size_mb * 1024 * 1024, // 转换为字节
                                        args->block_size);
    if (args->rc != 0) {
        XBDEV_ERRLOG("创建Null BDEV失败: %s (大小: %"PRIu64"MB), rc=%d\n", 
                   args->name, args->size_mb, args->rc);
    } else {
        XBDEV_NOTICELOG("成功创建Null BDEV: %s (大小: %"PRIu64"MB)\n", 
                      args->name, args->size_mb);
    }
    
    args->done = true;
}

/**
 * 移除设备
 *
 * @param name 设备名称
 * @return 成功返回0，失败返回错误码
 */
int xbdev_device_remove(const char *name)
{
    xbdev_request_t *req;
    struct {
        const char *name;
        int rc;
        bool done;
    } ctx = {0};
    int rc;
    
    if (!name) {
        XBDEV_ERRLOG("无效的设备名称\n");
        return -EINVAL;
    }
    
    // 设置上下文
    ctx.name = name;
    ctx.rc = 0;
    ctx.done = false;
    
    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("无法分配请求\n");
        return -ENOMEM;
    }
    
    // 设置请求
    req->type = XBDEV_REQ_REMOVE_BDEV;
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
        XBDEV_ERRLOG("移除设备失败: %s, rc=%d\n", name, rc);
        xbdev_sync_request_free(req);
        return rc;
    }
    
    // 释放资源
    xbdev_sync_request_free(req);
    
    XBDEV_NOTICELOG("成功移除设备: %s\n", name);
    
    return 0;
}

/**
 * SPDK线程上下文中执行设备移除
 */
void xbdev_device_remove_on_thread(void *ctx)
{
    struct {
        const char *name;
        int rc;
        bool done;
    } *args = ctx;
    
    // 查找设备
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(args->name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到设备: %s\n", args->name);
        args->rc = -ENODEV;
        args->done = true;
        return;
    }
    
    // 获取设备的模块
    const struct spdk_bdev_module *module = spdk_bdev_get_module(bdev);
    if (!module) {
        XBDEV_ERRLOG("找不到设备模块: %s\n", args->name);
        args->rc = -EINVAL;
        args->done = true;
        return;
    }
    
    // 根据设备类型执行不同的删除操作
    const char *module_name = spdk_bdev_get_module_name(bdev);
    
    if (strcmp(module_name, "nvme") == 0) {
        // 删除NVMe设备
        args->rc = spdk_bdev_nvme_delete(args->name);
    } else if (strcmp(module_name, "aio") == 0) {
        // 删除AIO设备
        args->rc = spdk_bdev_aio_delete(args->name);
    } else if (strcmp(module_name, "malloc") == 0) {
        // 删除Malloc设备
        args->rc = spdk_bdev_delete_malloc_disk(bdev);
    } else if (strcmp(module_name, "null") == 0) {
        // 删除Null设备
        args->rc = spdk_bdev_delete_null_disk(bdev);
    } else {
        // 尝试通用删除方式
        XBDEV_WARNLOG("未知设备类型: %s, 模块: %s，尝试通用删除方法\n", args->name, module_name);
        args->rc = -ENOTSUP;
    }
    
    if (args->rc != 0) {
        XBDEV_ERRLOG("删除设备失败: %s, rc=%d\n", args->name, args->rc);
    } else {
        XBDEV_NOTICELOG("成功删除设备: %s\n", args->name);
    }
    
    args->done = true;
}

/**
 * 应用基础设备配置
 *
 * @param config 配置对象
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_apply_base_devices(xbdev_config_t *config)
{
    int rc = 0;
    
    // 检查配置有效性
    if (!config || !config->base_devices) {
        return 0; // 没有基础设备配置，直接返回成功
    }
    
    // 遍历基础设备配置项
    struct spdk_json_val *val = config->base_devices + 1;  // 跳过数组头
    for (uint32_t i = 0; i < config->base_devices->len; i++) {
        // 确保当前值是对象
        if (val->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
            XBDEV_ERRLOG("基础设备配置项 #%d 不是对象\n", i);
            return -EINVAL;
        }
        
        // 解析配置项
        char *method = NULL;
        struct spdk_json_val *params = NULL;
        
        struct spdk_json_object_decoder decoders[] = {
            {"method", offsetof(struct { char *method; }, method), spdk_json_decode_string},
            {"params", offsetof(struct { struct spdk_json_val *params; }, params), spdk_json_decode_object}
        };
        
        if (spdk_json_decode_object(val, decoders, SPDK_COUNTOF(decoders), &method, &params)) {
            XBDEV_ERRLOG("无法解析基础设备配置项 #%d\n", i);
            if (method) free(method);
            return -EINVAL;
        }
        
        // 根据方法类型应用配置
        if (strcmp(method, "nvme") == 0) {
            // NVMe设备配置
            char *name = NULL;
            char *pci_addr = NULL;
            
            struct spdk_json_object_decoder nvme_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"pci_addr", offsetof(struct { char *pci_addr; }, pci_addr), spdk_json_decode_string}
            };
            
            if (spdk_json_decode_object(params, nvme_decoders, SPDK_COUNTOF(nvme_decoders), &name, &pci_addr)) {
                XBDEV_ERRLOG("无法解析NVMe设备参数\n");
                free(method);
                if (name) free(name);
                if (pci_addr) free(pci_addr);
                return -EINVAL;
            }
            
            // 注册NVMe设备
            rc = xbdev_config_nvme(pci_addr, name);
            
            free(name);
            free(pci_addr);
            
        } else if (strcmp(method, "aio") == 0) {
            // AIO设备配置
            char *name = NULL;
            char *filename = NULL;
            
            struct spdk_json_object_decoder aio_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"filename", offsetof(struct { char *filename; }, filename), spdk_json_decode_string}
            };
            
            if (spdk_json_decode_object(params, aio_decoders, SPDK_COUNTOF(aio_decoders), &name, &filename)) {
                XBDEV_ERRLOG("无法解析AIO设备参数\n");
                free(method);
                if (name) free(name);
                if (filename) free(filename);
                return -EINVAL;
            }
            
            // 注册AIO设备
            rc = xbdev_config_aio(filename, name);
            
            free(name);
            free(filename);
            
        } else if (strcmp(method, "nvmf") == 0) {
            // NVMe-oF设备配置
            char *name = NULL;
            char *addr = NULL;
            char *nqn = NULL;
            int port = 0;
            
            struct spdk_json_object_decoder nvmf_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"addr", offsetof(struct { char *addr; }, addr), spdk_json_decode_string},
                {"port", offsetof(struct { int port; }, port), spdk_json_decode_int32},
                {"nqn", offsetof(struct { char *nqn; }, nqn), spdk_json_decode_string}
            };
            
            if (spdk_json_decode_object(params, nvmf_decoders, SPDK_COUNTOF(nvmf_decoders), &name, &addr, &port, &nqn)) {
                XBDEV_ERRLOG("无法解析NVMe-oF设备参数\n");
                free(method);
                if (name) free(name);
                if (addr) free(addr);
                if (nqn) free(nqn);
                return -EINVAL;
            }
            
            // 注册NVMe-oF设备
            rc = xbdev_config_nvmf(addr, port, nqn, name);
            
            free(name);
            free(addr);
            free(nqn);
            
        } else if (strcmp(method, "malloc") == 0) {
            // Malloc设备配置
            char *name = NULL;
            uint64_t size_mb = 0;
            uint32_t block_size = 0;
            
            struct spdk_json_object_decoder malloc_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"size_mb", offsetof(struct { uint64_t size_mb; }, size_mb), spdk_json_decode_uint64},
                {"block_size", offsetof(struct { uint32_t block_size; }, block_size), spdk_json_decode_uint32}
            };
            
            if (spdk_json_decode_object(params, malloc_decoders, SPDK_COUNTOF(malloc_decoders), &name, &size_mb, &block_size)) {
                XBDEV_ERRLOG("无法解析Malloc设备参数\n");
                free(method);
                if (name) free(name);
                return -EINVAL;
            }
            
            // 注册Malloc设备
            rc = xbdev_config_malloc(name, size_mb, block_size);
            
            free(name);
            
        } else if (strcmp(method, "null") == 0) {
            // Null设备配置
            char *name = NULL;
            uint64_t size_mb = 0;
            uint32_t block_size = 0;
            
            struct spdk_json_object_decoder null_decoders[] = {
                {"name", offsetof(struct { char *name; }, name), spdk_json_decode_string},
                {"size_mb", offsetof(struct { uint64_t size_mb; }, size_mb), spdk_json_decode_uint64},
                {"block_size", offsetof(struct { uint32_t block_size; }, block_size), spdk_json_decode_uint32}
            };
            
            if (spdk_json_decode_object(params, null_decoders, SPDK_COUNTOF(null_decoders), &name, &size_mb, &block_size)) {
                XBDEV_ERRLOG("无法解析Null设备参数\n");
                free(method);
                if (name) free(name);
                return -EINVAL;
            }
            
            // 注册Null设备
            rc = xbdev_config_null(name, size_mb, block_size);
            
            free(name);
            
        } else {
            XBDEV_ERRLOG("未知的基础设备配置方法: %s\n", method);
            free(method);
            return -EINVAL;
        }
        
        free(method);
        
        // 检查注册结果
        if (rc != 0) {
            return rc;
        }
        
        // 移动到下一个配置项
        val = spdk_json_next(val);
    }
    
    return 0;
}