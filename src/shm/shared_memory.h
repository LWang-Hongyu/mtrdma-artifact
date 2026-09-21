#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include <sys/types.h>

#define MAX_PROCESSES 1024
#define SHM_NAME "/rdma_intercept_shm_v2"
#define SHM_SIZE 4096

// 资源使用情况结构
typedef struct {
    int qp_count;
    int mr_count;
    uint64_t memory_used;
} resource_usage_t;

// 共享内存数据结构
typedef struct {
    // 全局资源统计
    resource_usage_t global_stats;
    
    // 进程资源统计数组
    resource_usage_t process_stats[MAX_PROCESSES];
    
    // 进程ID映射（用于快速查找）
    pid_t process_pids[MAX_PROCESSES];
    
    // 进程启动时间（来自 /proc/[pid]/stat 的 starttime）
    // 用于 GC 线程检测 PID 重用：若 PID 相同但 starttime 不同，说明原进程已死，PID 被重用
    uint64_t process_start_times[MAX_PROCESSES];
    
    // 全局配置参数
    uint32_t max_global_qp;
    uint32_t max_global_mr;
    uint64_t max_global_memory;
    
    // 同步机制 - 自旋锁
    volatile int shm_lock;
    
    // 数据版本号，用于检测更新
    volatile uint64_t version;
    
    // 最后更新时间戳
    uint64_t last_update_time;
} shared_memory_data_t;

// 共享内存操作函数声明

/**
 * 初始化共享内存
 * @return 0成功，-1失败
 */
int shm_init(void);

/**
 * 销毁共享内存
 * @return 0成功，-1失败
 */
int shm_destroy(void);

/**
 * 获取共享内存指针
 * @return 共享内存数据指针，NULL表示失败
 */
shared_memory_data_t* shm_get_ptr(void);

/**
 * 加锁
 * @param data 共享内存数据指针
 */
void shm_lock(shared_memory_data_t* data);

/**
 * 解锁
 * @param data 共享内存数据指针
 */
void shm_unlock(shared_memory_data_t* data);

/**
 * 获取全局资源使用情况
 * @param usage 输出参数，资源使用情况
 * @return 0成功，-1失败
 */
int shm_get_global_resources(resource_usage_t* usage);

/**
 * 获取指定进程的资源使用情况
 * @param pid 进程ID
 * @param usage 输出参数，资源使用情况
 * @return 0成功，-1失败
 */
int shm_get_process_resources(pid_t pid, resource_usage_t* usage);

/**
 * 更新全局资源使用情况
 * @param usage 新的资源使用情况
 * @return 0成功，-1失败
 */
int shm_update_global_resources(const resource_usage_t* usage);

/**
 * 更新指定进程的资源使用情况
 * @param pid 进程ID
 * @param usage 新的资源使用情况
 * @return 0成功，-1失败
 */
int shm_update_process_resources(pid_t pid, const resource_usage_t* usage);

/**
 * 设置全局资源限制
 * @param max_qp 最大QP数量
 * @param max_mr 最大MR数量
 * @param max_memory 最大内存使用
 * @return 0成功，-1失败
 */
int shm_set_global_limits(uint32_t max_qp, uint32_t max_mr, uint64_t max_memory);

/**
 * 清空指定进程的共享内存条目，并从全局计数中减去其占用的资源
 * 由 GC 线程在检测到进程死亡后调用
 * @param slot 进程条目索引
 * @param out_qp 输出参数，被清理的 QP 数（用于租户计数调整）
 * @param out_mr 输出参数，被清理的 MR 数
 * @param out_mem 输出参数，被清理的内存字节数
 * @return 0成功，-1失败（slot无效）
 */
int shm_clear_process_entry(int slot, int *out_qp, int *out_mr, uint64_t *out_mem);

/**
 * 读取 /proc/[pid]/stat 获取进程启动时间（jiffies since boot）
 * 返回 0 表示读取失败（进程不存在等）
 * @param pid 进程ID
 * @return 启动时间（单位：jiffies），0 表示失败
 */
uint64_t shm_read_proc_start_time(pid_t pid);

#endif // SHARED_MEMORY_H