#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include "dynamic_policy.h"
#include "shm/shared_memory.h"
#include "shm/shared_memory_tenant.h"

// 全局策略配置
static dynamic_policy_config_t g_policy_config;
static pthread_mutex_t g_policy_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_adjust_thread;
static volatile int g_running = 0;

// 默认配置
#define DEFAULT_MAX_QP_PER_PROCESS 100
#define DEFAULT_MAX_GLOBAL_QP 1000
#define DEFAULT_MAX_MR_PER_PROCESS 1000
#define DEFAULT_MAX_GLOBAL_MR 10000
#define DEFAULT_MAX_MEMORY_PER_PROCESS (10ULL * 1024 * 1024 * 1024)  // 10GB
#define DEFAULT_ADJUST_INTERVAL 60  // 60秒

// 初始化默认策略规则
static void init_default_rule(policy_rule_t *rule, enum resource_type resource) {
    memset(rule, 0, sizeof(policy_rule_t));
    rule->rule_id = resource;
    rule->type = POLICY_TYPE_STATIC;
    rule->resource = resource;
    rule->priority = 1;
    rule->created_at = time(NULL);
    rule->expires_at = 0;
    rule->enabled = true;
}

// 设置默认策略配置
static void set_default_config(dynamic_policy_config_t *config) {
    memset(config, 0, sizeof(dynamic_policy_config_t));
    
    // QP策略
    init_default_rule(&config->qp_policy, RESOURCE_QP);
    config->qp_policy.limit.max_per_process = DEFAULT_MAX_QP_PER_PROCESS;
    config->qp_policy.limit.max_global = DEFAULT_MAX_GLOBAL_QP;
    
    // MR策略
    init_default_rule(&config->mr_policy, RESOURCE_MR);
    config->mr_policy.limit.max_per_process = DEFAULT_MAX_MR_PER_PROCESS;
    config->mr_policy.limit.max_global = DEFAULT_MAX_GLOBAL_MR;
    
    // CQ策略（默认与QP相同）
    init_default_rule(&config->cq_policy, RESOURCE_CQ);
    config->cq_policy.limit.max_per_process = DEFAULT_MAX_QP_PER_PROCESS;
    config->cq_policy.limit.max_global = DEFAULT_MAX_GLOBAL_QP;
    
    // PD策略
    init_default_rule(&config->pd_policy, RESOURCE_PD);
    config->pd_policy.limit.max_per_process = 100;
    config->pd_policy.limit.max_global = 1000;
    
    // 内存策略
    init_default_rule(&config->memory_policy, RESOURCE_MEMORY);
    config->memory_policy.limit.max_memory = DEFAULT_MAX_MEMORY_PER_PROCESS;
    
    // 动态调整参数
    config->auto_adjust = false;
    config->adjust_interval = DEFAULT_ADJUST_INTERVAL;
    config->high_watermark = 0.8f;
    config->low_watermark = 0.2f;
}

// 自动调整线程
static void *adjust_thread_func(void *arg) {
    (void)arg;
    
    while (g_running) {
        sleep(g_policy_config.adjust_interval);
        
        if (!g_running) break;
        
        if (g_policy_config.auto_adjust) {
            pthread_mutex_lock(&g_policy_mutex);
            dynamic_policy_adjust();
            pthread_mutex_unlock(&g_policy_mutex);
        }
    }
    
    return NULL;
}

// 初始化动态策略模块
int dynamic_policy_init(void) {
    fprintf(stderr, "[DYNAMIC_POLICY] 初始化动态策略模块\n");
    
    pthread_mutex_lock(&g_policy_mutex);
    
    // 设置默认配置
    set_default_config(&g_policy_config);
    
    // 初始化共享内存
    if (shm_init() != 0) {
        fprintf(stderr, "[DYNAMIC_POLICY] 初始化共享内存失败\n");
        pthread_mutex_unlock(&g_policy_mutex);
        return -1;
    }
    
    // 同步配置到共享内存
    shm_set_global_limits(
        g_policy_config.qp_policy.limit.max_global,
        g_policy_config.mr_policy.limit.max_global,
        g_policy_config.memory_policy.limit.max_memory
    );
    
    pthread_mutex_unlock(&g_policy_mutex);
    
    // 启动自动调整线程
    g_running = 1;
    if (pthread_create(&g_adjust_thread, NULL, adjust_thread_func, NULL) != 0) {
        fprintf(stderr, "[DYNAMIC_POLICY] 创建自动调整线程失败\n");
        return -1;
    }
    
    fprintf(stderr, "[DYNAMIC_POLICY] 动态策略模块初始化成功\n");
    return 0;
}

