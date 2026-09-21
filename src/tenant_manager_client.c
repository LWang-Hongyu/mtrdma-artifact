/*
 * tenant_manager_client.c - RDMA Tenant Manager Client
 * 
 * 命令行工具，用于与tenant_manager_daemon通信
 * 支持动态配额更新（无需重启应用程序）
 * 
 * 用法：
 *   tenant_manager_client create <tenant_id> <qp> <mr> [memory] [name]
 *   tenant_manager_client delete <tenant_id>
 *   tenant_manager_client update <tenant_id> <qp> <mr> [memory]   <- ★ 热更新
 *   tenant_manager_client status [tenant_id]
 *   tenant_manager_client list
 * 
 * 示例：
 *   # 创建租户
 *   tenant_manager_client create 20 100 100
 * 
 *   # ★ 动态更新配额（应用程序无需重启）
 *   tenant_manager_client update 20 50 50
 * 
 *   # 查看状态
 *   tenant_manager_client status 20
 *   tenant_manager_client list
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <json-c/json.h>

#define SOCKET_PATH "/tmp/rdma_tenant_manager.sock"
#define BUFFER_SIZE 4096

/* 发送命令到daemon并接收响应 */
char* send_command(const char* json_cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return NULL;
    }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect (is daemon running?)");
        close(fd);
        return NULL;
    }
    
    if (send(fd, json_cmd, strlen(json_cmd), 0) < 0) {
        perror("send");
        close(fd);
        return NULL;
    }
    
    char* response = malloc(BUFFER_SIZE);
    if (!response) {
        close(fd);
        return NULL;
    }
    
    int n = recv(fd, response, BUFFER_SIZE - 1, 0);
    close(fd);
    
    if (n <= 0) {
        free(response);
        return NULL;
    }
    
    response[n] = '\0';
    return response;
}

