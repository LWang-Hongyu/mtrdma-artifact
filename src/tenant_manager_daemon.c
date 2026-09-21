/*
 * tenant_manager_daemon.c - RDMA Tenant Manager Daemon
 * 
 * 方案A实现：共享内存热更新（Shared Memory Hot-Update）
 * 
 * 功能：
 * - 轻量级守护进程，监听Unix Socket
 * - 支持JSON协议命令
 * - 实时更新租户配额（无需重启应用）
 * - 命令：CREATE, UPDATE_QUOTA, DELETE, STATUS, LIST
 * 
 * 用法：
 *   tenant_manager_daemon --daemon --foreground    # 前台调试模式
 *   tenant_manager_daemon --daemon                 # 后台守护模式
 * 
 * 协议（JSON over Unix Socket）：
 *   {"cmd":"UPDATE_QUOTA","tenant":20,"qp":50,"mr":100,"memory":1073741824}
 *   {"cmd":"CREATE","tenant":20,"name":"Test","qp":50,"mr":100,"memory":1073741824}
 *   {"cmd":"DELETE","tenant":20}
 *   {"cmd":"STATUS","tenant":20}
 *   {"cmd":"LIST_TENANTS"}
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <json-c/json.h>

#include "shm/shared_memory_tenant.h"

#define SOCKET_PATH "/tmp/rdma_tenant_manager.sock"
#define PID_FILE "/tmp/rdma_tenant_manager.pid"
#define BUFFER_SIZE 4096

static volatile int running = 1;
static int server_fd = -1;

/* 信号处理 */
void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        fprintf(stderr, "[MANAGER] Received signal %d, shutting down...\n", sig);
        running = 0;
        if (server_fd >= 0) {
            close(server_fd);
            server_fd = -1;
        }
    }
}

/* 清理资源 */
void cleanup(void) {
    unlink(SOCKET_PATH);
    unlink(PID_FILE);
}

/* 写入PID文件 */
int write_pid_file(void) {
    FILE* fp = fopen(PID_FILE, "w");
    if (!fp) {
        perror("[MANAGER] Failed to write PID file");
        return -1;
    }
    fprintf(fp, "%d\n", getpid());
    fclose(fp);
    return 0;
}

/* 创建Unix Socket服务器 */
int create_server_socket(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[MANAGER] socket failed");
        return -1;
    }

    // 设置非阻塞模式（用于信号处理）
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // 删除已存在的socket文件
    unlink(SOCKET_PATH);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[MANAGER] bind failed");
        close(fd);
        return -1;
    }

    // 设置权限（允许所有用户访问，实验环境）
    chmod(SOCKET_PATH, 0777);

    if (listen(fd, 10) < 0) {
        perror("[MANAGER] listen failed");
        close(fd);
        return -1;
    }

    fprintf(stderr, "[MANAGER] Listening on %s\n", SOCKET_PATH);
    return fd;
}

/* 构建JSON响应 */
char* build_response(int success, const char* message, json_object* data) {
    json_object* resp = json_object_new_object();
    json_object_object_add(resp, "success", json_object_new_boolean(success));
    json_object_object_add(resp, "message", json_object_new_string(message ? message : ""));
    if (data) {
        json_object_object_add(resp, "data", data);
    }
    
    const char* str = json_object_to_json_string(resp);
    char* result = strdup(str);
    json_object_put(resp);
    return result;
}

