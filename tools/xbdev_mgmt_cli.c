/**
 * @file xbdev_mgmt_cli.c
 * @brief 命令行管理工具
 *
 * 这是一个简单的命令行工具，用于演示如何使用libxbdev的管理API。
 */

#include "xbdev.h"
#include "xbdev_mgmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <signal.h>

#define MAX_CMD_LENGTH 4096
#define MAX_RESPONSE_LENGTH 65536

// 命令行选项
static struct option long_options[] = {
    {"server", required_argument, 0, 's'},
    {"port", required_argument, 0, 'p'},
    {"command", required_argument, 0, 'c'},
    {"interactive", no_argument, 0, 'i'},
    {"output", required_argument, 0, 'o'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

// 命令行参数
static struct {
    char server[256];
    int port;
    char command[MAX_CMD_LENGTH];
    bool interactive;
    char output_file[256];
} g_options = {
    .server = "127.0.0.1",
    .port = 8181,
    .command = "",
    .interactive = false,
    .output_file = ""
};

// 全局配置
static bool g_running = true;

/**
 * 打印使用帮助
 */
static void print_usage(const char *prog_name) {
    printf("使用方法: %s [选项]\n", prog_name);
    printf("选项:\n");
    printf("  -s, --server=HOST       指定服务器地址 (默认: 127.0.0.1)\n");
    printf("  -p, --port=PORT         指定服务器端口 (默认: 8181)\n");
    printf("  -c, --command=CMD       执行单个命令\n");
    printf("  -i, --interactive       进入交互式模式\n");
    printf("  -o, --output=FILE       输出结果到文件\n");
    printf("  -h, --help              显示此帮助信息\n");
    printf("\n");
    printf("示例:\n");
    printf("  %s -c '{\"method\":\"device_list\"}'\n", prog_name);
    printf("  %s -i\n", prog_name);
    printf("\n");
}

/**
 * 执行命令并输出结果
 */
static int execute_command(const char *command) {
    char response[MAX_RESPONSE_LENGTH];
    int rc;
    
    // 构建JSON RPC请求
    char json_cmd[MAX_CMD_LENGTH];
    
    // 如果命令没有包含method字段，则假定是原始命令
    if (strstr(command, "\"method\"") == NULL) {
        snprintf(json_cmd, sizeof(json_cmd), "{\"method\":\"xbdev_execute_cmd\", \"params\":{\"cmd\":\"%s\"}}", command);
    } else {
        // 否则假定是完整的JSON RPC请求
        strncpy(json_cmd, command, sizeof(json_cmd) - 1);
        json_cmd[sizeof(json_cmd) - 1] = '\0';
    }
    
    // 向服务器发送请求（此处使用的是本地执行模式，真实情况应通过网络发送）
    rc = xbdev_mgmt_execute_cmd(json_cmd, response, sizeof(response));
    if (rc != 0) {
        fprintf(stderr, "执行命令失败: %d\n", rc);
        return rc;
    }
    
    // 输出响应
    printf("%s\n", response);
    
    // 如果指定了输出文件，将结果同时写入文件
    if (g_options.output_file[0] != '\0') {
        FILE *f = fopen(g_options.output_file, "w");
        if (f) {
            fprintf(f, "%s\n", response);
            fclose(f);
        } else {
            fprintf(stderr, "无法写入输出文件: %s\n", g_options.output_file);
        }
    }
    
    return 0;
}

/**
 * 信号处理函数
 */
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n接收到中断信号，正在退出...\n");
        g_running = false;
    }
}

/**
 * 交互式模式的命令提示
 */
