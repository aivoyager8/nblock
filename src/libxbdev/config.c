/**
 * @file config.c
 * @brief Implements configuration file parsing and processing
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include <spdk/json.h>
#include <spdk/string.h>
#include <spdk/util.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Parse JSON configuration file
 */
int xbdev_parse_config(const char *json_file) {
    FILE *file;
    char *buf;
    long file_size;
    int rc = 0;
    
    if (!json_file) {
        XBDEV_ERRLOG("No configuration file path specified\n");
        return -EINVAL;
    }
    
    // Open configuration file
    file = fopen(json_file, "r");
    if (!file) {
        XBDEV_ERRLOG("Cannot open configuration file: %s\n", json_file);
        return -ENOENT;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    if (file_size <= 0) {
        XBDEV_ERRLOG("Configuration file is empty: %s\n", json_file);
        fclose(file);
        return -EINVAL;
    }
    
    // Allocate buffer and read file contents
    buf = malloc(file_size + 1);
    if (!buf) {
        XBDEV_ERRLOG("Memory allocation failed\n");
        fclose(file);
        return -ENOMEM;
    }
    
    if (fread(buf, 1, file_size, file) != (size_t)file_size) {
        XBDEV_ERRLOG("Failed to read configuration file: %s\n", json_file);
        free(buf);
        fclose(file);
        return -EIO;
    }
    
    buf[file_size] = '\0';
    fclose(file);
    
    // Parse JSON
    struct spdk_json_val *values = NULL;
    size_t num_values = 0;
    rc = spdk_json_parse(buf, file_size, NULL, 0, &num_values, 0);
    if (rc != 0 && rc != -ESTALE) {
        XBDEV_ERRLOG("Invalid JSON format in configuration file: %s\n", json_file);
        free(buf);
        return rc;
    }
    
    values = calloc(num_values, sizeof(*values));
    if (values == NULL) {
        XBDEV_ERRLOG("Memory allocation failed\n");
        free(buf);
        return -ENOMEM;
    }
    
    rc = spdk_json_parse(buf, file_size, values, num_values, NULL, 0);
    if (rc != 0) {
        XBDEV_ERRLOG("Failed to parse JSON: %s\n", json_file);
        free(values);
        free(buf);
        return rc;
    }
    
    // Start parsing and applying configuration
    rc = _xbdev_apply_config(values);
    
    free(values);
    free(buf);
    
    return rc;
}

/**
 * 应用JSON配置
 */
static int _xbdev_apply_config(struct spdk_json_val *values) {
    struct spdk_json_object_decoder decoders[] = {
        {"base_devices", offsetof(xbdev_config_t, base_devices), spdk_json_decode_array},
        {"raid_devices", offsetof(xbdev_config_t, raid_devices), spdk_json_decode_array},
        {"lvol_stores", offsetof(xbdev_config_t, lvol_stores), spdk_json_decode_array},
        {"lvol_volumes", offsetof(xbdev_config_t, lvol_volumes), spdk_json_decode_array},
        {"snapshots", offsetof(xbdev_config_t, snapshots), spdk_json_decode_array},
        {"clones", offsetof(xbdev_config_t, clones), spdk_json_decode_array}
    };
    
    // 创建配置结构体
    xbdev_config_t config = {0};
    
    // 解码JSON对象
    if (spdk_json_decode_object(values, decoders, SPDK_COUNTOF(decoders), &config)) {
        XBDEV_ERRLOG("解码JSON配置失败\n");
        _xbdev_free_config(&config);
        return -EINVAL;
    }
    
    // 按顺序应用配置
    int rc = 0;
    
    // 1. 配置基础设备
    rc = _xbdev_apply_base_devices(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("配置基础设备失败: %d\n", rc);
        _xbdev_free_config(&config);
        return rc;
    }
    
    // 2. 配置RAID设备
    rc = _xbdev_apply_raid_devices(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("配置RAID设备失败: %d\n", rc);
        _xbdev_free_config(&config);
        return rc;
    }
    
    // 3. 配置LVOL存储池
    rc = _xbdev_apply_lvol_stores(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("配置LVOL存储池失败: %d\n", rc);
        _xbdev_free_config(&config);
        return rc;
    }
    
    // 4. 配置LVOL卷
    rc = _xbdev_apply_lvol_volumes(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("配置LVOL卷失败: %d\n", rc);
        _xbdev_free_config(&config);
        return rc;
    }
    
    // 5. 配置快照
    rc = _xbdev_apply_snapshots(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("配置快照失败: %d\n", rc);
        _xbdev_free_config(&config);
        return rc;
    }
    
    // 6. 配置克隆
    rc = _xbdev_apply_clones(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("配置克隆失败: %d\n", rc);
        _xbdev_free_config(&config);
        return rc;
    }
    
    // 释放配置
    _xbdev_free_config(&config);
    
    XBDEV_NOTICELOG("成功应用配置文件\n");
    
    return 0;
}

/**
 * 保存当前配置到文件
 */
int xbdev_save_config(const char *json_file) {
    FILE *file;
    
    if (!json_file) {
        XBDEV_ERRLOG("未指定配置文件路径\n");
        return -EINVAL;
    }
    
    // 打开配置文件
    file = fopen(json_file, "w");
    if (!file) {
        XBDEV_ERRLOG("无法打开配置文件: %s\n", json_file);
        return -ENOENT;
    }
    
    // 收集当前配置
    xbdev_config_t config = {0};
    int rc = _xbdev_collect_current_config(&config);
    if (rc != 0) {
        XBDEV_ERRLOG("收集当前配置失败: %d\n", rc);
        fclose(file);
        return rc;
    }
    
    // 序列化为JSON
    struct spdk_json_write_ctx *w;
    w = spdk_json_write_begin(file, 2, false);
    if (!w) {
        XBDEV_ERRLOG("开始JSON写入失败\n");
        _xbdev_free_config(&config);
        fclose(file);
        return -ENOMEM;
    }
    
    // 写入配置
    _xbdev_write_config_json(w, &config);
    
    // 完成写入
    spdk_json_write_end(w);
    fclose(file);
    
    // 释放配置
    _xbdev_free_config(&config);
    
    XBDEV_NOTICELOG("成功保存配置到文件: %s\n", json_file);
    
    return 0;
}

/**
 * 基于JSON配置打开设备
 */
int xbdev_open_from_json(const char *json_file) {
    // 首先解析配置文件
    int rc = xbdev_parse_config(json_file);
    if (rc != 0) {
        XBDEV_ERRLOG("解析配置文件失败: %d\n", rc);
        return rc;
    }
    
    XBDEV_NOTICELOG("成功从配置文件打开设备: %s\n", json_file);
    
    return 0;
}

/**
 * 基于JSON配置创建设备
 */
int xbdev_create_from_json(const char *json_file) {
    // 与xbdev_open_from_json相同，只是用于强调创建过程
    return xbdev_open_from_json(json_file);
}