/* 处理 UPDATE_QUOTA 命令 */
char* handle_update_quota(json_object* cmd_obj) {
    json_object* tenant_obj, *qp_obj, *mr_obj, *mem_obj;
    
    if (!json_object_object_get_ex(cmd_obj, "tenant", &tenant_obj) ||
        !json_object_object_get_ex(cmd_obj, "qp", &qp_obj)) {
        return build_response(0, "Missing required fields: tenant, qp", NULL);
    }
    
    uint32_t tenant_id = json_object_get_int(tenant_obj);
    int qp = json_object_get_int(qp_obj);
    int mr = json_object_object_get_ex(cmd_obj, "mr", &mr_obj) ? 
             json_object_get_int(mr_obj) : qp;
    uint64_t mem = json_object_object_get_ex(cmd_obj, "memory", &mem_obj) ? 
                   (uint64_t)json_object_get_int64(mem_obj) : 1073741824ULL;
    
    tenant_quota_t quota = {
        .max_qp_per_tenant = qp,
        .max_mr_per_tenant = mr,
        .max_memory_per_tenant = mem,
        .max_cq_per_tenant = qp,
        .max_pd_per_tenant = 10
    };
    
    fprintf(stderr, "[MANAGER] UPDATE_QUOTA: tenant=%u, QP=%d, MR=%d, Mem=%llu\n",
            tenant_id, qp, mr, (unsigned long long)mem);
    
    if (tenant_update_quota(tenant_id, &quota) != 0) {
        return build_response(0, "Failed to update quota", NULL);
    }
    
    // 更新时间戳
    tenant_shared_memory_t* shm = tenant_shm_get_ptr();
    if (shm) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        shm->last_update_time = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        shm->version++;
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Quota updated for tenant %u", tenant_id);
    return build_response(1, msg, NULL);
}

