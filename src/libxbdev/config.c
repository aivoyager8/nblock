/**
 * @file config.c
 * @brief 实现配置文件解析和应用
 *
 * 该文件实现JSON配置文件的解析、验证和应用，用于统一配置设备和存储系统。
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/util.h>
#include <spdk/string.h>
#include <spdk/json.h>
#include <spdk/jsonrpc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/**
 * JSON解析上下文
 */
struct json_parse_ctx {
    xbdev_config_t config;
    int rc;
};

/**
 * JSON配置应用上下文
 */
struct json_apply_ctx {
    xbdev_config_t *config;
    bool done;
    int rc;
};

/**
 * 读取JSON文件内容到缓冲区
 *
 * @param filename 文件名
 * @param buf 输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 成功返回读取的字节数，失败返回-1
 */
static ssize_t read_file_contents(const char *filename, char *buf, size_t buf_size)
{
    int fd;
    ssize_t total_read = 0;
    ssize_t n;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        XBDEV_ERRLOG("无法打开配置文件: %s\n", filename);
        return -1;
    }

    while (total_read < buf_size) {
        n = read(fd, buf + total_read, buf_size - total_read);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            XBDEV_ERRLOG("读取配置文件失败: %s\n", filename);
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total_read += n;
    }

    close(fd);
    return total_read;
}

/**
 * 解析基础设备配置
 *
 * @param ctx 解析上下文
 * @param values JSON配置值
 */
static void parse_base_devices(struct json_parse_ctx *ctx, struct spdk_json_val *values)
{
    ctx->config.base_devices = values;
    XBDEV_INFOLOG("解析基础设备配置成功\n");
}

/**
 * 解析RAID设备配置
 *
 * @param ctx 解析上下文
 * @param values JSON配置值
 */
static void parse_raid_devices(struct json_parse_ctx *ctx, struct spdk_json_val *values)
{
    ctx->config.raid_devices = values;
    XBDEV_INFOLOG("解析RAID设备配置成功\n");
}

/**
 * 解析LVOL存储池配置
 *
 * @param ctx 解析上下文
 * @param values JSON配置值
 */
static void parse_lvol_stores(struct json_parse_ctx *ctx, struct spdk_json_val *values)
{
    ctx->config.lvol_stores = values;
    XBDEV_INFOLOG("解析LVOL存储池配置成功\n");
}

/**
 * 解析LVOL卷配置
 *
 * @param ctx 解析上下文
 * @param values JSON配置值
 */
static void parse_lvol_volumes(struct json_parse_ctx *ctx, struct spdk_json_val *values)
{
    ctx->config.lvol_volumes = values;
    XBDEV_INFOLOG("解析LVOL卷配置成功\n");
}

/**
 * 解析快照配置
 *
 * @param ctx 解析上下文
 * @param values JSON配置值
 */
static void parse_snapshots(struct json_parse_ctx *ctx, struct spdk_json_val *values)
{
    ctx->config.snapshots = values;
    XBDEV_INFOLOG("解析快照配置成功\n");
}

/**
 * 解析克隆配置
 *
 * @param ctx 解析上下文
 * @param values JSON配置值
 */
static void parse_clones(struct json_parse_ctx *ctx, struct spdk_json_val *values)
{
    ctx->config.clones = values;
    XBDEV_INFOLOG("解析克隆配置成功\n");
}

/**
 * 解析JSON配置对象
 *
 * @param ctx 解析上下文
 * @param values JSON值数组
 * @return 成功返回0，失败返回错误码
 */
static int parse_json_config(struct json_parse_ctx *ctx, struct spdk_json_val *values)
{
    if (values->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
        XBDEV_ERRLOG("JSON配置必须是对象\n");
        return -EINVAL;
    }

    struct spdk_json_object_decoder decoders[] = {
        {"base_devices", parse_base_devices, &ctx->config.base_devices},
        {"raid_devices", parse_raid_devices, &ctx->config.raid_devices, true},
        {"lvol_stores", parse_lvol_stores, &ctx->config.lvol_stores, true},
        {"lvol_volumes", parse_lvol_volumes, &ctx->config.lvol_volumes, true},
        {"snapshots", parse_snapshots, &ctx->config.snapshots, true},
        {"clones", parse_clones, &ctx->config.clones, true}
    };

    return spdk_json_decode_object(values, decoders, SPDK_COUNTOF(decoders), ctx);
}

