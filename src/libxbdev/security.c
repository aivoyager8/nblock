/**
 * @file security.c
 * @brief 实现存储安全功能，包括加密和访问控制
 */

#include "xbdev.h"
#include "xbdev_internal.h"
#include "xbdev_security.h"
#include <spdk/thread.h>
#include <spdk/log.h>
#include <spdk/util.h>
#include <spdk/bdev.h>
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <json-c/json.h>

// 加密上下文结构
typedef struct {
    xbdev_encryption_algorithm_t algorithm;
    uint8_t key[64];            // 加密密钥
    uint32_t key_size;          // 密钥大小（字节）
    uint8_t iv[16];             // 初始化向量
    EVP_CIPHER_CTX *ctx;        // OpenSSL加密上下文
    uint32_t sector_size;       // 扇区大小
    bool enabled;               // 是否启用
} xbdev_crypto_ctx_t;

// 卷加密表
#define XBDEV_MAX_ENCRYPTED_VOLS 64
static struct {
    char lvol_name[64];
    xbdev_crypto_ctx_t crypto_ctx;
    bool in_use;
} g_encrypted_vols[XBDEV_MAX_ENCRYPTED_VOLS];
static pthread_mutex_t g_crypto_mutex = PTHREAD_MUTEX_INITIALIZER;

// 访问控制配置
static xbdev_access_control_t g_access_control = {
    .mode = XBDEV_ACCESS_MODE_OPEN
};
static pthread_mutex_t g_access_control_mutex = PTHREAD_MUTEX_INITIALIZER;

// 令牌结构
typedef struct {
    char username[64];
    char token[256];
    time_t expiry;
    bool in_use;
} xbdev_token_t;

// 令牌管理
#define XBDEV_MAX_TOKENS 256
static xbdev_token_t g_tokens[XBDEV_MAX_TOKENS];
static pthread_mutex_t g_token_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * 初始化安全模块
 */
int xbdev_security_init(void) {
    memset(g_encrypted_vols, 0, sizeof(g_encrypted_vols));
    memset(g_tokens, 0, sizeof(g_tokens));
    pthread_mutex_init(&g_crypto_mutex, NULL);
    pthread_mutex_init(&g_access_control_mutex, NULL);
    pthread_mutex_init(&g_token_mutex, NULL);
    
    // 初始化OpenSSL
    OpenSSL_add_all_algorithms();
    
    XBDEV_NOTICELOG("安全模块已初始化\n");
    return 0;
}

/**
 * 清理安全模块
 */
void xbdev_security_fini(void) {
    pthread_mutex_lock(&g_crypto_mutex);
    
    // 清理所有加密上下文
    for (int i = 0; i < XBDEV_MAX_ENCRYPTED_VOLS; i++) {
        if (g_encrypted_vols[i].in_use && g_encrypted_vols[i].crypto_ctx.ctx) {
            EVP_CIPHER_CTX_free(g_encrypted_vols[i].crypto_ctx.ctx);
            g_encrypted_vols[i].crypto_ctx.ctx = NULL;
        }
    }
    
    pthread_mutex_unlock(&g_crypto_mutex);
    
    // 清理OpenSSL
    EVP_cleanup();
    
    // 销毁互斥锁
    pthread_mutex_destroy(&g_crypto_mutex);
    pthread_mutex_destroy(&g_access_control_mutex);
    pthread_mutex_destroy(&g_token_mutex);
    
    XBDEV_NOTICELOG("安全模块已清理\n");
}

/**
 * 查找卷加密上下文
 */
static xbdev_crypto_ctx_t *find_crypto_ctx(const char *lvol_name) {
    for (int i = 0; i < XBDEV_MAX_ENCRYPTED_VOLS; i++) {
        if (g_encrypted_vols[i].in_use && 
            strcmp(g_encrypted_vols[i].lvol_name, lvol_name) == 0) {
            return &g_encrypted_vols[i].crypto_ctx;
        }
    }
    return NULL;
}

/**
 * 分配新的卷加密上下文
 */
static xbdev_crypto_ctx_t *alloc_crypto_ctx(const char *lvol_name) {
    for (int i = 0; i < XBDEV_MAX_ENCRYPTED_VOLS; i++) {
        if (!g_encrypted_vols[i].in_use) {
            g_encrypted_vols[i].in_use = true;
            strncpy(g_encrypted_vols[i].lvol_name, lvol_name, sizeof(g_encrypted_vols[i].lvol_name) - 1);
            return &g_encrypted_vols[i].crypto_ctx;
        }
    }
    return NULL;
}

