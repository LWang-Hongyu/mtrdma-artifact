#ifndef EBPF_ENHANCED_MONITOR_H
#define EBPF_ENHANCED_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

// 最大支持的租户数
#define MAX_TENANTS 1024
#define MAX_QP_TYPES 4

// QP类型枚举
enum qp_type {
    QP_TYPE_RC = 0,  // Reliable Connection
    QP_TYPE_UC = 1,  // Unreliable Connection
    QP_TYPE_UD = 2,  // Unreliable Datagram
    QP_TYPE_XRC = 3, // Extended Reliable Connection
};

// 增强的资源使用情况结构
typedef struct {
    int qp_count;
    int mr_count;
    int cq_count;
    int pd_count;
    uint64_t memory_used;
    uint64_t total_qp_creates;    // 总创建次数统计
    uint64_t total_qp_destroys;   // 总销毁次数统计
    uint64_t total_mr_regs;       // 总注册次数统计
    uint64_t total_mr_deregs;     // 总注销次数统计
} enhanced_resource_usage_t;

// 性能统计结构
typedef struct {
    uint64_t create_qp_lat_total;  // QP创建延迟总和(ns)
    uint64_t reg_mr_lat_total;     // MR注册延迟总和(ns)
    uint64_t create_qp_count;      // QP创建计数
    uint64_t reg_mr_count;         // MR注册计数
} perf_stats_t;

// 租户资源结构
typedef struct {
    uint32_t tenant_id;
    enhanced_resource_usage_t usage;
    perf_stats_t perf;
} tenant_resource_t;

// eBPF maps文件描述符结构
typedef struct {
    int process_resources_fd;
    int global_resources_fd;
    int tenant_resources_fd;
    int pid_to_tenant_fd;
    int qp_type_stats_fd;
    int perf_stats_fd;
    int filter_config_fd;
} ebpf_maps_fds_t;

// 初始化增强型eBPF监控
int ebpf_enhanced_monitor_init(void);

// 关闭增强型eBPF监控
void ebpf_enhanced_monitor_cleanup(void);

// 设置进程的租户ID
int ebpf_set_tenant_id(int pid, uint32_t tenant_id);

// 获取进程的租户ID
int ebpf_get_tenant_id(int pid, uint32_t *tenant_id);

// 获取租户资源使用情况
int ebpf_get_tenant_resources(uint32_t tenant_id, tenant_resource_t *resources);

// 获取性能统计
int ebpf_get_perf_stats(perf_stats_t *stats);

// 获取QP类型统计
int ebpf_get_qp_type_stats(enum qp_type type, uint64_t *count);

// 设置过滤配置
int ebpf_set_filter_config(bool enable_filter, bool track_perf);

// 获取所有租户资源列表
int ebpf_get_all_tenant_resources(tenant_resource_t *resources, int max_count);

// 从eBPF maps获取进程资源（增强版）
int ebpf_get_enhanced_process_resources(int pid, enhanced_resource_usage_t *usage);

// 从eBPF maps获取全局资源（增强版）
int ebpf_get_enhanced_global_resources(enhanced_resource_usage_t *usage);

#endif // EBPF_ENHANCED_MONITOR_H