/* 处理 CREATE 命令 */
char* handle_create(json_object* cmd_obj) {
    json_object* tenant_obj, *name_obj, *qp_obj, *mr_obj, *mem_obj;
    
    if (!json_object_object_get_ex(cmd_obj, "tenant", &tenant_obj)) {
        return build_response(0, "Missing required field: tenant", NULL);
    }
    
    uint32_t tenant_id = json_object_get_int(tenant_obj);
    const char* name = json_object_object_get_ex(cmd_obj, "name", &name_obj) ?
                       json_object_get_string(name_obj) : "unnamed";
    int qp = json_object_object_get_ex(cmd_obj, "qp", &qp_obj) ?
             json_object_get_int(qp_obj) : 100;
    int mr = json_object_object_get_ex(cmd_obj, "mr", &mr_obj) ?
             json_object_get_int(mr_obj) : 100;
    uint64_t mem = json_object_object_get_ex(cmd_obj, "memory", &mem_obj) ?
                   (uint64_t)json_object_get_int64(mem_obj) : 1073741824ULL;
    
    tenant_quota_t quota = {
        .max_qp_per_tenant = qp,
        .max_mr_per_tenant = mr,
        .max_memory_per_tenant = mem,
        .max_cq_per_tenant = qp,
        .max_pd_per_tenant = 10
    };
    
    fprintf(stderr, "[MANAGER] CREATE: tenant=%u, name=%s, QP=%d, MR=%d\n",
            tenant_id, name, qp, mr);
    
    if (tenant_create(tenant_id, name, &quota) != 0) {
        return build_response(0, "Failed to create tenant", NULL);
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Tenant %u created", tenant_id);
    return build_response(1, msg, NULL);
}

/* 处理 DELETE 命令 */
char* handle_delete(json_object* cmd_obj) {
    json_object* tenant_obj;
    
    if (!json_object_object_get_ex(cmd_obj, "tenant", &tenant_obj)) {
        return build_response(0, "Missing required field: tenant", NULL);
    }
    
    uint32_t tenant_id = json_object_get_int(tenant_obj);
    
    fprintf(stderr, "[MANAGER] DELETE: tenant=%u\n", tenant_id);
    
    if (tenant_delete(tenant_id) != 0) {
        return build_response(0, "Failed to delete tenant", NULL);
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Tenant %u deleted", tenant_id);
    return build_response(1, msg, NULL);
}

/* 处理 STATUS 命令 */
char* handle_status(json_object* cmd_obj) {
    json_object* tenant_obj;
    
    if (!json_object_object_get_ex(cmd_obj, "tenant", &tenant_obj)) {
        // 返回所有租户状态
        json_object* tenants_array = json_object_new_array();
        tenant_shared_memory_t* shm = tenant_shm_get_ptr();
        
        if (shm) {
            for (int i = 0; i < MAX_TENANTS; i++) {
                if (shm->tenants[i].status != TENANT_STATUS_INACTIVE) {
                    json_object* t = json_object_new_object();
                    json_object_object_add(t, "id", json_object_new_int(shm->tenants[i].tenant_id));
                    json_object_object_add(t, "name", json_object_new_string(shm->tenants[i].tenant_name));
                    json_object_object_add(t, "status", json_object_new_int(shm->tenants[i].status));
                    json_object_object_add(t, "qp_used", json_object_new_int(shm->tenants[i].usage.qp_count));
                    json_object_object_add(t, "qp_limit", json_object_new_int(shm->tenants[i].quota.max_qp_per_tenant));
                    json_object_object_add(t, "mr_used", json_object_new_int(shm->tenants[i].usage.mr_count));
                    json_object_object_add(t, "mr_limit", json_object_new_int(shm->tenants[i].quota.max_mr_per_tenant));
                    json_object_object_add(t, "memory_used", json_object_new_int64(shm->tenants[i].usage.memory_used));
                    json_object_object_add(t, "memory_limit", json_object_new_int64(shm->tenants[i].quota.max_memory_per_tenant));
                    json_object_array_add(tenants_array, t);
                }
            }
        }
        
        return build_response(1, "All tenants status", tenants_array);
    }
    
    uint32_t tenant_id = json_object_get_int(tenant_obj);
    tenant_info_t info;
    
    if (tenant_get_info(tenant_id, &info) != 0) {
        return build_response(0, "Tenant not found", NULL);
    }
    
    json_object* data = json_object_new_object();
    json_object_object_add(data, "id", json_object_new_int(info.tenant_id));
    json_object_object_add(data, "name", json_object_new_string(info.tenant_name));
    json_object_object_add(data, "status", json_object_new_int(info.status));
    json_object_object_add(data, "qp_used", json_object_new_int(info.usage.qp_count));
    json_object_object_add(data, "qp_limit", json_object_new_int(info.quota.max_qp_per_tenant));
    json_object_object_add(data, "mr_used", json_object_new_int(info.usage.mr_count));
    json_object_object_add(data, "mr_limit", json_object_new_int(info.quota.max_mr_per_tenant));
    json_object_object_add(data, "memory_used", json_object_new_int64(info.usage.memory_used));
    json_object_object_add(data, "memory_limit", json_object_new_int64(info.quota.max_memory_per_tenant));
    json_object_object_add(data, "total_qp_creates", json_object_new_int64(info.usage.total_qp_creates));
    json_object_object_add(data, "total_mr_regs", json_object_new_int64(info.usage.total_mr_regs));
    
    return build_response(1, "Tenant status", data);
}

/* 处理 LIST_TENANTS 命令 */
char* handle_list_tenants(void) {
    json_object* tenants_array = json_object_new_array();
    tenant_shared_memory_t* shm = tenant_shm_get_ptr();
    
    if (shm) {
        for (int i = 0; i < MAX_TENANTS; i++) {
            if (shm->tenants[i].status != TENANT_STATUS_INACTIVE) {
                json_object* t = json_object_new_object();
                json_object_object_add(t, "id", json_object_new_int(shm->tenants[i].tenant_id));
                json_object_object_add(t, "name", json_object_new_string(shm->tenants[i].tenant_name));
                json_object_object_add(t, "qp_used", json_object_new_int(shm->tenants[i].usage.qp_count));
                json_object_object_add(t, "qp_limit", json_object_new_int(shm->tenants[i].quota.max_qp_per_tenant));
                json_object_object_add(t, "mr_used", json_object_new_int(shm->tenants[i].usage.mr_count));
                json_object_object_add(t, "mr_limit", json_object_new_int(shm->tenants[i].quota.max_mr_per_tenant));
                json_object_array_add(tenants_array, t);
            }
        }
    }
    
    json_object* data = json_object_new_object();
    json_object_object_add(data, "count", json_object_new_int(json_object_array_length(tenants_array)));
    json_object_object_add(data, "version", json_object_new_int64(shm ? shm->version : 0));
    json_object_object_add(data, "tenants", tenants_array);
    
    return build_response(1, "Tenant list", data);
}

/* 处理客户端命令 */
char* process_command(const char* json_str) {
    json_object* cmd_obj = json_tokener_parse(json_str);
    if (!cmd_obj) {
        return build_response(0, "Invalid JSON", NULL);
    }
    
    json_object* cmd_type_obj;
    if (!json_object_object_get_ex(cmd_obj, "cmd", &cmd_type_obj)) {
        json_object_put(cmd_obj);
        return build_response(0, "Missing 'cmd' field", NULL);
    }
    
    const char* cmd = json_object_get_string(cmd_type_obj);
    char* response = NULL;
    
    if (strcmp(cmd, "UPDATE_QUOTA") == 0) {
        response = handle_update_quota(cmd_obj);
    } else if (strcmp(cmd, "CREATE") == 0) {
        response = handle_create(cmd_obj);
    } else if (strcmp(cmd, "DELETE") == 0) {
        response = handle_delete(cmd_obj);
    } else if (strcmp(cmd, "STATUS") == 0) {
        response = handle_status(cmd_obj);
    } else if (strcmp(cmd, "LIST_TENANTS") == 0) {
        response = handle_list_tenants();
    } else {
        response = build_response(0, "Unknown command", NULL);
    }
    
    json_object_put(cmd_obj);
    return response;
}

/* 处理单个客户端连接 */
void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, sizeof(buffer));
    
    int n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    
    buffer[n] = '\0';
    fprintf(stderr, "[MANAGER] Received: %s\n", buffer);
    
    char* response = process_command(buffer);
    if (response) {
        send(client_fd, response, strlen(response), 0);
        send(client_fd, "\n", 1, 0);  // 添加换行符
        free(response);
    }
    
    close(client_fd);
}