/**
 * 释放卷加密上下文
 */
static void free_crypto_ctx(const char *lvol_name) {
    for (int i = 0; i < XBDEV_MAX_ENCRYPTED_VOLS; i++) {
        if (g_encrypted_vols[i].in_use && 
            strcmp(g_encrypted_vols[i].lvol_name, lvol_name) == 0) {
            if (g_encrypted_vols[i].crypto_ctx.ctx) {
                EVP_CIPHER_CTX_free(g_encrypted_vols[i].crypto_ctx.ctx);
            }
            memset(&g_encrypted_vols[i], 0, sizeof(g_encrypted_vols[i]));
            break;
        }
    }
}

/**
 * 从passphrase派生加密密钥
 */
static int derive_key(const char *passphrase, xbdev_key_derive_method_t method,
                     const uint8_t *salt, uint32_t salt_length, 
                     uint32_t iterations, uint8_t *key, uint32_t key_size) {
    switch (method) {
        case XBDEV_KEY_DERIVE_NONE:
            // 直接使用passphrase作为密钥（通常不推荐）
            strncpy((char *)key, passphrase, key_size);
            return 0;
            
        case XBDEV_KEY_DERIVE_PBKDF2:
            // 使用PBKDF2算法
            if (!PKCS5_PBKDF2_HMAC(passphrase, strlen(passphrase),
                                 salt, salt_length,
                                 iterations, EVP_sha256(),
                                 key_size, key)) {
                return -EINVAL;
            }
            return 0;
            
        case XBDEV_KEY_DERIVE_SCRYPT:
            // scrypt暂未实现
            XBDEV_ERRLOG("scrypt密钥派生方法未实现\n");
            return -ENOTSUP;
            
        case XBDEV_KEY_DERIVE_ARGON2ID:
            // Argon2id暂未实现
            XBDEV_ERRLOG("Argon2id密钥派生方法未实现\n");
            return -ENOTSUP;
            
        default:
            XBDEV_ERRLOG("未知的密钥派生方法: %d\n", method);
            return -EINVAL;
    }
}

/**
 * 初始化加密上下文
 */
static int init_crypto_ctx(xbdev_crypto_ctx_t *ctx, xbdev_encryption_algorithm_t algorithm,
                          const uint8_t *key, uint32_t key_size, uint32_t sector_size) {
    const EVP_CIPHER *cipher = NULL;
    
    // 选择加密算法
    switch (algorithm) {
        case XBDEV_ENCRYPTION_AES_XTS_128:
            cipher = EVP_aes_128_xts();
            break;
        case XBDEV_ENCRYPTION_AES_XTS_256:
            cipher = EVP_aes_256_xts();
            break;
        case XBDEV_ENCRYPTION_AES_CBC_256:
            cipher = EVP_aes_256_cbc();
            break;
        case XBDEV_ENCRYPTION_SM4:
            // SM4暂未实现
            XBDEV_ERRLOG("SM4加密算法未实现\n");
            return -ENOTSUP;
        case XBDEV_ENCRYPTION_CUSTOM:
            // 自定义加密暂未实现
            XBDEV_ERRLOG("自定义加密算法未实现\n");
            return -ENOTSUP;
        default:
            XBDEV_ERRLOG("无效的加密算法: %d\n", algorithm);
            return -EINVAL;
    }
    
    if (!cipher) {
        XBDEV_ERRLOG("无法获取加密算法\n");
        return -EINVAL;
    }
    
    // 创建加密上下文
    ctx->ctx = EVP_CIPHER_CTX_new();
    if (!ctx->ctx) {
        XBDEV_ERRLOG("无法创建加密上下文\n");
        return -ENOMEM;
    }
    
    // 设置加密参数
    ctx->algorithm = algorithm;
    memcpy(ctx->key, key, key_size);
    ctx->key_size = key_size;
    ctx->sector_size = sector_size;
    ctx->enabled = true;
    
    // 生成随机初始化向量
    if (RAND_bytes(ctx->iv, sizeof(ctx->iv)) != 1) {
        XBDEV_ERRLOG("生成随机IV失败\n");
        EVP_CIPHER_CTX_free(ctx->ctx);
        ctx->ctx = NULL;
        return -EINVAL;
    }
    
    return 0;
}