/* 打印JSON响应（格式化） */
void print_response(const char* response) {
    json_object* obj = json_tokener_parse(response);
    if (!obj) {
        printf("Response: %s\n", response);
        return;
    }
    
    json_object* success_obj;
    if (json_object_object_get_ex(obj, "success", &success_obj)) {
        int success = json_object_get_boolean(success_obj);
        
        if (success) {
            printf("✓ Success\n");
        } else {
            printf("✗ Failed\n");
        }
    }
    
    json_object* msg_obj;
    if (json_object_object_get_ex(obj, "message", &msg_obj)) {
        printf("Message: %s\n", json_object_get_string(msg_obj));
    }
    
    json_object* data_obj;
    if (json_object_object_get_ex(obj, "data", &data_obj)) {
        printf("\nData:\n");
        
        // 如果是数组，打印列表
        if (json_object_is_type(data_obj, json_type_array)) {
            int len = json_object_array_length(data_obj);
            for (int i = 0; i < len; i++) {
                json_object* item = json_object_array_get_idx(data_obj, i);
                json_object *id_obj, *name_obj, *qp_obj, *mr_obj;
                
                if (json_object_object_get_ex(item, "id", &id_obj) &&
                    json_object_object_get_ex(item, "qp_used", &qp_obj)) {
                    uint32_t id = json_object_get_int(id_obj);
                    int qp = json_object_get_int(qp_obj);
                    int qp_limit = 0;
                    json_object_object_get_ex(item, "qp_limit", &qp_obj);
                    qp_limit = json_object_get_int(qp_obj);
                    
                    const char* name = "?";
                    if (json_object_object_get_ex(item, "name", &name_obj)) {
                        name = json_object_get_string(name_obj);
                    }
                    
                    printf("  Tenant %u (%s): QP=%d/%d", id, name, qp, qp_limit);
                    
                    if (json_object_object_get_ex(item, "mr_used", &mr_obj)) {
                        int mr = json_object_get_int(mr_obj);
                        int mr_limit = 0;
                        json_object_object_get_ex(item, "mr_limit", &mr_obj);
                        mr_limit = json_object_get_int(mr_obj);
                        printf(", MR=%d/%d", mr, mr_limit);
                    }
                    printf("\n");
                }
            }
        } 
        // 如果是租户列表响应
        else if (json_object_is_type(data_obj, json_type_object)) {
            json_object* count_obj, *tenants_obj, *version_obj;
            
            if (json_object_object_get_ex(data_obj, "count", &count_obj)) {
                printf("  Active tenants: %d\n", json_object_get_int(count_obj));
            }
            if (json_object_object_get_ex(data_obj, "version", &version_obj)) {
                printf("  Policy version: %lu\n", json_object_get_int64(version_obj));
            }
            
            if (json_object_object_get_ex(data_obj, "tenants", &tenants_obj) &&
                json_object_is_type(tenants_obj, json_type_array)) {
                int len = json_object_array_length(tenants_obj);
                printf("\n  Tenants:\n");
                for (int i = 0; i < len; i++) {
                    json_object* t = json_object_array_get_idx(tenants_obj, i);
                    json_object *id_obj, *name_obj, *qp_obj, *mr_obj;
                    
                    if (json_object_object_get_ex(t, "id", &id_obj)) {
                        uint32_t id = json_object_get_int(id_obj);
                        const char* name = "?";
                        int qp = 0, qp_limit = 0;
                        int mr = 0, mr_limit = 0;
                        
                        if (json_object_object_get_ex(t, "name", &name_obj))
                            name = json_object_get_string(name_obj);
                        if (json_object_object_get_ex(t, "qp_used", &qp_obj))
                            qp = json_object_get_int(qp_obj);
                        if (json_object_object_get_ex(t, "qp_limit", &qp_obj))
                            qp_limit = json_object_get_int(qp_obj);
                        if (json_object_object_get_ex(t, "mr_used", &mr_obj))
                            mr = json_object_get_int(mr_obj);
                        if (json_object_object_get_ex(t, "mr_limit", &mr_obj))
                            mr_limit = json_object_get_int(mr_obj);
                        
                        printf("    [%u] %s: QP=%d/%d, MR=%d/%d\n", 
                               id, name, qp, qp_limit, mr, mr_limit);
                    }
                }
            }
            // 单个租户状态
            else {
                json_object *id_obj, *name_obj, *qp_used, *qp_limit, *mr_used, *mr_limit;
                json_object *mem_used, *mem_limit, *total_qp, *total_mr;
                
                if (json_object_object_get_ex(data_obj, "id", &id_obj)) {
                    printf("  Tenant ID: %d\n", json_object_get_int(id_obj));
                }
                if (json_object_object_get_ex(data_obj, "name", &name_obj)) {
                    printf("  Name: %s\n", json_object_get_string(name_obj));
                }
                if (json_object_object_get_ex(data_obj, "status", &id_obj)) {
                    int status = json_object_get_int(id_obj);
                    printf("  Status: %s\n", status == 1 ? "ACTIVE" : (status == 2 ? "SUSPENDED" : "INACTIVE"));
                }
                if (json_object_object_get_ex(data_obj, "qp_used", &qp_used) &&
                    json_object_object_get_ex(data_obj, "qp_limit", &qp_limit)) {
                    printf("  QP: %d/%d\n", json_object_get_int(qp_used), json_object_get_int(qp_limit));
                }
                if (json_object_object_get_ex(data_obj, "mr_used", &mr_used) &&
                    json_object_object_get_ex(data_obj, "mr_limit", &mr_limit)) {
                    printf("  MR: %d/%d\n", json_object_get_int(mr_used), json_object_get_int(mr_limit));
                }
                if (json_object_object_get_ex(data_obj, "memory_used", &mem_used) &&
                    json_object_object_get_ex(data_obj, "memory_limit", &mem_limit)) {
                    printf("  Memory: %lu/%lu bytes\n", 
                           (unsigned long)json_object_get_int64(mem_used),
                           (unsigned long)json_object_get_int64(mem_limit));
                }
                if (json_object_object_get_ex(data_obj, "total_qp_creates", &total_qp)) {
                    printf("  Total QP creates: %lu\n", (unsigned long)json_object_get_int64(total_qp));
                }
                if (json_object_object_get_ex(data_obj, "total_mr_regs", &total_mr)) {
                    printf("  Total MR registers: %lu\n", (unsigned long)json_object_get_int64(total_mr));
                }
            }
        }
    }
    
    json_object_put(obj);
}

/* 构建JSON命令 */
char* build_create_cmd(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s create <tenant_id> <qp> <mr> [memory] [name]\n", argv[0]);
        return NULL;
    }
    
    uint32_t tenant_id = atoi(argv[2]);
    int qp = atoi(argv[3]);
    int mr = atoi(argv[4]);
    uint64_t mem = (argc > 5) ? (uint64_t)atoll(argv[5]) : 1073741824ULL;
    const char* name = (argc > 6) ? argv[6] : "unnamed";
    
    json_object* cmd = json_object_new_object();
    json_object_object_add(cmd, "cmd", json_object_new_string("CREATE"));
    json_object_object_add(cmd, "tenant", json_object_new_int(tenant_id));
    json_object_object_add(cmd, "name", json_object_new_string(name));
    json_object_object_add(cmd, "qp", json_object_new_int(qp));
    json_object_object_add(cmd, "mr", json_object_new_int(mr));
    json_object_object_add(cmd, "memory", json_object_new_int64(mem));
    
    const char* str = json_object_to_json_string(cmd);
    char* result = strdup(str);
    json_object_put(cmd);
    return result;
}

