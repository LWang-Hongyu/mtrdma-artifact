#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include "shm/shared_memory.h"

// 从共享内存获取进程资源使用情况
static int get_process_resources_via_shared_memory(int pid, resource_usage_t *usage)
{
    if (!usage) {
        return -1;
    }
    
    int result = shm_get_process_resources(pid, usage);
    if (result == 0) {
        fprintf(stderr, "[EBPF_MONITOR_SHM] 从共享内存获取进程资源使用情况成功: PID=%d, QP=%d, MR=%d, Memory=%llu\n", 
                pid, usage->qp_count, usage->mr_count, (unsigned long long)usage->memory_used);
    } else {
        // 如果获取失败，返回0值
        memset(usage, 0, sizeof(resource_usage_t));
        fprintf(stderr, "[EBPF_MONITOR_SHM] 无法从共享内存获取进程资源使用情况: PID=%d\n", pid);
    }
    
    return result;
}

// 从共享内存获取全局资源使用情况
static int get_global_resources_via_shared_memory(resource_usage_t *usage)
{
    if (!usage) {
        return -1;
    }
    
    int result = shm_get_global_resources(usage);
    if (result == 0) {
        fprintf(stderr, "[EBPF_MONITOR_SHM] 从共享内存获取全局资源使用情况成功: QP=%d, MR=%d, Memory=%llu\n", 
                usage->qp_count, usage->mr_count, (unsigned long long)usage->memory_used);
    } else {
        // 如果获取失败，返回0值
        memset(usage, 0, sizeof(resource_usage_t));
        fprintf(stderr, "[EBPF_MONITOR_SHM] 无法从共享内存获取全局资源使用情况\n");
    }
    
    return result;
}

// 初始化共享内存监控
int ebpf_monitor_init(void)
{
    fprintf(stderr, "[EBPF_MONITOR_SHM] 开始初始化共享内存监控\n");
    
    // 尝试初始化共享内存
    if (shm_init() != 0) {
        fprintf(stderr, "[EBPF_MONITOR_SHM] 初始化共享内存失败\n");
        return -1;
    }
    
    fprintf(stderr, "[EBPF_MONITOR_SHM] 共享内存监控初始化成功\n");
    return 0;
}

// 清理共享内存监控
void ebpf_monitor_cleanup(void)
{
    fprintf(stderr, "[EBPF_MONITOR_SHM] 清理共享内存监控\n");
    // 注意：不在此处销毁共享内存，因为它可能被其他进程使用
}

// 获取进程资源使用情况
int ebpf_get_process_resources(int pid, resource_usage_t *usage)
{
    return get_process_resources_via_shared_memory(pid, usage);
}

// 获取全局资源使用情况
int ebpf_get_global_resources(resource_usage_t *usage)
{
    return get_global_resources_via_shared_memory(usage);
}

// 更新进程资源使用情况
int ebpf_update_process_resources(int pid, const resource_usage_t *usage)
{
    if (!usage) {
        return -1;
    }
    
    int result = shm_update_process_resources(pid, usage);
    if (result == 0) {
        fprintf(stderr, "[EBPF_MONITOR_SHM] 成功更新进程资源使用情况: PID=%d, QP=%d, MR=%d, Memory=%llu\n", 
                pid, usage->qp_count, usage->mr_count, (unsigned long long)usage->memory_used);
    } else {
        fprintf(stderr, "[EBPF_MONITOR_SHM] 无法更新进程资源使用情况: PID=%d\n", pid);
    }
    
    return result;
}

// 更新全局资源使用情况
int ebpf_update_global_resources(const resource_usage_t *usage)
{
    if (!usage) {
        return -1;
    }
    
    int result = shm_update_global_resources(usage);
    if (result == 0) {
        fprintf(stderr, "[EBPF_MONITOR_SHM] 成功更新全局资源使用情况: QP=%d, MR=%d, Memory=%llu\n", 
                usage->qp_count, usage->mr_count, (unsigned long long)usage->memory_used);
    } else {
        fprintf(stderr, "[EBPF_MONITOR_SHM] 无法更新全局资源使用情况\n");
    }
    
    return result;
}