/**
 * 为卷启用加密
 */
int xbdev_encryption_enable(const char *lvol_name, const xbdev_encryption_config_t *config) {
    int rc;
    xbdev_crypto_ctx_t *crypto_ctx;
    uint8_t key[64] = {0};
    
    if (!lvol_name || !config) {
        return -EINVAL;
    }
    
    // 检查卷是否存在
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(lvol_name);
    if (!bdev) {
        XBDEV_ERRLOG("找不到卷: %s\n", lvol_name);
        return -ENOENT;
    }
    
    // 检查算法是否支持
    if (config->algorithm <= XBDEV_ENCRYPTION_NONE || 
        config->algorithm >= XBDEV_ENCRYPTION_CUSTOM) {
        XBDEV_ERRLOG("不支持的加密算法: %d\n", config->algorithm);
        return -ENOTSUP;
    }
    
    pthread_mutex_lock(&g_crypto_mutex);
    
    // 检查卷是否已经加密
    if (find_crypto_ctx(lvol_name) != NULL) {
        XBDEV_ERRLOG("卷已加密: %s\n", lvol_name);
        pthread_mutex_unlock(&g_crypto_mutex);
        return -EEXIST;
    }
    
    // 分配加密上下文
    crypto_ctx = alloc_crypto_ctx(lvol_name);
    if (!crypto_ctx) {
        XBDEV_ERRLOG("无法分配加密上下文: %s\n", lvol_name);
        pthread_mutex_unlock(&g_crypto_mutex);
        return -ENOMEM;
    }
    
    // 派生加密密钥
    uint32_t key_size = (config->algorithm == XBDEV_ENCRYPTION_AES_XTS_128) ? 16 : 32;
    rc = derive_key(config->passphrase, config->key_method, 
                   config->salt, config->salt_length, 
                   config->kdf_iterations, key, key_size);
    if (rc != 0) {
        XBDEV_ERRLOG("密钥派生失败: %d\n", rc);
        free_crypto_ctx(lvol_name);
        pthread_mutex_unlock(&g_crypto_mutex);
        return rc;
    }
    
    // 初始化加密上下文
    rc = init_crypto_ctx(crypto_ctx, config->algorithm, key, key_size, config->sector_size);
    if (rc != 0) {
        XBDEV_ERRLOG("初始化加密上下文失败: %d\n", rc);
        free_crypto_ctx(lvol_name);
        pthread_mutex_unlock(&g_crypto_mutex);
        return rc;
    }
    
    XBDEV_NOTICELOG("已为卷启用加密: %s (算法: %d)\n", lvol_name, config->algorithm);
    
    pthread_mutex_unlock(&g_crypto_mutex);
    
    // 清除敏感数据
    memset(key, 0, sizeof(key));
    
    return 0;
}

/**
 * 禁用卷加密
 */
int xbdev_encryption_disable(const char *lvol_name, const char *passphrase) {
    // 此函数会验证密码并移除加密
    // 实际上这需要重新写入整个卷的数据（先解密再写回）
    // 此处只是简单地移除加密上下文
    
    if (!lvol_name || !passphrase) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_crypto_mutex);
    
    // 检查卷是否已加密
    xbdev_crypto_ctx_t *crypto_ctx = find_crypto_ctx(lvol_name);
    if (!crypto_ctx) {
        XBDEV_ERRLOG("卷未加密: %s\n", lvol_name);
        pthread_mutex_unlock(&g_crypto_mutex);
        return -EINVAL;
    }
    
    // TODO: 验证密码
    
    // 释放加密上下文
    free_crypto_ctx(lvol_name);
    
    XBDEV_NOTICELOG("已禁用卷加密: %s\n", lvol_name);
    
    pthread_mutex_unlock(&g_crypto_mutex);
    
    return 0;
}

/**
 * 更改卷加密密钥
 */
int xbdev_encryption_change_key(const char *lvol_name, const char *old_passphrase, const char *new_passphrase) {
    // 实现密钥更改逻辑
    // 此处暂未实现
    XBDEV_WARNLOG("卷加密密钥更改功能暂未实现\n");
    return -ENOTSUP;
}

/**
 * 获取卷加密状态
 */