char* build_delete_cmd(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s delete <tenant_id>\n", argv[0]);
        return NULL;
    }
    
    uint32_t tenant_id = atoi(argv[2]);
    
    json_object* cmd = json_object_new_object();
    json_object_object_add(cmd, "cmd", json_object_new_string("DELETE"));
    json_object_object_add(cmd, "tenant", json_object_new_int(tenant_id));
    
    const char* str = json_object_to_json_string(cmd);
    char* result = strdup(str);
    json_object_put(cmd);
    return result;
}

char* build_update_cmd(int argc, char* argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s update <tenant_id> <qp> <mr> [memory]\n", argv[0]);
        fprintf(stderr, "\n  ★ Hot Update - No application restart needed!\n");
        return NULL;
    }
    
    uint32_t tenant_id = atoi(argv[2]);
    int qp = atoi(argv[3]);
    int mr = atoi(argv[4]);
    uint64_t mem = (argc > 5) ? (uint64_t)atoll(argv[5]) : 1073741824ULL;
    
    json_object* cmd = json_object_new_object();
    json_object_object_add(cmd, "cmd", json_object_new_string("UPDATE_QUOTA"));
    json_object_object_add(cmd, "tenant", json_object_new_int(tenant_id));
    json_object_object_add(cmd, "qp", json_object_new_int(qp));
    json_object_object_add(cmd, "mr", json_object_new_int(mr));
    json_object_object_add(cmd, "memory", json_object_new_int64(mem));
    
    const char* str = json_object_to_json_string(cmd);
    char* result = strdup(str);
    json_object_put(cmd);
    return result;
}

char* build_status_cmd(int argc, char* argv[]) {
    json_object* cmd = json_object_new_object();
    json_object_object_add(cmd, "cmd", json_object_new_string("STATUS"));
    
    if (argc > 2) {
        uint32_t tenant_id = atoi(argv[2]);
        json_object_object_add(cmd, "tenant", json_object_new_int(tenant_id));
    }
    
    const char* str = json_object_to_json_string(cmd);
    char* result = strdup(str);
    json_object_put(cmd);
    return result;
}

char* build_list_cmd(void) {
    json_object* cmd = json_object_new_object();
    json_object_object_add(cmd, "cmd", json_object_new_string("LIST_TENANTS"));
    
    const char* str = json_object_to_json_string(cmd);
    char* result = strdup(str);
    json_object_put(cmd);
    return result;
}

/* 打印用法 */
void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <command> [args...]\n", prog);
    fprintf(stderr, "\nCommands:\n");
    fprintf(stderr, "  create <tenant_id> <qp> <mr> [memory] [name]  Create a new tenant\n");
    fprintf(stderr, "  delete <tenant_id>                             Delete a tenant\n");
    fprintf(stderr, "  update <tenant_id> <qp> <mr> [memory]          ★ Hot update quota\n");
    fprintf(stderr, "  status [tenant_id]                             Show tenant status\n");
    fprintf(stderr, "  list                                           List all tenants\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s create 20 100 100 1073741824 \"TestTenant\"\n", prog);
    fprintf(stderr, "  %s update 20 50 50           # Reduce quota dynamically\n", prog);
    fprintf(stderr, "  %s status 20                  # Show specific tenant\n", prog);
    fprintf(stderr, "  %s list                       # Show all tenants\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    char* json_cmd = NULL;
    
    if (strcmp(argv[1], "create") == 0) {
        json_cmd = build_create_cmd(argc, argv);
    } else if (strcmp(argv[1], "delete") == 0) {
        json_cmd = build_delete_cmd(argc, argv);
    } else if (strcmp(argv[1], "update") == 0 || strcmp(argv[1], "set-quota") == 0) {
        json_cmd = build_update_cmd(argc, argv);
    } else if (strcmp(argv[1], "status") == 0) {
        json_cmd = build_status_cmd(argc, argv);
    } else if (strcmp(argv[1], "list") == 0) {
        json_cmd = build_list_cmd();
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }
    
    if (!json_cmd) {
        return 1;
    }
    
    printf("Sending: %s\n", json_cmd);
    
    char* response = send_command(json_cmd);
    free(json_cmd);
    
    if (!response) {
        fprintf(stderr, "Failed to communicate with daemon\n");
        fprintf(stderr, "Is the daemon running? Start with: tenant_manager_daemon --daemon --foreground\n");
        return 1;
    }
    
    printf("\nResponse:\n");
    print_response(response);
    free(response);
    
    return 0;
}