/**
 * 解析JSON文本
 *
 * @param json_text JSON文本
 * @param json_len 文本长度
 * @param config 输出参数，配置结构
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_parse_json(const char *json_text, size_t json_len, xbdev_config_t *config)
{
    struct json_parse_ctx ctx = {0};
    struct spdk_json_val *values = NULL;
    size_t values_cnt;
    int rc;

    // 解析JSON文本为值数组
    rc = spdk_json_parse(json_text, json_len, NULL, 0, &values_cnt, 0);
    if (rc != 0) {
        XBDEV_ERRLOG("计算JSON值数量失败: %d\n", rc);
        return rc;
    }

    values = calloc(values_cnt, sizeof(struct spdk_json_val));
    if (!values) {
        XBDEV_ERRLOG("分配JSON值数组内存失败\n");
        return -ENOMEM;
    }

    rc = spdk_json_parse(json_text, json_len, values, values_cnt, NULL, 0);
    if (rc != 0) {
        XBDEV_ERRLOG("解析JSON失败: %d\n", rc);
        free(values);
        return rc;
    }

    // 解析JSON配置对象
    memset(config, 0, sizeof(*config));
    rc = parse_json_config(&ctx, values);
    if (rc != 0) {
        XBDEV_ERRLOG("解析JSON配置失败: %d\n", rc);
        free(values);
        return rc;
    }

    // 成功解析
    memcpy(config, &ctx.config, sizeof(*config));
    XBDEV_NOTICELOG("解析JSON配置成功\n");
    
    return 0;
}

/**
 * 释放配置结构
 *
 * @param config 配置结构
 * @return 成功返回0，失败返回错误码
 */
int _xbdev_free_config(xbdev_config_t *config)
{
    if (!config) {
        return -EINVAL;
    }

    // 目前不需要特殊释放操作，因为我们只存储了JSON值的引用
    memset(config, 0, sizeof(*config));
    
    return 0;
}

/**
 * 在SPDK线程上下文中应用配置
 */