int xbdev_encryption_get_status(const char *lvol_name, bool *is_encrypted, xbdev_encryption_algorithm_t *algorithm) {
    if (!lvol_name || !is_encrypted) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_crypto_mutex);
    
    // 查找加密上下文
    xbdev_crypto_ctx_t *crypto_ctx = find_crypto_ctx(lvol_name);
    if (!crypto_ctx || !crypto_ctx->enabled) {
        *is_encrypted = false;
        if (algorithm) {
            *algorithm = XBDEV_ENCRYPTION_NONE;
        }
    } else {
        *is_encrypted = true;
        if (algorithm) {
            *algorithm = crypto_ctx->algorithm;
        }
    }
    
    pthread_mutex_unlock(&g_crypto_mutex);
    
    return 0;
}

/**
 * 配置管理服务器访问控制
 */
int xbdev_mgmt_set_access_control(const xbdev_access_control_t *config) {
    if (!config) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_access_control_mutex);
    
    // 复制访问控制配置
    memcpy(&g_access_control, config, sizeof(xbdev_access_control_t));
    
    // 处理特定的访问控制模式
    switch (config->mode) {
        case XBDEV_ACCESS_MODE_OPEN:
            // 开放模式，不做额外处理
            break;
            
        case XBDEV_ACCESS_MODE_BASIC:
            // 基本认证，确保凭据已设置
            if (strlen(config->credentials) == 0) {
                XBDEV_WARNLOG("启用基本认证，但未提供凭据\n");
            }
            break;
            
        case XBDEV_ACCESS_MODE_TOKEN:
            // 令牌认证，清理现有令牌
            pthread_mutex_lock(&g_token_mutex);
            memset(g_tokens, 0, sizeof(g_tokens));
            pthread_mutex_unlock(&g_token_mutex);
            break;
            
        case XBDEV_ACCESS_MODE_TLS_CERT:
            // TLS证书认证
            XBDEV_WARNLOG("TLS证书认证模式暂未完全实现\n");
            break;
            
        default:
            XBDEV_ERRLOG("未知的访问控制模式: %d\n", config->mode);
            pthread_mutex_unlock(&g_access_control_mutex);
            return -EINVAL;
    }
    
    XBDEV_NOTICELOG("已更新访问控制配置，模式: %d\n", config->mode);
    
    pthread_mutex_unlock(&g_access_control_mutex);
    
    return 0;
}

/**
 * 验证管理访问凭据
 */
int xbdev_mgmt_authenticate(const char *username, const char *password) {
    int rc = -EACCES;  // 默认拒绝访问
    
    if (!username || !password) {
        return -EINVAL;
    }
    
    pthread_mutex_lock(&g_access_control_mutex);
    
    // 根据访问模式进行认证
    switch (g_access_control.mode) {
        case XBDEV_ACCESS_MODE_OPEN:
            // 开放模式，始终允许访问
            rc = 0;
            break;
            
        case XBDEV_ACCESS_MODE_BASIC:
            // 基本认证，格式为"username:password"
            {
                char *credentials_copy = strdup(g_access_control.credentials);
                if (!credentials_copy) {
                    rc = -ENOMEM;
                    break;
                }
                
                char *stored_username = credentials_copy;
                char *stored_password = strchr(credentials_copy, ':');
                
                if (stored_password) {
                    *stored_password = '\0';
                    stored_password++;
                    
                    if (strcmp(username, stored_username) == 0 &&
                        strcmp(password, stored_password) == 0) {
                        rc = 0;  // 认证成功
                    }
                }
                
                free(credentials_copy);
            }
            break;
            
        case XBDEV_ACCESS_MODE_TOKEN:
            // 令牌认证，检查令牌表
            pthread_mutex_lock(&g_token_mutex);
            for (int i = 0; i < XBDEV_MAX_TOKENS; i++) {
                if (g_tokens[i].in_use && 
                    strcmp(g_tokens[i].username, username) == 0 &&
                    strcmp(g_tokens[i].token, password) == 0 &&
                    g_tokens[i].expiry > time(NULL)) {
                    rc = 0;  // 令牌有效
                    break;
                }
            }
            pthread_mutex_unlock(&g_token_mutex);
            break;
            
        case XBDEV_ACCESS_MODE_TLS_CERT:
            // TLS证书认证
            XBDEV_WARNLOG("TLS证书认证未实现\n");
            rc = -ENOTSUP;
            break;
            
        default:
            XBDEV_ERRLOG("未知的访问控制模式: %d\n", g_access_control.mode);
            rc = -EINVAL;
    }
    
    pthread_mutex_unlock(&g_access_control_mutex);
    
    if (rc == 0) {
        XBDEV_NOTICELOG("用户认证成功: %s\n", username);
    } else {
        XBDEV_WARNLOG("用户认证失败: %s (错误码: %d)\n", username, rc);
    }
    
    return rc;
}