static void run_interactive_mode(void) {
    char cmd_buffer[MAX_CMD_LENGTH];
    
    printf("XBDEV管理终端 (输入 'exit' 退出)\n");
    
    while (g_running) {
        printf("xbdev> ");
        fflush(stdout);
        
        if (fgets(cmd_buffer, sizeof(cmd_buffer), stdin) == NULL) {
            break;
        }
        
        // 去除末尾换行符
        size_t len = strlen(cmd_buffer);
        if (len > 0 && cmd_buffer[len - 1] == '\n') {
            cmd_buffer[len - 1] = '\0';
        }
        
        // 检查退出命令
        if (strcmp(cmd_buffer, "exit") == 0 || strcmp(cmd_buffer, "quit") == 0) {
            break;
        }
        
        // 检查帮助命令
        if (strcmp(cmd_buffer, "help") == 0) {
            printf("可用命令:\n");
            printf("  device_list                - 列出所有设备\n");
            printf("  device_info <device>       - 显示设备详情\n");
            printf("  create_raid <参数>         - 创建RAID设备\n");
            printf("  create_lvol <参数>         - 创建逻辑卷\n");
            printf("  create_snapshot <参数>     - 创建快照\n");
            printf("  resize_lvol <参数>         - 调整逻辑卷大小\n");
            printf("  delete_device <参数>       - 删除设备\n");
            printf("  exit                       - 退出程序\n");
            printf("  help                       - 显示此帮助信息\n");
            printf("\n使用JSON格式的参数，例如:\n");
            printf("  device_info {\"device\":\"nvme0\"}\n");
            continue;
        }
        
        // 执行命令
        execute_command(cmd_buffer);
    }
}

/**
 * 主函数
 */
int main(int argc, char **argv) {
    int opt;
    int option_index = 0;
    
    // 解析命令行参数
    while ((opt = getopt_long(argc, argv, "s:p:c:io:h", long_options, &option_index)) != -1) {
        switch (opt) {
        case 's':
            strncpy(g_options.server, optarg, sizeof(g_options.server) - 1);
            g_options.server[sizeof(g_options.server) - 1] = '\0';
            break;
        case 'p':
            g_options.port = atoi(optarg);
            break;
        case 'c':
            strncpy(g_options.command, optarg, sizeof(g_options.command) - 1);
            g_options.command[sizeof(g_options.command) - 1] = '\0';
            break;
        case 'i':
            g_options.interactive = true;
            break;
        case 'o':
            strncpy(g_options.output_file, optarg, sizeof(g_options.output_file) - 1);
            g_options.output_file[sizeof(g_options.output_file) - 1] = '\0';
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            fprintf(stderr, "未知选项: %c\n", opt);
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // 检查参数有效性
    if (g_options.port <= 0 || g_options.port > 65535) {
        fprintf(stderr, "无效的端口号: %d\n", g_options.port);
        return 1;
    }
    
    // 注册信号处理函数
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 初始化libxbdev
    int rc = xbdev_init();
    if (rc != 0) {
        fprintf(stderr, "初始化libxbdev失败: %d\n", rc);
        return 1;
    }
    
    // 初始化管理服务器（可选，仅本地测试时使用）
    // 正常情况下，CLI工具应连接到已经运行的服务器
    if (strcmp(g_options.server, "127.0.0.1") == 0 || strcmp(g_options.server, "localhost") == 0) {
        rc = xbdev_mgmt_server_init(g_options.server, g_options.port);
        if (rc != 0) {
            fprintf(stderr, "初始化管理服务器失败: %d (如果服务器已在运行，忽略此错误)\n", rc);
            // 继续执行，因为服务器可能已经启动
        }
    }
    
    // 根据模式运行
    if (g_options.interactive) {
        // 交互模式
        run_interactive_mode();
    } else if (g_options.command[0]) {
        // 执行单个命令
        rc = execute_command(g_options.command);
        if (rc != 0) {
            fprintf(stderr, "命令执行失败: %d\n", rc);
            xbdev_fini();
            return 1;
        }
    } else {
        // 没有指定命令或交互模式
        fprintf(stderr, "请指定命令(-c)或使用交互模式(-i)\n");
        print_usage(argv[0]);
        xbdev_fini();
        return 1;
    }
    
    // 清理资源
    if (strcmp(g_options.server, "127.0.0.1") == 0 || strcmp(g_options.server, "localhost") == 0) {
        // 仅在本地测试时清理服务器
        xbdev_mgmt_server_fini();
    }
    
    xbdev_fini();
    
    return 0;
}