// 清理动态策略模块
void dynamic_policy_cleanup(void) {
    fprintf(stderr, "[DYNAMIC_POLICY] 清理动态策略模块\n");
    
    g_running = 0;
    pthread_join(g_adjust_thread, NULL);
    
    pthread_mutex_lock(&g_policy_mutex);
    memset(&g_policy_config, 0, sizeof(g_policy_config));
    pthread_mutex_unlock(&g_policy_mutex);
}

// 加载策略配置文件（简化格式）
int dynamic_policy_load_config(const char *config_path) {
    fprintf(stderr, "[DYNAMIC_POLICY] 加载策略配置: %s\n", config_path);
    
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        fprintf(stderr, "[DYNAMIC_POLICY] 无法打开配置文件: %s\n", config_path);
        return -1;
    }
    
    pthread_mutex_lock(&g_policy_mutex);
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        // 跳过注释和空行
        if (line[0] == '#' || line[0] == '\n') continue;
        
        char key[64];
        uint32_t val;
        if (sscanf(line, "%63s %u", key, &val) == 2) {
            if (strcmp(key, "max_qp_per_process") == 0) {
                g_policy_config.qp_policy.limit.max_per_process = val;
            } else if (strcmp(key, "max_global_qp") == 0) {
                g_policy_config.qp_policy.limit.max_global = val;
            } else if (strcmp(key, "max_mr_per_process") == 0) {
                g_policy_config.mr_policy.limit.max_per_process = val;
            } else if (strcmp(key, "max_global_mr") == 0) {
                g_policy_config.mr_policy.limit.max_global = val;
            } else if (strcmp(key, "max_memory_mb") == 0) {
                g_policy_config.memory_policy.limit.max_memory = (uint64_t)val * 1024 * 1024;
            } else if (strcmp(key, "auto_adjust") == 0) {
                g_policy_config.auto_adjust = val;
            } else if (strcmp(key, "adjust_interval") == 0) {
                g_policy_config.adjust_interval = val;
            }
        }
    }
    
    pthread_mutex_unlock(&g_policy_mutex);
    fclose(fp);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 策略配置加载成功\n");
    return 0;
}

// 保存策略配置到文件（简化格式）
int dynamic_policy_save_config(const char *config_path) {
    fprintf(stderr, "[DYNAMIC_POLICY] 保存策略配置: %s\n", config_path);
    
    pthread_mutex_lock(&g_policy_mutex);
    
    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        fprintf(stderr, "[DYNAMIC_POLICY] 无法写入配置文件: %s\n", config_path);
        pthread_mutex_unlock(&g_policy_mutex);
        return -1;
    }
    
    fprintf(fp, "# RDMA Dynamic Policy Configuration\n");
    fprintf(fp, "max_qp_per_process %u\n", g_policy_config.qp_policy.limit.max_per_process);
    fprintf(fp, "max_global_qp %u\n", g_policy_config.qp_policy.limit.max_global);
    fprintf(fp, "max_mr_per_process %u\n", g_policy_config.mr_policy.limit.max_per_process);
    fprintf(fp, "max_global_mr %u\n", g_policy_config.mr_policy.limit.max_global);
    fprintf(fp, "max_memory_mb %llu\n", 
            (unsigned long long)(g_policy_config.memory_policy.limit.max_memory / (1024*1024)));
    fprintf(fp, "auto_adjust %d\n", g_policy_config.auto_adjust ? 1 : 0);
    fprintf(fp, "adjust_interval %u\n", g_policy_config.adjust_interval);
    
    pthread_mutex_unlock(&g_policy_mutex);
    fclose(fp);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 策略配置保存成功\n");
    return 0;
}