/**
 * 生成随机令牌
 */
static void generate_random_token(char *token_buf, size_t buf_size) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (buf_size < 32) return;
    
    // 生成随机字节
    unsigned char random_data[32];
    RAND_bytes(random_data, sizeof(random_data));
    
    // 转换为可打印字符
    for (int i = 0; i < 32 && i < buf_size - 1; i++) {
        token_buf[i] = charset[random_data[i] % (sizeof(charset) - 1)];
    }
    token_buf[buf_size - 1] = '\0';
}

/**
 * 生成新访问令牌
 */
int xbdev_mgmt_generate_token(const char *username, const char *password, 
                             char *token_buf, size_t buf_size, uint32_t expiry_seconds) {
    if (!username || !password || !token_buf || buf_size < 32) {
        return -EINVAL;
    }
    
    // 先验证用户名密码
    int rc = xbdev_mgmt_authenticate(username, password);
    if (rc != 0) {
        XBDEV_ERRLOG("令牌生成失败：认证失败 (%d)\n", rc);
        return rc;
    }
    
    pthread_mutex_lock(&g_token_mutex);
    
    // 查找现有令牌或空槽位
    int existing_idx = -1;
    int free_idx = -1;
    
    for (int i = 0; i < XBDEV_MAX_TOKENS; i++) {
        if (g_tokens[i].in_use && strcmp(g_tokens[i].username, username) == 0) {
            existing_idx = i;
            break;
        } else if (!g_tokens[i].in_use && free_idx == -1) {
            free_idx = i;
        }
    }
    
    int idx = (existing_idx >= 0) ? existing_idx : free_idx;
    if (idx == -1) {
        // 令牌表已满
        pthread_mutex_unlock(&g_token_mutex);
        XBDEV_ERRLOG("令牌表已满，无法生成新令牌\n");
        return -ENOSPC;
    }
    
    // 生成随机令牌
    char token[256];
    generate_random_token(token, sizeof(token));
    
    // 保存令牌
    g_tokens[idx].in_use = true;
    strncpy(g_tokens[idx].username, username, sizeof(g_tokens[idx].username) - 1);
    strncpy(g_tokens[idx].token, token, sizeof(g_tokens[idx].token) - 1);
    g_tokens[idx].expiry = time(NULL) + expiry_seconds;
    
    // 返回令牌给调用者
    strncpy(token_buf, token, buf_size - 1);
    token_buf[buf_size - 1] = '\0';
    
    pthread_mutex_unlock(&g_token_mutex);
    
    XBDEV_NOTICELOG("已为用户生成新令牌: %s (有效期: %d秒)\n", 
                  username, expiry_seconds);
    
    return 0;
}

/**
 * 加密IO请求
 *
 * @param bdev_io SPDK BDEV IO请求
 * @param encrypt true表示加密，false表示解密
 * @return 成功返回0，失败返回错误码
 */
int xbdev_encrypt_io(struct spdk_bdev_io *bdev_io, bool encrypt) {
    // TODO: 实现IO数据的加密和解密
    // 流程：
    // 1. 获取IO请求的卷名
    // 2. 查找对应的加密上下文
    // 3. 根据IO类型和数据执行加密或解密操作
    
    // 此处暂未实现
    return -ENOTSUP;
}

/**
 * 导出安全配置到JSON
 */
