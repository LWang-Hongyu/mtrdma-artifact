#ifndef DYNAMIC_POLICY_H
#define DYNAMIC_POLICY_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// 策略类型
enum policy_type {
    POLICY_TYPE_STATIC = 0,   // 静态策略
    POLICY_TYPE_DYNAMIC = 1,  // 动态策略
    POLICY_TYPE_ADAPTIVE = 2, // 自适应策略
};

// 资源限制类型
enum resource_type {
    RESOURCE_QP = 0,      // Queue Pair
    RESOURCE_MR = 1,      // Memory Region
    RESOURCE_CQ = 2,      // Completion Queue
    RESOURCE_PD = 3,      // Protection Domain
    RESOURCE_MEMORY = 4,  // Memory
};

// 资源限制结构
typedef struct {
    uint32_t max_per_process;  // 每进程限制
    uint32_t max_global;       // 全局限制
    uint64_t max_memory;       // 内存限制（字节）
} resource_limit_t;

// 策略规则结构
typedef struct {
    uint32_t rule_id;
    enum policy_type type;
    enum resource_type resource;
    resource_limit_t limit;
    uint32_t priority;         // 优先级，数值越高优先级越高
    time_t created_at;
    time_t expires_at;         // 过期时间，0表示永不过期
    bool enabled;
} policy_rule_t;

// 动态策略配置
typedef struct {
    policy_rule_t qp_policy;
    policy_rule_t mr_policy;
    policy_rule_t cq_policy;
    policy_rule_t pd_policy;
    policy_rule_t memory_policy;
    
    // 动态调整参数
    bool auto_adjust;          // 是否自动调整
    uint32_t adjust_interval;  // 调整间隔（秒）
    float high_watermark;      // 高水位线（0.0-1.0）
    float low_watermark;       // 低水位线（0.0-1.0）
} dynamic_policy_config_t;

// 租户策略（用于多租户）
typedef struct {
    uint32_t tenant_id;
    char tenant_name[64];
    dynamic_policy_config_t policy;
    time_t last_updated;
} tenant_policy_t;

// 初始化动态策略模块
int dynamic_policy_init(void);

// 清理动态策略模块
void dynamic_policy_cleanup(void);

// 加载策略配置文件
int dynamic_policy_load_config(const char *config_path);

// 保存策略配置到文件
int dynamic_policy_save_config(const char *config_path);

// 设置资源限制
int dynamic_policy_set_limit(enum resource_type resource, const resource_limit_t *limit);

// 获取资源限制
int dynamic_policy_get_limit(enum resource_type resource, resource_limit_t *limit);

// 设置租户策略
int dynamic_policy_set_tenant_policy(uint32_t tenant_id, const tenant_policy_t *policy);

// 获取租户策略
int dynamic_policy_get_tenant_policy(uint32_t tenant_id, tenant_policy_t *policy);

// 删除租户策略
int dynamic_policy_delete_tenant_policy(uint32_t tenant_id);

// 启用/禁用策略规则
int dynamic_policy_enable_rule(enum resource_type resource, bool enable);

// 检查资源是否超出限制
bool dynamic_policy_check_limit(enum resource_type resource, 
                                 uint32_t current_usage, 
                                 uint32_t requested_amount);

// 检查租户资源是否超出限制
bool dynamic_policy_check_tenant_limit(uint32_t tenant_id,
                                        enum resource_type resource,
                                        uint32_t current_usage,
                                        uint32_t requested_amount);

// 更新策略（热更新）
int dynamic_policy_update(const dynamic_policy_config_t *config);

// 获取当前策略配置
int dynamic_policy_get_config(dynamic_policy_config_t *config);

// 设置自动调整参数
int dynamic_policy_set_auto_adjust(bool enable, 
                                    uint32_t interval,
                                    float high_watermark, 
                                    float low_watermark);

// 执行一次策略调整（动态调整资源限制）
int dynamic_policy_adjust(void);

// 获取所有租户策略列表
int dynamic_policy_get_all_tenant_policies(tenant_policy_t *policies, int max_count);

// 设置默认策略
int dynamic_policy_set_default(void);

// 打印当前策略配置（用于调试）
void dynamic_policy_print_config(void);

#endif // DYNAMIC_POLICY_H
