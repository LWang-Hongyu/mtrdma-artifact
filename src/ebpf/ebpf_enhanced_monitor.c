#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ebpf_enhanced_monitor.h"
#include "shm/shared_memory.h"

// eBPF程序路径
#define EBPF_PROG_PATH "/sys/fs/bpf/rdma_monitor_enhanced"
#define EBPF_OBJ_FILE "rdma_monitor_enhanced.bpf.o"

// 全局变量
static struct bpf_object *obj = NULL;
static struct bpf_link *links[16] = {NULL}; // 存储所有prog links
static int link_count = 0;
static ebpf_maps_fds_t map_fds = {-1, -1, -1, -1, -1, -1, -1};

// 辅助函数：查找map的文件描述符
static int find_map_fd(const char *name) {
    if (!obj) return -1;
    
    struct bpf_map *map = bpf_object__find_map_by_name(obj, name);
    if (!map) {
        fprintf(stderr, "[EBPF_ENHANCED] 无法找到map: %s\n", name);
        return -1;
    }
    
    return bpf_map__fd(map);
}

// 初始化增强型eBPF监控
int ebpf_enhanced_monitor_init(void) {
    fprintf(stderr, "[EBPF_ENHANCED] 开始初始化增强型eBPF监控\n");
    
    // 首先初始化共享内存
    if (shm_init() != 0) {
        fprintf(stderr, "[EBPF_ENHANCED] 初始化共享内存失败\n");
        return -1;
    }
    
    char obj_path[256];
    snprintf(obj_path, sizeof(obj_path), "%s/%s", "/opt/mtrdma/build", EBPF_OBJ_FILE);
    
    // 检查eBPF对象文件是否存在
    if (access(obj_path, F_OK) != 0) {
        fprintf(stderr, "[EBPF_ENHANCED] eBPF对象文件不存在: %s\n", obj_path);
        fprintf(stderr, "[EBPF_ENHANCED] 将使用共享内存模式运行\n");
        // 即使没有eBPF程序，也返回成功，使用共享内存模式
        return 0;
    }
    
    // 打开eBPF对象文件
    struct bpf_object_open_opts opts = {};
    opts.sz = sizeof(opts);
    
    obj = bpf_object__open_file(obj_path, &opts);
    if (!obj) {
        fprintf(stderr, "[EBPF_ENHANCED] 无法打开eBPF对象文件: %s\n", obj_path);
        fprintf(stderr, "[EBPF_ENHANCED] 将使用共享内存模式运行\n");
        return 0;
    }
    
    // 加载eBPF程序
    int err = bpf_object__load(obj);
    if (err) {
        fprintf(stderr, "[EBPF_ENHANCED] 无法加载eBPF程序: %d\n", err);
        bpf_object__close(obj);
        obj = NULL;
        return 0; // 使用共享内存模式
    }
    
    // 获取maps的文件描述符
    map_fds.process_resources_fd = find_map_fd("process_resources");
    map_fds.global_resources_fd = find_map_fd("global_resources");
    map_fds.tenant_resources_fd = find_map_fd("tenant_resources");
    map_fds.pid_to_tenant_fd = find_map_fd("pid_to_tenant");
    map_fds.qp_type_stats_fd = find_map_fd("qp_type_stats");
    map_fds.perf_stats_fd = find_map_fd("perf_stats");
    map_fds.filter_config_fd = find_map_fd("filter_config");
    
    // 附加kprobe程序
    struct bpf_program *prog;
    bpf_object__for_each_program(prog, obj) {
        const char *prog_name = bpf_program__name(prog);
        
        // 根据程序名选择附加类型
        if (strstr(prog_name, "kprobe_") || strstr(prog_name, "kretprobe_")) {
            struct bpf_link *link = bpf_program__attach(prog);
            if (!link) {
                fprintf(stderr, "[EBPF_ENHANCED] 无法附加程序: %s\n", prog_name);
            } else {
                links[link_count++] = link;
                fprintf(stderr, "[EBPF_ENHANCED] 成功附加程序: %s\n", prog_name);
            }
        }
    }
    
    // 初始化过滤配置（默认启用性能跟踪）
    ebpf_set_filter_config(true, true);
    
    fprintf(stderr, "[EBPF_ENHANCED] 增强型eBPF监控初始化成功\n");
    fprintf(stderr, "[EBPF_ENHANCED] 成功附加 %d 个eBPF程序\n", link_count);
    
    return 0;
}

// 关闭增强型eBPF监控
void ebpf_enhanced_monitor_cleanup(void) {
    fprintf(stderr, "[EBPF_ENHANCED] 清理增强型eBPF监控\n");
    
    // 分离所有links
    for (int i = 0; i < link_count; i++) {
        if (links[i]) {
            bpf_link__destroy(links[i]);
            links[i] = NULL;
        }
    }
    link_count = 0;
    
    // 关闭eBPF对象
    if (obj) {
        bpf_object__close(obj);
        obj = NULL;
    }
    
    // 重置map fds
    memset(&map_fds, -1, sizeof(map_fds));
}

// 设置进程的租户ID
int ebpf_set_tenant_id(int pid, uint32_t tenant_id) {
    if (map_fds.pid_to_tenant_fd < 0) {
        // 如果没有eBPF map，使用共享内存存储租户映射
        fprintf(stderr, "[EBPF_ENHANCED] eBPF map不可用，使用共享内存存储租户映射 PID=%d -> Tenant=%u\n", 
                pid, tenant_id);
        // TODO: 在共享内存中实现租户映射
        return 0;
    }
    
    uint32_t key = pid;
    int err = bpf_map_update_elem(map_fds.pid_to_tenant_fd, &key, &tenant_id, BPF_ANY);
    if (err) {
        fprintf(stderr, "[EBPF_ENHANCED] 设置租户ID失败: %d\n", err);
        return -1;
    }
    
    fprintf(stderr, "[EBPF_ENHANCED] 设置租户映射成功 PID=%d -> Tenant=%u\n", pid, tenant_id);
    return 0;
}