int xbdev_security_export_config(struct json_object *config_obj) {
    // 导出加密卷配置
    struct json_object *encrypted_vols = json_object_new_array();
    
    pthread_mutex_lock(&g_crypto_mutex);
    
    for (int i = 0; i < XBDEV_MAX_ENCRYPTED_VOLS; i++) {
        if (g_encrypted_vols[i].in_use) {
            struct json_object *vol_obj = json_object_new_object();
            
            json_object_object_add(vol_obj, "name", 
                                 json_object_new_string(g_encrypted_vols[i].lvol_name));
            json_object_object_add(vol_obj, "algorithm", 
                                 json_object_new_int(g_encrypted_vols[i].crypto_ctx.algorithm));
            json_object_object_add(vol_obj, "sector_size", 
                                 json_object_new_int(g_encrypted_vols[i].crypto_ctx.sector_size));
            
            // 注意：出于安全考虑，不导出密钥和IV
            
            json_object_array_add(encrypted_vols, vol_obj);
        }
    }
    
    pthread_mutex_unlock(&g_crypto_mutex);
    
    // 导出访问控制配置
    struct json_object *access_control = json_object_new_object();
    
    pthread_mutex_lock(&g_access_control_mutex);
    
    json_object_object_add(access_control, "mode", 
                         json_object_new_int(g_access_control.mode));
    
    // 出于安全考虑，不导出凭据
    json_object_object_add(access_control, "allowed_ips", 
                         json_object_new_string(g_access_control.allowed_ips));
    json_object_object_add(access_control, "allowed_users", 
                         json_object_new_string(g_access_control.allowed_users));
    json_object_object_add(access_control, "audit_logging", 
                         json_object_new_boolean(g_access_control.audit_logging));
    json_object_object_add(access_control, "audit_log_path", 
                         json_object_new_string(g_access_control.audit_log_path));
    
    pthread_mutex_unlock(&g_access_control_mutex);
    
    // 将配置添加到主配置对象
    json_object_object_add(config_obj, "encrypted_volumes", encrypted_vols);
    json_object_object_add(config_obj, "access_control", access_control);
    
    return 0;
}

/**
 * 从JSON导入安全配置
 */
int xbdev_security_import_config(struct json_object *config_obj) {
    struct json_object *encrypted_vols = NULL;
    struct json_object *access_control = NULL;
    int rc = 0;
    
    // 获取加密卷配置
    if (json_object_object_get_ex(config_obj, "encrypted_volumes", &encrypted_vols)) {
        int num_vols = json_object_array_length(encrypted_vols);
        
        // 处理每个加密卷
        for (int i = 0; i < num_vols; i++) {
            struct json_object *vol_obj = json_object_array_get_idx(encrypted_vols, i);
            struct json_object *name_obj = NULL;
            struct json_object *algorithm_obj = NULL;
            struct json_object *sector_size_obj = NULL;
            
            // 获取卷信息
            if (json_object_object_get_ex(vol_obj, "name", &name_obj) &&
                json_object_object_get_ex(vol_obj, "algorithm", &algorithm_obj) &&
                json_object_object_get_ex(vol_obj, "sector_size", &sector_size_obj)) {
                
                const char *name = json_object_get_string(name_obj);
                int algorithm = json_object_get_int(algorithm_obj);
                int sector_size = json_object_get_int(sector_size_obj);
                
                // 注意：实际配置需要密码才能完成
                XBDEV_NOTICELOG("导入的卷加密配置需要密码才能激活: %s\n", name);
            }
        }
    }
    
    // 获取访问控制配置
    if (json_object_object_get_ex(config_obj, "access_control", &access_control)) {
        struct json_object *mode_obj = NULL;
        struct json_object *allowed_ips_obj = NULL;
        struct json_object *allowed_users_obj = NULL;
        struct json_object *audit_logging_obj = NULL;
        struct json_object *audit_log_path_obj = NULL;
        
        // 创建临时访问控制配置
        xbdev_access_control_t temp_config;
        memset(&temp_config, 0, sizeof(temp_config));
        
        // 设置默认模式为开放
        temp_config.mode = XBDEV_ACCESS_MODE_OPEN;
        
        // 获取配置字段
        if (json_object_object_get_ex(access_control, "mode", &mode_obj)) {
            temp_config.mode = json_object_get_int(mode_obj);
        }
        
        if (json_object_object_get_ex(access_control, "allowed_ips", &allowed_ips_obj)) {
            strncpy(temp_config.allowed_ips, 
                    json_object_get_string(allowed_ips_obj), 
                    sizeof(temp_config.allowed_ips) - 1);
        }
        
        if (json_object_object_get_ex(access_control, "allowed_users", &allowed_users_obj)) {
            strncpy(temp_config.allowed_users, 
                    json_object_get_string(allowed_users_obj), 
                    sizeof(temp_config.allowed_users) - 1);
        }
        
        if (json_object_object_get_ex(access_control, "audit_logging", &audit_logging_obj)) {
            temp_config.audit_logging = json_object_get_boolean(audit_logging_obj);
        }
        
        if (json_object_object_get_ex(access_control, "audit_log_path", &audit_log_path_obj)) {
            strncpy(temp_config.audit_log_path, 
                    json_object_get_string(audit_log_path_obj), 
                    sizeof(temp_config.audit_log_path) - 1);
        }
        
        // 应用访问控制配置（注意：需要单独设置凭据）
        rc = xbdev_mgmt_set_access_control(&temp_config);
        if (rc != 0) {
            XBDEV_ERRLOG("应用访问控制配置失败: %d\n", rc);
            return rc;
        }
    }
    
    return rc;
}