/* 主循环 */
void main_loop(void) {
    fd_set readfds;
    struct timeval tv;
    
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("[MANAGER] select failed");
            break;
        }
        
        if (ret > 0 && FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("[MANAGER] accept failed");
                }
                continue;
            }
            
            handle_client(client_fd);
        }
    }
}

/* 打印用法 */
void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --daemon          Run as daemon\n");
    fprintf(stderr, "  --foreground      Run in foreground (with --daemon)\n");
    fprintf(stderr, "  --help            Show this help\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s --daemon --foreground    # Debug mode\n", prog);
    fprintf(stderr, "  %s --daemon                 # Production mode\n", prog);
}

int main(int argc, char* argv[]) {
    int daemon_mode = 0;
    int foreground = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    // 初始化共享内存
    if (tenant_shm_init() != 0) {
        fprintf(stderr, "[MANAGER] Failed to initialize shared memory\n");
        return 1;
    }
    fprintf(stderr, "[MANAGER] Shared memory initialized\n");
    
    // 如果作为daemon运行
    if (daemon_mode && !foreground) {
        if (daemon(0, 0) != 0) {
            perror("[MANAGER] daemon failed");
            return 1;
        }
        if (write_pid_file() != 0) {
            return 1;
        }
        fprintf(stderr, "[MANAGER] Running as daemon (PID: %d)\n", getpid());
    }
    
    // 注册信号处理
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    atexit(cleanup);
    
    // 创建服务器socket
    server_fd = create_server_socket();
    if (server_fd < 0) {
        return 1;
    }
    
    fprintf(stderr, "[MANAGER] Ready. Waiting for commands...\n");
    
    // 主循环
    main_loop();
    
    fprintf(stderr, "[MANAGER] Shutdown complete\n");
    return 0;
}