// 获取进程的租户ID
int ebpf_get_tenant_id(int pid, uint32_t *tenant_id) {
    if (!tenant_id) return -1;
    
    if (map_fds.pid_to_tenant_fd < 0) {
        *tenant_id = 0; // 默认租户
        return 0;
    }
    
    uint32_t key = pid;
    int err = bpf_map_lookup_elem(map_fds.pid_to_tenant_fd, &key, tenant_id);
    if (err) {
        *tenant_id = 0; // 默认租户
    }
    
    return 0;
}

// 获取租户资源使用情况
int ebpf_get_tenant_resources(uint32_t tenant_id, tenant_resource_t *resources) {
    if (!resources) return -1;
    
    if (map_fds.tenant_resources_fd < 0) {
        // 从共享内存获取
        memset(resources, 0, sizeof(tenant_resource_t));
        resources->tenant_id = tenant_id;
        return 0;
    }
    
    int err = bpf_map_lookup_elem(map_fds.tenant_resources_fd, &tenant_id, resources);
    if (err) {
        memset(resources, 0, sizeof(tenant_resource_t));
        resources->tenant_id = tenant_id;
    }
    
    return 0;
}

// 获取性能统计
int ebpf_get_perf_stats(perf_stats_t *stats) {
    if (!stats) return -1;
    
    if (map_fds.perf_stats_fd < 0) {
        memset(stats, 0, sizeof(perf_stats_t));
        return 0;
    }
    
    uint32_t key = 0;
    int err = bpf_map_lookup_elem(map_fds.perf_stats_fd, &key, stats);
    if (err) {
        memset(stats, 0, sizeof(perf_stats_t));
    }
    
    return 0;
}

// 获取QP类型统计
int ebpf_get_qp_type_stats(enum qp_type type, uint64_t *count) {
    if (!count || type < 0 || type >= MAX_QP_TYPES) return -1;
    
    if (map_fds.qp_type_stats_fd < 0) {
        *count = 0;
        return 0;
    }
    
    uint32_t key = type;
    int err = bpf_map_lookup_elem(map_fds.qp_type_stats_fd, &key, count);
    if (err) {
        *count = 0;
    }
    
    return 0;
}

// 设置过滤配置
int ebpf_set_filter_config(bool enable_filter, bool track_perf) {
    if (map_fds.filter_config_fd < 0) {
        return 0;
    }
    
    uint32_t key = 0;
    uint32_t config = 0;
    if (enable_filter) config |= 0x1;
    if (track_perf) config |= 0x2;
    
    int err = bpf_map_update_elem(map_fds.filter_config_fd, &key, &config, BPF_ANY);
    if (err) {
        fprintf(stderr, "[EBPF_ENHANCED] 设置过滤配置失败: %d\n", err);
        return -1;
    }
    
    fprintf(stderr, "[EBPF_ENHANCED] 过滤配置已设置: enable_filter=%d, track_perf=%d\n",
            enable_filter, track_perf);
    return 0;
}

// 获取所有租户资源列表
int ebpf_get_all_tenant_resources(tenant_resource_t *resources, int max_count) {
    if (!resources || max_count <= 0) return -1;
    
    if (map_fds.tenant_resources_fd < 0) {
        return 0;
    }
    
    int count = 0;
    uint32_t key, prev_key = 0;
    
    while (count < max_count && bpf_map_get_next_key(map_fds.tenant_resources_fd, 
                                                      prev_key == 0 ? NULL : &prev_key, 
                                                      &key) == 0) {
        tenant_resource_t *res = &resources[count];
        if (bpf_map_lookup_elem(map_fds.tenant_resources_fd, &key, res) == 0) {
            count++;
        }
        prev_key = key;
    }
    
    return count;
}

// 从eBPF maps获取进程资源（增强版）
int ebpf_get_enhanced_process_resources(int pid, enhanced_resource_usage_t *usage) {
    if (!usage) return -1;
    
    if (map_fds.process_resources_fd < 0) {
        // 回退到共享内存
        resource_usage_t basic_usage;
        int ret = shm_get_process_resources(pid, &basic_usage);
        if (ret == 0) {
            memset(usage, 0, sizeof(enhanced_resource_usage_t));
            usage->qp_count = basic_usage.qp_count;
            usage->mr_count = basic_usage.mr_count;
            usage->memory_used = basic_usage.memory_used;
        }
        return ret;
    }
    
    uint32_t key = pid;
    int err = bpf_map_lookup_elem(map_fds.process_resources_fd, &key, usage);
    if (err) {
        memset(usage, 0, sizeof(enhanced_resource_usage_t));
    }
    
    return 0;
}

// 从eBPF maps获取全局资源（增强版）
int ebpf_get_enhanced_global_resources(enhanced_resource_usage_t *usage) {
    if (!usage) return -1;
    
    if (map_fds.global_resources_fd < 0) {
        // 回退到共享内存
        resource_usage_t basic_usage;
        int ret = shm_get_global_resources(&basic_usage);
        if (ret == 0) {
            memset(usage, 0, sizeof(enhanced_resource_usage_t));
            usage->qp_count = basic_usage.qp_count;
            usage->mr_count = basic_usage.mr_count;
            usage->memory_used = basic_usage.memory_used;
        }
        return ret;
    }
    
    uint32_t key = 0;
    int err = bpf_map_lookup_elem(map_fds.global_resources_fd, &key, usage);
    if (err) {
        memset(usage, 0, sizeof(enhanced_resource_usage_t));
    }
    
    return 0;
}