void xbdev_apply_config_on_thread(void *arg)
{
    struct json_apply_ctx *ctx = arg;
    int rc;

    // 依次应用各部分配置
    rc = _xbdev_apply_base_devices(ctx->config);
    if (rc != 0) {
        XBDEV_ERRLOG("应用基础设备配置失败: %d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }

    rc = _xbdev_apply_raid_devices(ctx->config);
    if (rc != 0) {
        XBDEV_ERRLOG("应用RAID设备配置失败: %d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }

    rc = _xbdev_apply_lvol_stores(ctx->config);
    if (rc != 0) {
        XBDEV_ERRLOG("应用LVOL存储池配置失败: %d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }

    rc = _xbdev_apply_lvol_volumes(ctx->config);
    if (rc != 0) {
        XBDEV_ERRLOG("应用LVOL卷配置失败: %d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }

    rc = _xbdev_apply_snapshots(ctx->config);
    if (rc != 0) {
        XBDEV_ERRLOG("应用快照配置失败: %d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }

    rc = _xbdev_apply_clones(ctx->config);
    if (rc != 0) {
        XBDEV_ERRLOG("应用克隆配置失败: %d\n", rc);
        ctx->rc = rc;
        ctx->done = true;
        return;
    }

    XBDEV_NOTICELOG("应用所有配置成功\n");
    ctx->rc = 0;
    ctx->done = true;
}

/**
 * 解析JSON配置文件
 *
 * @param json_file JSON配置文件路径
 * @return 成功返回0，失败返回错误码
 */
int xbdev_parse_config(const char *json_file)
{
    char *buf;
    ssize_t len;
    struct stat st;
    xbdev_config_t config = {0};
    xbdev_request_t *req;
    struct json_apply_ctx ctx = {0};
    int rc;

    if (!json_file) {
        XBDEV_ERRLOG("无效的JSON文件路径\n");
        return -EINVAL;
    }

    // 获取文件大小
    if (stat(json_file, &st) != 0) {
        XBDEV_ERRLOG("无法获取文件状态: %s\n", json_file);
        return -errno;
    }

    // 分配缓冲区
    buf = malloc(st.st_size + 1);
    if (!buf) {
        XBDEV_ERRLOG("内存分配失败\n");
        return -ENOMEM;
    }

    // 读取文件内容
    len = read_file_contents(json_file, buf, st.st_size);
    if (len < 0) {
        XBDEV_ERRLOG("读取文件失败: %s\n", json_file);
        free(buf);
        return -EIO;
    }
    buf[len] = '\0';

    // 解析JSON文本
    rc = _xbdev_parse_json(buf, len, &config);
    if (rc != 0) {
        XBDEV_ERRLOG("解析JSON配置失败: %d\n", rc);
        free(buf);
        return rc;
    }
    
    // 释放文件缓冲区
    free(buf);

    // 设置应用上下文
    ctx.config = &config;
    ctx.done = false;
    ctx.rc = 0;

    // 分配请求
    req = xbdev_sync_request_alloc();
    if (!req) {
        XBDEV_ERRLOG("分配请求失败\n");
        _xbdev_free_config(&config);
        return -ENOMEM;
    }

    // 设置请求类型
    req->type = XBDEV_REQ_CUSTOM;
    req->ctx = &ctx;

    // 提交请求并等待完成
    rc = xbdev_sync_request_execute(req);
    if (rc != 0) {
        XBDEV_ERRLOG("执行同步请求失败: %d\n", rc);
        xbdev_sync_request_free(req);
        _xbdev_free_config(&config);
        return rc;
    }

    // 检查应用结果
    rc = ctx.rc;

    // 释放请求
    xbdev_sync_request_free(req);

    // 释放配置
    _xbdev_free_config(&config);

    return rc;
}

/**
 * 保存当前配置到JSON文件
 *
 * @param json_file JSON配置文件路径
 * @return 成功返回0，失败返回错误码
 */
int xbdev_save_config(const char *json_file)
{
    xbdev_config_t config = {0};
    struct spdk_json_write_ctx *w;
    FILE *f;
    int rc;

    if (!json_file) {
        XBDEV_ERRLOG("无效的JSON文件路径\n");
        return -EINVAL;
    }

    // 收集当前配置
    rc = _xbdev_collect_current_config(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("收集当前配置失败: %d\n", rc);
        return rc;
    }

    // 打开输出文件
    f = fopen(json_file, "w");
    if (!f) {
        XBDEV_ERRLOG("无法打开输出文件: %s\n", json_file);
        _xbdev_free_config(&config);
        return -errno;
    }

    // 创建JSON写入上下文
    w = spdk_json_write_begin(spdk_json_write_file, f, 0);
    if (!w) {
        XBDEV_ERRLOG("创建JSON写入上下文失败\n");
        fclose(f);
        _xbdev_free_config(&config);
        return -ENOMEM;
    }

    // 写入配置
    _xbdev_write_config_json(w, &config);

    // 完成写入
    spdk_json_write_end(w);
    fclose(f);

    // 释放配置
    _xbdev_free_config(&config);

    XBDEV_NOTICELOG("保存配置到文件成功: %s\n", json_file);
    
    return 0;
}

/**
 * 基于JSON配置创建设备
 *
 * @param json_file JSON配置文件路径
 * @return 成功返回0，失败返回错误码
 */
int xbdev_create_from_json(const char *json_file)
{
    // 基本上和xbdev_parse_config相同，但会对设备执行创建而非打开操作
    return xbdev_parse_config(json_file);
}
