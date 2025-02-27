/**
 * @file xbdev_security.h
 * @brief 存储安全功能的接口定义
 * 
 * 该文件定义了libxbdev的安全相关接口，包括加密、访问控制和认证功能。
 */

#ifndef XBDEV_SECURITY_H
#define XBDEV_SECURITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 加密算法类型
 */
typedef enum {
    XBDEV_ENCRYPTION_NONE = 0,
    XBDEV_ENCRYPTION_AES_XTS_128,
    XBDEV_ENCRYPTION_AES_XTS_256,
    XBDEV_ENCRYPTION_AES_CBC_256,
    XBDEV_ENCRYPTION_SM4,         // 国密SM4算法
    XBDEV_ENCRYPTION_CUSTOM       // 自定义加密算法
} xbdev_encryption_algorithm_t;

/**
 * 密钥派生方法
 */
typedef enum {
    XBDEV_KEY_DERIVE_NONE = 0,    // 直接使用原始密钥
    XBDEV_KEY_DERIVE_PBKDF2,      // PBKDF2派生
    XBDEV_KEY_DERIVE_SCRYPT,      // scrypt派生
    XBDEV_KEY_DERIVE_ARGON2ID     // Argon2id派生
} xbdev_key_derive_method_t;

/**
 * 加密配置结构体
 */
typedef struct {
    xbdev_encryption_algorithm_t algorithm;  // 加密算法
    xbdev_key_derive_method_t key_method;    // 密钥派生方法
    uint32_t sector_size;                    // 加密扇区大小
    uint32_t kdf_iterations;                 // 密钥派生迭代次数
    char passphrase[256];                    // 口令或密钥
    uint8_t salt[64];                        // 密钥派生用盐
    uint32_t salt_length;                    // 盐长度
    bool discard_encryption;                 // 是否加密UNMAP/TRIM命令的范围
} xbdev_encryption_config_t;

/**
 * 访问控制模式
 */
typedef enum {
    XBDEV_ACCESS_MODE_OPEN = 0,   // 开放访问
    XBDEV_ACCESS_MODE_BASIC,      // 基本访问控制（用户名/密码）
    XBDEV_ACCESS_MODE_TOKEN,      // 基于令牌的访问控制
    XBDEV_ACCESS_MODE_TLS_CERT    // 基于TLS证书的访问控制
} xbdev_access_mode_t;

/**
 * 访问控制配置
 */
typedef struct {
    xbdev_access_mode_t mode;     // 访问控制模式
    char credentials[512];        // 凭据信息（根据模式不同内容不同）
    char allowed_ips[1024];       // 允许访问的IP地址列表，逗号分隔
    char allowed_users[1024];     // 允许访问的用户列表，逗号分隔
    bool audit_logging;           // 是否启用审计日志
    char audit_log_path[256];     // 审计日志路径
} xbdev_access_control_t;

/**
 * 为卷启用加密
 * 
 * @param lvol_name 逻辑卷名称
 * @param config 加密配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_encryption_enable(const char *lvol_name, const xbdev_encryption_config_t *config);

/**
 * 禁用卷加密
 * 
 * @param lvol_name 逻辑卷名称
 * @param passphrase 当前密码（用于验证）
 * @return 成功返回0，失败返回错误码
 */
int xbdev_encryption_disable(const char *lvol_name, const char *passphrase);

/**
 * 更改卷加密密钥
 * 
 * @param lvol_name 逻辑卷名称
 * @param old_passphrase 旧密码
 * @param new_passphrase 新密码
 * @return 成功返回0，失败返回错误码
 */
int xbdev_encryption_change_key(const char *lvol_name, const char *old_passphrase, const char *new_passphrase);

/**
 * 获取卷加密状态
 * 
 * @param lvol_name 逻辑卷名称
 * @param is_encrypted 输出参数，是否已加密
 * @param algorithm 输出参数，使用的加密算法
 * @return 成功返回0，失败返回错误码
 */
int xbdev_encryption_get_status(const char *lvol_name, bool *is_encrypted, xbdev_encryption_algorithm_t *algorithm);

/**
 * 配置管理服务器访问控制
 * 
 * @param config 访问控制配置
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_set_access_control(const xbdev_access_control_t *config);

/**
 * 验证管理访问凭据
 * 
 * @param username 用户名
 * @param password 密码或令牌
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_authenticate(const char *username, const char *password);

/**
 * 生成新访问令牌
 * 
 * @param username 用户名
 * @param password 密码
 * @param token_buf 输出令牌的缓冲区
 * @param buf_size 缓冲区大小
 * @param expiry_seconds 令牌有效期（秒）
 * @return 成功返回0，失败返回错误码
 */
int xbdev_mgmt_generate_token(const char *username, const char *password, 
                             char *token_buf, size_t buf_size, uint32_t expiry_seconds);

#ifdef __cplusplus
}
#endif

#endif // XBDEV_SECURITY_H
