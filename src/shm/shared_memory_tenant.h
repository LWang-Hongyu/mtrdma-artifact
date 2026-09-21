#ifndef SHARED_MEMORY_TENANT_H
#define SHARED_MEMORY_TENANT_H

#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>
#include "shared_memory.h"

// 最大租户数
#define MAX_TENANTS 128
#define TENANT_NAME_MAX 64
#define TENANT_SHM_NAME "/rdma_intercept_tenant_shm_v2"

// 租户状态
enum tenant_status {
    TENANT_STATUS_INACTIVE = 0,  // 未激活
    TENANT_STATUS_ACTIVE = 1,    // 激活
    TENANT_STATUS_SUSPENDED = 2, // 暂停
};

// 租户资源配额
typedef struct {
    uint32_t max_qp_per_tenant;      // 每租户最大QP数
    uint32_t max_mr_per_tenant;      // 每租户最大MR数
    uint64_t max_memory_per_tenant;  // 每租户最大内存
    uint32_t max_cq_per_tenant;      // 每租户最大CQ数
    uint32_t max_pd_per_tenant;      // 每租户最大PD数
} tenant_quota_t;

// 租户资源使用统计
typedef struct {
    int qp_count;
    int mr_count;
    int cq_count;
    int pd_count;
    uint64_t memory_used;
    uint64_t total_qp_creates;
    uint64_t total_qp_destroys;
    uint64_t total_mr_regs;
    uint64_t total_mr_deregs;
} tenant_resource_usage_t;

// 租户信息结构
typedef struct {
    uint32_t tenant_id;                          // 租户ID
    char tenant_name[TENANT_NAME_MAX];           // 租户名称
    enum tenant_status status;                   // 租户状态
    tenant_quota_t quota;                        // 资源配额
    tenant_resource_usage_t usage;               // 资源使用
    time_t created_at;                           // 创建时间
    time_t last_active_at;                       // 最后活跃时间
    uint32_t process_count;                      // 关联的进程数
    pid_t processes[MAX_PROCESSES];              // 关联的进程列表
} tenant_info_t;

// 进程到租户的映射
typedef struct {
    pid_t pid;           // 进程ID
    uint32_t tenant_id;  // 租户ID
    time_t mapped_at;    // 映射时间
} pid_tenant_mapping_t;

// 租户共享内存数据结构
typedef struct {
    // 租户信息数组
    tenant_info_t tenants[MAX_TENANTS];
    
    // 进程到租户的映射
    pid_tenant_mapping_t pid_mappings[MAX_PROCESSES];
    
    // 全局租户统计
    uint32_t active_tenant_count;
    uint32_t total_process_count;
    
    // 同步机制
    volatile int tenant_shm_lock;
    
    // 数据版本号
    volatile uint64_t version;
    
    // 最后更新时间
    uint64_t last_update_time;
} tenant_shared_memory_t;

// ========== 租户管理API ==========

/**
 * 初始化租户共享内存
 * @return 0成功，-1失败
 */
int tenant_shm_init(void);

/**
 * 销毁租户共享内存
 * @return 0成功，-1失败
 */
int tenant_shm_destroy(void);

/**
 * 获取租户共享内存指针
 * @return 共享内存数据指针，NULL表示失败
 */
tenant_shared_memory_t* tenant_shm_get_ptr(void);

/**
 * 加锁
 * @param data 共享内存数据指针
 */
void tenant_shm_lock(tenant_shared_memory_t* data);

/**
 * 解锁
 * @param data 共享内存数据指针
 */
void tenant_shm_unlock(tenant_shared_memory_t* data);

/**
 * 创建租户
 * @param tenant_id 租户ID
 * @param name 租户名称
 * @param quota 资源配额
 * @return 0成功，-1失败
 */
int tenant_create(uint32_t tenant_id, const char *name, const tenant_quota_t *quota);

/**
 * 删除租户
 * @param tenant_id 租户ID
 * @return 0成功，-1失败
 */
int tenant_delete(uint32_t tenant_id);

/**
 * 获取租户信息
 * @param tenant_id 租户ID
 * @param info 输出参数，租户信息
 * @return 0成功，-1失败
 */
int tenant_get_info(uint32_t tenant_id, tenant_info_t *info);

/**
 * 设置租户状态
 * @param tenant_id 租户ID
 * @param status 新状态
 * @return 0成功，-1失败
 */
int tenant_set_status(uint32_t tenant_id, enum tenant_status status);

/**
 * 更新租户配额
 * @param tenant_id 租户ID
 * @param quota 新配额
 * @return 0成功，-1失败
 */
int tenant_update_quota(uint32_t tenant_id, const tenant_quota_t *quota);

/**
 * 将进程绑定到租户
 * @param pid 进程ID
 * @param tenant_id 租户ID
 * @return 0成功，-1失败
 */
int tenant_bind_process(pid_t pid, uint32_t tenant_id);

/**
 * 解除进程与租户的绑定
 * @param pid 进程ID
 * @return 0成功，-1失败
 */
int tenant_unbind_process(pid_t pid);

/**
 * 获取进程所属的租户ID
 * @param pid 进程ID
 * @param tenant_id 输出参数，租户ID
 * @return 0成功，-1失败
 */
int tenant_get_process_tenant(pid_t pid, uint32_t *tenant_id);

/**
 * 更新租户资源使用
 * @param tenant_id 租户ID
 * @param usage 资源使用情况
 * @return 0成功，-1失败
 */
int tenant_update_resource_usage(uint32_t tenant_id, const tenant_resource_usage_t *usage);

/**
 * 获取租户资源使用
 * @param tenant_id 租户ID
 * @param usage 输出参数，资源使用情况
 * @return 0成功，-1失败
 */
int tenant_get_resource_usage(uint32_t tenant_id, tenant_resource_usage_t *usage);

/**
 * 检查租户资源限制
 * @param tenant_id 租户ID
 * @param resource_type 资源类型（0=QP, 1=MR, 2=Memory）
 * @param requested_amount 请求的资源量
 * @return true超出限制，false未超出
 */
bool tenant_check_resource_limit(uint32_t tenant_id, int resource_type, uint32_t requested_amount);

/**
 * 原子方式尝试分配一个QP（检查配额 + 递增计数在同一锁中完成）
 * @param tenant_id 租户ID
 * @return true分配成功（配额未超），false配额已满
 */
bool tenant_try_allocate_qp(uint32_t tenant_id);

/**
 * 获取所有活跃租户列表
 * @param tenants 输出参数，租户信息数组
 * @param max_count 最大返回数量
 * @return 实际返回的租户数量
 */
int tenant_get_active_list(tenant_info_t *tenants, int max_count);

/**
 * 打印所有租户信息（调试用）
 */
void tenant_print_all(void);

/**
 * 获取租户统计信息
 * @param tenant_id 租户ID
 * @param total_qp_creates 输出参数，总QP创建数
 * @param total_mr_regs 输出参数，总MR注册数
 * @return 0成功，-1失败
 */
int tenant_get_statistics(uint32_t tenant_id, uint64_t *total_qp_creates, uint64_t *total_mr_regs);

#endif // SHARED_MEMORY_TENANT_H