/**
 * 审计记录功能
 *
 * @param username 用户名
 * @param action 执行的操作
 * @param target 操作目标
 * @param result 操作结果(0=成功, 非0=失败)
 */
void xbdev_security_audit_log(const char *username, const char *action, 
                            const char *target, int result) {
    // 检查是否启用审计日志
    if (!g_access_control.audit_logging) {
        return;
    }
    
    // 创建时间戳
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // 打开审计日志文件
    FILE *log_file = NULL;
    if (g_access_control.audit_log_path[0] != '\0') {
        log_file = fopen(g_access_control.audit_log_path, "a");
    }
    
    if (!log_file) {
        XBDEV_WARNLOG("无法打开审计日志文件: %s\n", 
                    g_access_control.audit_log_path);
        return;
    }
    
    // 写入审计日志
    fprintf(log_file, "[%s] 用户: %s, 操作: %s, 目标: %s, 结果: %s\n",
            timestamp,
            username ? username : "unknown",
            action ? action : "unknown",
            target ? target : "unknown",
            result == 0 ? "成功" : "失败");
    
    // 关闭日志文件
    fclose(log_file);
}

/**
 * 检查IP地址是否在允许列表中
 *
 * @param ip_addr 待检查的IP地址
 * @return true=允许访问, false=禁止访问
 */
bool xbdev_security_check_ip(const char *ip_addr) {
    if (!ip_addr) {
        return false;
    }
    
    // 如果没有设置IP限制，允许所有访问
    if (g_access_control.allowed_ips[0] == '\0') {
        return true;
    }
    
    // 复制允许的IP地址列表进行解析
    char *allowed_ips = strdup(g_access_control.allowed_ips);
    if (!allowed_ips) {
        return false;
    }
    
    // 使用逗号分割IP地址列表
    bool allowed = false;
    char *token = strtok(allowed_ips, ",");
    
    while (token) {
        // 去除前后空格
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';
        
        // 检查是否匹配
        if (strcmp(token, ip_addr) == 0) {
            allowed = true;
            break;
        }
        
        // 下一个IP
        token = strtok(NULL, ",");
    }
    
    free(allowed_ips);
    return allowed;
}

/**
 * 检查用户是否在允许列表中
 *
 * @param username 待检查的用户名
 * @return true=允许访问, false=禁止访问
 */
bool xbdev_security_check_user(const char *username) {
    if (!username) {
        return false;
    }
    
    // 如果没有设置用户限制，允许所有访问
    if (g_access_control.allowed_users[0] == '\0') {
        return true;
    }
    
    // 复制允许的用户列表进行解析
    char *allowed_users = strdup(g_access_control.allowed_users);
    if (!allowed_users) {
        return false;
    }
    
    // 使用逗号分割用户列表
    bool allowed = false;
    char *token = strtok(allowed_users, ",");
    
    while (token) {
        // 去除前后空格
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') *end-- = '\0';
        
        // 检查是否匹配
        if (strcmp(token, username) == 0) {
            allowed = true;
            break;
        }
        
        // 下一个用户
        token = strtok(NULL, ",");
    }
    
    free(allowed_users);
    return allowed;
}

/**
 * 清理过期令牌
 */
void xbdev_security_cleanup_tokens(void) {
    time_t now = time(NULL);
    int cleaned = 0;
    
    pthread_mutex_lock(&g_token_mutex);
    
    for (int i = 0; i < XBDEV_MAX_TOKENS; i++) {
        if (g_tokens[i].in_use && g_tokens[i].expiry < now) {
            // 清除过期令牌
            memset(&g_tokens[i], 0, sizeof(g_tokens[i]));
            cleaned++;
        }
    }
    
    pthread_mutex_unlock(&g_token_mutex);
    
    if (cleaned > 0) {
        XBDEV_NOTICELOG("已清理%d个过期令牌\n", cleaned);
    }
}