// 设置资源限制
int dynamic_policy_set_limit(enum resource_type resource, const resource_limit_t *limit) {
    if (!limit) return -1;
    
    pthread_mutex_lock(&g_policy_mutex);
    
    switch (resource) {
        case RESOURCE_QP:
            memcpy(&g_policy_config.qp_policy.limit, limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_MR:
            memcpy(&g_policy_config.mr_policy.limit, limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_CQ:
            memcpy(&g_policy_config.cq_policy.limit, limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_PD:
            memcpy(&g_policy_config.pd_policy.limit, limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_MEMORY:
            memcpy(&g_policy_config.memory_policy.limit, limit, sizeof(resource_limit_t));
            break;
        default:
            pthread_mutex_unlock(&g_policy_mutex);
            return -1;
    }
    
    // 同步到共享内存
    shm_set_global_limits(
        g_policy_config.qp_policy.limit.max_global,
        g_policy_config.mr_policy.limit.max_global,
        g_policy_config.memory_policy.limit.max_memory
    );
    
    pthread_mutex_unlock(&g_policy_mutex);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 设置资源限制成功: resource=%d\n", resource);
    return 0;
}

// 获取资源限制
int dynamic_policy_get_limit(enum resource_type resource, resource_limit_t *limit) {
    if (!limit) return -1;
    
    pthread_mutex_lock(&g_policy_mutex);
    
    switch (resource) {
        case RESOURCE_QP:
            memcpy(limit, &g_policy_config.qp_policy.limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_MR:
            memcpy(limit, &g_policy_config.mr_policy.limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_CQ:
            memcpy(limit, &g_policy_config.cq_policy.limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_PD:
            memcpy(limit, &g_policy_config.pd_policy.limit, sizeof(resource_limit_t));
            break;
        case RESOURCE_MEMORY:
            memcpy(limit, &g_policy_config.memory_policy.limit, sizeof(resource_limit_t));
            break;
        default:
            pthread_mutex_unlock(&g_policy_mutex);
            return -1;
    }
    
    pthread_mutex_unlock(&g_policy_mutex);
    return 0;
}

// 检查资源是否超出限制
bool dynamic_policy_check_limit(enum resource_type resource, 
                                 uint32_t current_usage, 
                                 uint32_t requested_amount) {
    resource_limit_t limit;
    if (dynamic_policy_get_limit(resource, &limit) != 0) {
        return false; // 允许操作
    }
    
    // 获取全局资源使用情况
    resource_usage_t global_usage;
    shm_get_global_resources(&global_usage);
    
    // 检查全局限制
    uint32_t global_current = 0;
    switch (resource) {
        case RESOURCE_QP: global_current = global_usage.qp_count; break;
        case RESOURCE_MR: global_current = global_usage.mr_count; break;
        default: break;
    }
    
    if (global_current + requested_amount > limit.max_global) {
        fprintf(stderr, "[DYNAMIC_POLICY] 超出全局限制: current=%u, request=%u, limit=%u\n",
                global_current, requested_amount, limit.max_global);
        return true; // 超出限制
    }
    
    // 检查每进程限制
    if (current_usage + requested_amount > limit.max_per_process) {
        fprintf(stderr, "[DYNAMIC_POLICY] 超出每进程限制: current=%u, request=%u, limit=%u\n",
                current_usage, requested_amount, limit.max_per_process);
        return true; // 超出限制
    }
    
    return false; // 未超出限制
}

// 更新策略（热更新）
int dynamic_policy_update(const dynamic_policy_config_t *config) {
    if (!config) return -1;
    
    pthread_mutex_lock(&g_policy_mutex);
    memcpy(&g_policy_config, config, sizeof(dynamic_policy_config_t));
    
    // 同步到共享内存
    shm_set_global_limits(
        g_policy_config.qp_policy.limit.max_global,
        g_policy_config.mr_policy.limit.max_global,
        g_policy_config.memory_policy.limit.max_memory
    );
    
    pthread_mutex_unlock(&g_policy_mutex);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 策略热更新成功\n");
    return 0;
}

// 获取当前策略配置
int dynamic_policy_get_config(dynamic_policy_config_t *config) {
    if (!config) return -1;
    
    pthread_mutex_lock(&g_policy_mutex);
    memcpy(config, &g_policy_config, sizeof(dynamic_policy_config_t));
    pthread_mutex_unlock(&g_policy_mutex);
    
    return 0;
}

// 设置自动调整参数
int dynamic_policy_set_auto_adjust(bool enable, 
                                    uint32_t interval,
                                    float high_watermark, 
                                    float low_watermark) {
    pthread_mutex_lock(&g_policy_mutex);
    
    g_policy_config.auto_adjust = enable;
    if (interval > 0) {
        g_policy_config.adjust_interval = interval;
    }
    if (high_watermark > 0.0f && high_watermark <= 1.0f) {
        g_policy_config.high_watermark = high_watermark;
    }
    if (low_watermark >= 0.0f && low_watermark < 1.0f) {
        g_policy_config.low_watermark = low_watermark;
    }
    
    pthread_mutex_unlock(&g_policy_mutex);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 自动调整参数设置: enable=%d, interval=%u, high=%.2f, low=%.2f\n",
            enable, interval, high_watermark, low_watermark);
    return 0;
}

// 执行一次策略调整
int dynamic_policy_adjust(void) {
    pthread_mutex_lock(&g_policy_mutex);
    
    // 获取全局资源使用情况
    resource_usage_t global_usage;
    shm_get_global_resources(&global_usage);
    
    // 根据使用率动态调整策略
    if (g_policy_config.qp_policy.limit.max_global > 0) {
        float qp_usage_rate = (float)global_usage.qp_count / g_policy_config.qp_policy.limit.max_global;
        
        if (qp_usage_rate > g_policy_config.high_watermark) {
            fprintf(stderr, "[DYNAMIC_POLICY] QP使用率过高(%.2f%%)，考虑增加限制\n", 
                    qp_usage_rate * 100);
            // 可以在这里实现动态增加限制的逻辑
        } else if (qp_usage_rate < g_policy_config.low_watermark) {
            fprintf(stderr, "[DYNAMIC_POLICY] QP使用率较低(%.2f%%)，可以考虑降低限制\n", 
                    qp_usage_rate * 100);
        }
    }
    
    pthread_mutex_unlock(&g_policy_mutex);
    
    return 0;
}

// 打印当前策略配置
void dynamic_policy_print_config(void) {
    pthread_mutex_lock(&g_policy_mutex);
    
    fprintf(stderr, "\n========== 动态策略配置 ==========\n");
    fprintf(stderr, "QP策略: max_per_process=%u, max_global=%u, enabled=%d\n",
            g_policy_config.qp_policy.limit.max_per_process,
            g_policy_config.qp_policy.limit.max_global,
            g_policy_config.qp_policy.enabled);
    fprintf(stderr, "MR策略: max_per_process=%u, max_global=%u, enabled=%d\n",
            g_policy_config.mr_policy.limit.max_per_process,
            g_policy_config.mr_policy.limit.max_global,
            g_policy_config.mr_policy.enabled);
    fprintf(stderr, "内存策略: max_memory=%llu, enabled=%d\n",
            (unsigned long long)g_policy_config.memory_policy.limit.max_memory,
            g_policy_config.memory_policy.enabled);
    fprintf(stderr, "自动调整: enable=%d, interval=%u, high=%.2f, low=%.2f\n",
            g_policy_config.auto_adjust,
            g_policy_config.adjust_interval,
            g_policy_config.high_watermark,
            g_policy_config.low_watermark);
    fprintf(stderr, "==================================\n\n");
    
    pthread_mutex_unlock(&g_policy_mutex);
}

// 租户策略管理（简化实现，实际可使用共享内存或数据库存储）
static tenant_policy_t g_tenant_policies[MAX_TENANTS] = {0};
static pthread_mutex_t g_tenant_mutex = PTHREAD_MUTEX_INITIALIZER;

// 设置租户策略
int dynamic_policy_set_tenant_policy(uint32_t tenant_id, const tenant_policy_t *policy) {
    if (!policy || tenant_id >= MAX_TENANTS) return -1;
    
    pthread_mutex_lock(&g_tenant_mutex);
    memcpy(&g_tenant_policies[tenant_id], policy, sizeof(tenant_policy_t));
    g_tenant_policies[tenant_id].last_updated = time(NULL);
    pthread_mutex_unlock(&g_tenant_mutex);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 设置租户策略成功: tenant_id=%u\n", tenant_id);
    return 0;
}

// 获取租户策略
int dynamic_policy_get_tenant_policy(uint32_t tenant_id, tenant_policy_t *policy) {
    if (!policy || tenant_id >= MAX_TENANTS) return -1;
    
    pthread_mutex_lock(&g_tenant_mutex);
    memcpy(policy, &g_tenant_policies[tenant_id], sizeof(tenant_policy_t));
    pthread_mutex_unlock(&g_tenant_mutex);
    
    return 0;
}

// 删除租户策略
int dynamic_policy_delete_tenant_policy(uint32_t tenant_id) {
    if (tenant_id >= MAX_TENANTS) return -1;
    
    pthread_mutex_lock(&g_tenant_mutex);
    memset(&g_tenant_policies[tenant_id], 0, sizeof(tenant_policy_t));
    pthread_mutex_unlock(&g_tenant_mutex);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 删除租户策略成功: tenant_id=%u\n", tenant_id);
    return 0;
}

// 检查租户资源是否超出限制
bool dynamic_policy_check_tenant_limit(uint32_t tenant_id,
                                        enum resource_type resource,
                                        uint32_t current_usage,
                                        uint32_t requested_amount) {
    if (tenant_id >= MAX_TENANTS) return false;
    
    tenant_policy_t policy;
    if (dynamic_policy_get_tenant_policy(tenant_id, &policy) != 0) {
        fprintf(stderr, "[DYNAMIC_POLICY] 无法获取租户%d策略\n", tenant_id);
        return false;
    }
    
    resource_limit_t limit;
    const char *resource_name;
    switch (resource) {
        case RESOURCE_QP:
            limit = policy.policy.qp_policy.limit;
            resource_name = "QP";
            break;
        case RESOURCE_MR:
            limit = policy.policy.mr_policy.limit;
            resource_name = "MR";
            break;
        case RESOURCE_CQ:
            limit = policy.policy.cq_policy.limit;
            resource_name = "CQ";
            break;
        case RESOURCE_PD:
            limit = policy.policy.pd_policy.limit;
            resource_name = "PD";
            break;
        case RESOURCE_MEMORY:
            limit = policy.policy.memory_policy.limit;
            resource_name = "Memory";
            break;
        default:
            return false;
    }
    
    fprintf(stderr, "[DYNAMIC_POLICY] 租户%d %s检查: current=%u, request=%u, limit=%u\n", 
            tenant_id, resource_name, current_usage, requested_amount, limit.max_per_process);
    
    if (current_usage + requested_amount > limit.max_per_process) {
        fprintf(stderr, "[DYNAMIC_POLICY] 租户%d超出%s资源限制\n", tenant_id, resource_name);
        return true;
    }
    
    fprintf(stderr, "[DYNAMIC_POLICY] 租户%d %s检查通过\n", tenant_id, resource_name);
    return false;
}

// 获取所有租户策略列表
int dynamic_policy_get_all_tenant_policies(tenant_policy_t *policies, int max_count) {
    if (!policies || max_count <= 0) return -1;
    
    pthread_mutex_lock(&g_tenant_mutex);
    
    int count = 0;
    for (int i = 0; i < MAX_TENANTS && count < max_count; i++) {
        if (g_tenant_policies[i].tenant_id != 0) {
            memcpy(&policies[count], &g_tenant_policies[i], sizeof(tenant_policy_t));
            count++;
        }
    }
    
    pthread_mutex_unlock(&g_tenant_mutex);
    
    return count;
}

// 启用/禁用策略规则
int dynamic_policy_enable_rule(enum resource_type resource, bool enable) {
    pthread_mutex_lock(&g_policy_mutex);
    
    switch (resource) {
        case RESOURCE_QP:
            g_policy_config.qp_policy.enabled = enable;
            break;
        case RESOURCE_MR:
            g_policy_config.mr_policy.enabled = enable;
            break;
        case RESOURCE_CQ:
            g_policy_config.cq_policy.enabled = enable;
            break;
        case RESOURCE_PD:
            g_policy_config.pd_policy.enabled = enable;
            break;
        case RESOURCE_MEMORY:
            g_policy_config.memory_policy.enabled = enable;
            break;
        default:
            pthread_mutex_unlock(&g_policy_mutex);
            return -1;
    }
    
    pthread_mutex_unlock(&g_policy_mutex);
    
    fprintf(stderr, "[DYNAMIC_POLICY] %s资源策略规则\n", enable ? "启用" : "禁用");
    return 0;
}

// 设置默认策略
int dynamic_policy_set_default(void) {
    pthread_mutex_lock(&g_policy_mutex);
    set_default_config(&g_policy_config);
    pthread_mutex_unlock(&g_policy_mutex);
    
    fprintf(stderr, "[DYNAMIC_POLICY] 恢复默认策略配置\n");
    return 0;
}
