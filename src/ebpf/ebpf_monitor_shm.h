#ifndef EBPF_MONITOR_SHM_H
#define EBPF_MONITOR_SHM_H

#include <stdint.h>
#include "../shm/shared_memory.h"

// 初始化eBPF监控（使用共享内存）
int ebpf_monitor_init(void);

// 关闭eBPF监控
void ebpf_monitor_cleanup(void);

// 获取进程资源使用情况（通过共享内存）
int ebpf_get_process_resources(int pid, resource_usage_t *usage);

// 获取全局资源使用情况（通过共享内存）
int ebpf_get_global_resources(resource_usage_t *usage);

// 更新进程资源使用情况（通过共享内存）
int ebpf_update_process_resources(int pid, const resource_usage_t *usage);

// 更新全局资源使用情况（通过共享内存）
int ebpf_update_global_resources(const resource_usage_t *usage);

#endif // EBPF_MONITOR_SHM_H