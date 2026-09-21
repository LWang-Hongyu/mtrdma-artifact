#define _GNU_SOURCE
/* NO_DEBUG: Disable debug output for performance testing */
#ifdef NO_DEBUG
  #define DEBUG_FPRINTF(...) ((void)0)
#else
  #define DEBUG_FPRINTF(...) fprintf(__VA_ARGS__)
#endif


/* 禁用调试日志 - EXP-1重新测试 */
#ifdef NO_DEBUG
  #define DISABLE_FPRINTF 1
#endif

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include "rdma_intercept.h"
#include "ebpf/ebpf_monitor_shm.h"
#include "shm/shared_memory.h"
#include "shm/shared_memory_tenant.h"

/* 函数指针类型定义 */
typedef struct ibv_qp *(*ibv_create_qp_fn)(struct ibv_pd *, struct ibv_qp_init_attr *);
typedef int (*ibv_destroy_qp_fn)(struct ibv_qp *);
typedef struct ibv_cq *(*ibv_create_cq_fn)(struct ibv_context *, int, void *, struct ibv_comp_channel *, int);
typedef int (*ibv_destroy_cq_fn)(struct ibv_cq *);
typedef struct ibv_pd *(*ibv_alloc_pd_fn)(struct ibv_context *);
typedef int (*ibv_dealloc_pd_fn)(struct ibv_pd *);
typedef int (*ibv_dereg_mr_fn)(struct ibv_mr *);
typedef struct ibv_mr *(*ibv_reg_mr_fn)(struct ibv_pd *, void *, size_t, int);
typedef struct ibv_qp *(*ibv_create_qp_ex_fn)(struct ibv_context *, struct ibv_qp_init_attr_ex *);
typedef struct ibv_context *(*ibv_open_device_fn)(struct ibv_device *);

/* 原始函数指针存储 */
static ibv_create_qp_fn real_ibv_create_qp = NULL;
static ibv_destroy_qp_fn real_ibv_destroy_qp = NULL;
static ibv_create_cq_fn real_ibv_create_cq = NULL;
static ibv_destroy_cq_fn real_ibv_destroy_cq = NULL;
static ibv_alloc_pd_fn real_ibv_alloc_pd = NULL;
static ibv_dealloc_pd_fn real_ibv_dealloc_pd = NULL;
static ibv_dereg_mr_fn real_ibv_dereg_mr = NULL;
static ibv_reg_mr_fn real_ibv_reg_mr = NULL;
static ibv_open_device_fn real_ibv_open_device = NULL;

/* 存储每个verbs context的原始create_qp_ex函数指针，用于拦截替换 */
#define MAX_VERBS_CONTEXTS 16
static struct {
    struct ibv_context *context;
    ibv_create_qp_ex_fn original_create_qp_ex;
} g_verb_ctx_table[MAX_VERBS_CONTEXTS];
static int g_verb_ctx_count = 0;
static pthread_mutex_t g_verb_ctx_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 静态初始化标志 */
static pthread_once_t hooks_init_once = PTHREAD_ONCE_INIT;

/* eBPF初始化状态 */
static int ebpf_initialized = 0;
static int tenant_initialized = 0;

/* 记录初始化时的PID，用于检测fork后的子进程 */
static pid_t cached_pid = 0;

/* MR操作速率限制状态 */
static pthread_mutex_t mr_rate_limit_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t mr_op_count_in_window = 0;
static struct timespec mr_rate_window_start = {0, 0};

/* 获取当前时间（毫秒） */
static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 检查并更新MR操作速率限制
 * 返回值：true - 允许操作，false - 超出速率限制
 */
static bool check_mr_rate_limit(void) {
    intercept_config_t config;
    rdma_intercept_get_config(&config);
    
    /* 如果未启用速率限制，直接允许 */
    if (!config.enable_mr_rate_limit || config.max_mr_ops_per_sec == 0) {
        return true;
    }
    
    pthread_mutex_lock(&mr_rate_limit_mutex);
    
    uint64_t current_time = get_time_ms();
    uint32_t window_ms = config.mr_rate_limit_window_ms > 0 ? 
                         config.mr_rate_limit_window_ms : 1000; // 默认1秒窗口
    
    /* 检查是否需要重置窗口 */
    if (mr_rate_window_start.tv_sec == 0 && mr_rate_window_start.tv_nsec == 0) {
        /* 首次初始化 */
        clock_gettime(CLOCK_MONOTONIC, &mr_rate_window_start);
        mr_op_count_in_window = 0;
    } else {
        uint64_t window_start_ms = (uint64_t)mr_rate_window_start.tv_sec * 1000 + 
                                   mr_rate_window_start.tv_nsec / 1000000;
        if (current_time - window_start_ms >= window_ms) {
            /* 窗口过期，重置 */
            clock_gettime(CLOCK_MONOTONIC, &mr_rate_window_start);
            mr_op_count_in_window = 0;
        }
    }
    
    /* 计算窗口内允许的最大操作数 */
    uint64_t max_ops_in_window = (uint64_t)config.max_mr_ops_per_sec * window_ms / 1000;
    if (max_ops_in_window < 1) max_ops_in_window = 1;
    
    /* 检查是否超出限制 */
    bool allowed = (mr_op_count_in_window < max_ops_in_window);
    
    if (allowed) {
        mr_op_count_in_window++;
    } else {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] MR rate limit exceeded: %lu ops in window, max=%lu\n",
                      mr_op_count_in_window, max_ops_in_window);
    }
    
    pthread_mutex_unlock(&mr_rate_limit_mutex);
    
    return allowed;
}

/* 通过共享内存获取进程资源使用情况 */
static int get_process_resources_via_shared_memory(int pid, resource_usage_t *usage)
{
    if (!usage) {
        return -1;
    }
    
    int result = shm_get_process_resources(pid, usage);
    if (result == 0) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] 从共享内存获取进程资源: PID=%d, QP=%d, MR=%d\n", 
                pid, usage->qp_count, usage->mr_count);
    } else {
        memset(usage, 0, sizeof(resource_usage_t));
    }
    
    return result;
}

/* 通过共享内存获取全局资源使用情况 */
static int get_global_resources_via_shared_memory(resource_usage_t *usage)
{
    if (!usage) {
        return -1;
    }
    
    int result = shm_get_global_resources(usage);
    if (result == 0) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] 从共享内存获取全局资源: QP=%d, MR=%d\n", 
                usage->qp_count, usage->mr_count);
    } else {
        memset(usage, 0, sizeof(resource_usage_t));
    }
    
    return result;
}

/* 获取进程的租户ID */
static uint32_t get_current_tenant_id(void) {
    /* E7: cgroup 成员身份派生优先 (防 RDMA_TENANT_ID 冒用)。
     * 进程位于 rdma:/tenant<N> 子组时, 租户身份【强制】从 cgroup 成员关系派生并重绑:
     * 即使环境变量被冒用 (设成受害者的 ID), 记账也归属攻击者自己的租户。
     * 部署面由 orchestrator 为每个租户建 tenant<N> 子组并放进程。 */
    if (1) {
        FILE *cgf = fopen("/proc/self/cgroup", "r");
        if (cgf) {
            char cgline[512];
            while (fgets(cgline, sizeof(cgline), cgf)) {
                char *seg = strstr(cgline, "rdma:/tenant");
                if (seg) {
                    uint32_t t = (uint32_t)strtoul(seg + strlen("rdma:/tenant"), NULL, 10);
                    if (t != 0 && tenant_shm_init() == 0 &&
                        tenant_bind_process(getpid(), t) == 0) {
                        tenant_initialized = 1;
                        break;
                    }
                }
            }
            fclose(cgf);
        }
    }

    if (!tenant_initialized) {
        return 0;
    }

    pid_t pid = getpid();

    /* 检测fork：如果PID变化但pthread_once没重新执行，
     * 尝试从RDMA_TENANT_ID环境变量绑定当前PID到租户 */
    if (pid != cached_pid) {
        const char *tenant_env = getenv("RDMA_TENANT_ID");
        if (tenant_env) {
            uint32_t tenant_id = atoi(tenant_env);
            if (tenant_id > 0) {
                if (tenant_bind_process(pid, tenant_id) == 0) {
                    DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Post-fork: process %d bound to tenant %u\n", pid, tenant_id);
                }
            }
        }
        cached_pid = pid;
    }

    uint32_t tenant_id = 0;
    if (tenant_get_process_tenant(pid, &tenant_id) != 0) {
        return 0;
    }

    return tenant_id;
}

/* 初始化函数指针 */
static void init_function_pointers(void) {
    init_if_needed();
    
    void *libibverbs = dlopen("libibverbs.so", RTLD_LAZY);
    if (!libibverbs) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Failed to open libibverbs.so: %s\n", dlerror());
        return;
    }
    
    dlerror();
    
    real_ibv_create_qp = (ibv_create_qp_fn)dlsym(libibverbs, "ibv_create_qp");
    real_ibv_destroy_qp = (ibv_destroy_qp_fn)dlsym(libibverbs, "ibv_destroy_qp");
    real_ibv_create_cq = (ibv_create_cq_fn)dlsym(libibverbs, "ibv_create_cq");
    real_ibv_destroy_cq = (ibv_destroy_cq_fn)dlsym(libibverbs, "ibv_destroy_cq");
    real_ibv_alloc_pd = (ibv_alloc_pd_fn)dlsym(libibverbs, "ibv_alloc_pd");
    real_ibv_dealloc_pd = (ibv_dealloc_pd_fn)dlsym(libibverbs, "ibv_dealloc_pd");
    real_ibv_dereg_mr = (ibv_dereg_mr_fn)dlsym(libibverbs, "ibv_dereg_mr");
    real_ibv_reg_mr = (ibv_reg_mr_fn)dlsym(libibverbs, "ibv_reg_mr");
    
    real_ibv_open_device = (ibv_open_device_fn)dlsym(libibverbs, "ibv_open_device");
    if (!real_ibv_open_device) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Warning: could not find ibv_open_device\n");
    }
    
    DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Function pointers initialized\n");
    
    /* 初始化eBPF监控 */
    int ebpf_err = ebpf_monitor_init();
    if (ebpf_err) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] eBPF monitor init warning: %d\n", ebpf_err);
        ebpf_initialized = 0;
    } else {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] eBPF monitor initialized\n");
        ebpf_initialized = 1;
    }
    
    /* 初始化租户共享内存 */
    if (tenant_shm_init() == 0) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Tenant shared memory initialized\n");
        tenant_initialized = 1;
    } else {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Tenant shared memory init warning\n");
        tenant_initialized = 0;
    }
    
    /* 绑定当前进程到租户（如果设置了环境变量） */
    const char *tenant_env = getenv("RDMA_TENANT_ID");
    if (tenant_env && tenant_initialized) {
        uint32_t tenant_id = atoi(tenant_env);
        if (tenant_id > 0) {
            pid_t pid = getpid();
            tenant_bind_process(pid, tenant_id);
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Process %d bound to tenant %u\n", pid, tenant_id);
        }
    }
    
    cached_pid = getpid();
}

/* 更新租户资源计数 */
static void update_tenant_resource_count(uint32_t tenant_id, int resource_type, int delta) {
    if (tenant_id == 0 || !tenant_initialized) {
        return;
    }
    
    tenant_resource_usage_t usage;
    if (tenant_get_resource_usage(tenant_id, &usage) != 0) {
        return;
    }
    
    switch (resource_type) {
        case 0: // QP
            usage.qp_count += delta;
            if (usage.qp_count < 0) usage.qp_count = 0;
            if (delta > 0) usage.total_qp_creates++;
            else if (delta < 0) usage.total_qp_destroys++;
            break;
        case 1: // MR
            usage.mr_count += delta;
            if (delta > 0) usage.total_mr_regs++;
            else if (delta < 0) usage.total_mr_deregs++;
            break;
        case 3: // CQ
            usage.cq_count += delta;
            break;
        case 4: // PD
            usage.pd_count += delta;
            break;
    }
    
    tenant_update_resource_usage(tenant_id, &usage);
}

/* 检查并原子分配QP（检查+递增在同一锁内完成） */
static bool check_tenant_qp_limit(uint32_t tenant_id) {
    /* E6 强制模式: fail-closed + fork 懒重绑。
     * - tenant_id==0 且无 RDMA_TENANT_ID: 拒绝 (fail-closed, 覆盖"机制内必受配额");
     * - tenant_id==0 但继承了 RDMA_TENANT_ID (fork 自设 env 的父进程):
     *   就地懒重绑 (shm init + bind) 后放行 —— 修复 fork 继承库状态导致绑定失效的问题。 */
    { static int mandatory = -1;
      if (mandatory == -1) mandatory = (getenv("RDMA_INTERCEPT_MANDATORY") != NULL) ? 1 : 0;
      if (mandatory && tenant_id == 0) {
        const char *env_tid = getenv("RDMA_TENANT_ID");
        if (env_tid && tenant_shm_init() == 0) {
          uint32_t e = (uint32_t)strtoul(env_tid, NULL, 10);
          if (e != 0 && tenant_bind_process(getpid(), e) == 0) {
            tenant_id = e;
            tenant_initialized = 1;
          }
        }
      }
      if (mandatory && (tenant_id == 0 || !tenant_initialized)) {
        fprintf(stderr, "[RDMA_HOOKS_TENANT] mandatory mode: no tenant binding, QP creation denied (pid=%d)\n", getpid());
        return false;
      }
    }

    if (tenant_id == 0 || !tenant_initialized) {
        return true; // 默认租户，不限制
    }

    return tenant_try_allocate_qp(tenant_id);
}

/* 检查MR创建是否符合租户资源限制 */
// check_tenant_mr_limit函数已内联到调用处
static inline bool check_tenant_mr_limit_inline(uint32_t tenant_id, size_t length) {
    DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] DEBUG MR: tenant_id=%u, tenant_initialized=%d\n",
            tenant_id, tenant_initialized);
    
    if (tenant_id == 0 || !tenant_initialized) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] DEBUG MR: allowing (default tenant or not initialized)\n");
        return true; // 默认租户，不限制
    }
    
    // 直接从租户共享内存获取配额和使用情况
    tenant_info_t info;
    if (tenant_get_info(tenant_id, &info) != 0) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] DEBUG MR: tenant_get_info failed\n");
        return false;
    }
    
    DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] DEBUG MR: tenant %u MR usage=%d/%d\n",
            tenant_id, info.usage.mr_count, info.quota.max_mr_per_tenant);
    
    // 检查MR数量限制
    if ((uint32_t)info.usage.mr_count >= info.quota.max_mr_per_tenant) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] 租户%d MR配额已用完 (%d/%d)\n", 
                tenant_id, info.usage.mr_count, info.quota.max_mr_per_tenant);
        return false;
    }
    
    // 检查内存限制 (如果配额为0则跳过)
    if (info.quota.max_memory_per_tenant > 0 && 
        info.usage.memory_used + length > info.quota.max_memory_per_tenant) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] 租户%d 内存配额不足 (%llu/%llu)\n", 
                tenant_id, (unsigned long long)info.usage.memory_used, 
                (unsigned long long)info.quota.max_memory_per_tenant);
        return false;
    }
    
    return true;
}

/* 检查QP创建是否符合资源限制（无副作用：不修改任何计数） */
static bool check_qp_creation_restrictions(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) {
    (void)pd;
    
    DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] DEBUG: enable_qp_control=%d, enable_intercept=%d\n", 
            g_intercept_state.config.enable_qp_control, g_intercept_state.config.enable_intercept);
    
    if (!g_intercept_state.config.enable_qp_control) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] DEBUG: QP control disabled, skipping checks\n");
        return true;
    }
    
    /* 检查QP类型限制 */
    switch (qp_init_attr->qp_type) {
        case IBV_QPT_RC:
            if (!g_intercept_state.config.allow_rc_qp) {
                DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] RC QP creation denied\n");
                return false;
            }
            break;
        case IBV_QPT_UC:
            if (!g_intercept_state.config.allow_uc_qp) {
                DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] UC QP creation denied\n");
                return false;
            }
            break;
        case IBV_QPT_UD:
            if (!g_intercept_state.config.allow_ud_qp) {
                DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] UD QP creation denied\n");
                return false;
            }
            break;
        default:
            break;
    }
    
    /* 获取进程资源使用情况 */
    resource_usage_t proc_usage;
    int pid = getpid();
    int collector_err = get_process_resources_via_shared_memory(pid, &proc_usage);
    
    if (collector_err == 0) {
        uint32_t effective_qp_count = (uint32_t)proc_usage.qp_count;
        if ((effective_qp_count + 1) > g_intercept_state.config.max_qp_per_process) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP creation denied: per-process limit\n");
            return false;
        }
    }
    
    /* 检查WR(WQE深度)限制 — 移植自 rdma_hooks.c 非租户路径, 补齐租户路径缺失的执行 */
    if (qp_init_attr->cap.max_send_wr > g_intercept_state.config.max_send_wr_limit) {
        fprintf(stderr, "[RDMA_HOOKS_TENANT] QP creation denied: send WR limit (%d) exceeded (requested %d)\n",
                g_intercept_state.config.max_send_wr_limit, qp_init_attr->cap.max_send_wr);
        return false;
    }
    if (qp_init_attr->cap.max_recv_wr > g_intercept_state.config.max_recv_wr_limit) {
        fprintf(stderr, "[RDMA_HOOKS_TENANT] QP creation denied: recv WR limit (%d) exceeded (requested %d)\n",
                g_intercept_state.config.max_recv_wr_limit, qp_init_attr->cap.max_recv_wr);
        return false;
    }

    /* 检查全局限制 */
    resource_usage_t global_usage;
    int global_err = get_global_resources_via_shared_memory(&global_usage);
    if (global_err == 0) {
        if ((uint32_t)global_usage.qp_count >= g_intercept_state.config.max_global_qp) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP creation denied: global limit\n");
            return false;
        }
    }
    
    return true;
}

/* 被拦截的ibv_create_qp函数 */
struct ibv_qp *ibv_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_create_qp) {
        if (real_ibv_create_qp) {
            return real_ibv_create_qp(pd, qp_init_attr);
        }
        errno = ENOSYS;
        return NULL;
    }

    /* 第一步：检查无副作用的限制（类型、进程级、全局），不修改任何计数 */
    if (!check_qp_creation_restrictions(pd, qp_init_attr)) {
        errno = EPERM;
        return NULL;
    }

    bool tenant_ok = true;
    uint32_t tenant_id = get_current_tenant_id();

    /* 第二步：原子检查租户QP配额并预留计数（这是唯一有副作用且会修改计数的操作）
     * 放在最后执行，确保前面所有检查通过后才预留配额，避免回滚 */
    if (tenant_id != 0 && tenant_initialized) {
        if (!check_tenant_qp_limit(tenant_id)) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP creation denied: tenant %u QP limit reached\n", tenant_id);
            errno = EPERM;
            return NULL;
        }
    } else {
        tenant_ok = false;
    }

    /* 第三阶段：在调用真实函数前，限制 WR 深度（WQE Cache 攻击防御第一阶段）
     * 将 max_recv_wr 和 max_send_wr 裁剪到配置上限，
     * 防止单个 QP 申请过大的 WQE 缓存挤占 NIC 片上空间
     * 注意：仅当限制值 > 0 时才进行裁剪，0 表示不限制 */
    if (g_intercept_state.config.enable_qp_control) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP attr before clip: max_send_wr=%u max_recv_wr=%u max_send_sge=%u max_recv_sge=%u\n",
               qp_init_attr->cap.max_send_wr,
               qp_init_attr->cap.max_recv_wr,
               qp_init_attr->cap.max_send_sge,
               qp_init_attr->cap.max_recv_sge);
        if (g_intercept_state.config.max_recv_wr_limit > 0 &&
            qp_init_attr->cap.max_recv_wr > g_intercept_state.config.max_recv_wr_limit) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Capping max_recv_wr: %u -> %u\n",
                   qp_init_attr->cap.max_recv_wr, g_intercept_state.config.max_recv_wr_limit);
            qp_init_attr->cap.max_recv_wr = g_intercept_state.config.max_recv_wr_limit;
        }
        if (g_intercept_state.config.max_send_wr_limit > 0 &&
            qp_init_attr->cap.max_send_wr > g_intercept_state.config.max_send_wr_limit) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Capping max_send_wr: %u -> %u\n",
                   qp_init_attr->cap.max_send_wr, g_intercept_state.config.max_send_wr_limit);
            qp_init_attr->cap.max_send_wr = g_intercept_state.config.max_send_wr_limit;
        }
    }

    struct ibv_qp *qp = real_ibv_create_qp(pd, qp_init_attr);
    
    if (!qp) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] real_ibv_create_qp FAILED! errno=%d (%s)\n",
               errno, strerror(errno));
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT]   qp_type=%d max_send_wr=%u max_recv_wr=%u max_send_sge=%u max_recv_sge=%u\n",
               qp_init_attr->qp_type,
               qp_init_attr->cap.max_send_wr,
               qp_init_attr->cap.max_recv_wr,
               qp_init_attr->cap.max_send_sge,
               qp_init_attr->cap.max_recv_sge);
    }
    
    if (qp) {
        pthread_mutex_lock(&g_intercept_state.resource_mutex);
        g_intercept_state.qp_count++;
        pthread_mutex_unlock(&g_intercept_state.resource_mutex);
        
        /* 更新共享内存 */
        resource_usage_t new_usage;
        int pid = getpid();
        new_usage.qp_count = g_intercept_state.qp_count;
        new_usage.mr_count = g_intercept_state.mr_count;
        new_usage.memory_used = g_intercept_state.memory_used;
        shm_update_process_resources(pid, &new_usage);
        
        /* 注意：租户QP计数已在check_tenant_qp_limit中原子递增，无需再次更新 */
        
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP created: %p\n", qp);
    } else if (tenant_ok) {
        /* real_ibv_create_qp 失败，回滚预分配的QP计数 */
        tenant_resource_usage_t usage;
        if (tenant_get_resource_usage(tenant_id, &usage) == 0) {
            if (usage.qp_count > 0) {
                usage.qp_count--;
            }
            if (usage.total_qp_creates > 0) {
                usage.total_qp_creates--;
            }
            tenant_update_resource_usage(tenant_id, &usage);
        }
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP creation failed, rolled back tenant %u count\n", tenant_id);
    }

    return qp;
}

/* 检查QP创建是否符合资源限制（扩展属性版本，用于ibv_create_qp_ex回调包装） */
static bool check_qp_creation_restrictions_extended(struct ibv_qp_init_attr_ex *qp_init_attr_ex, struct ibv_pd *pd) {
    (void)pd;

    if (!g_intercept_state.config.enable_qp_control) {
        return true;
    }

    switch (qp_init_attr_ex->qp_type) {
        case IBV_QPT_RC:
            if (!g_intercept_state.config.allow_rc_qp) {
                DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] RC QP creation denied (create_qp_ex callback)\n");
                return false;
            }
            break;
        case IBV_QPT_UC:
            if (!g_intercept_state.config.allow_uc_qp) {
                DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] UC QP creation denied (create_qp_ex callback)\n");
                return false;
            }
            break;
        case IBV_QPT_UD:
            if (!g_intercept_state.config.allow_ud_qp) {
                DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] UD QP creation denied (create_qp_ex callback)\n");
                return false;
            }
            break;
        default:
            break;
    }

    resource_usage_t proc_usage;
    int pid = getpid();
    int collector_err = get_process_resources_via_shared_memory(pid, &proc_usage);

    if (collector_err == 0) {
        uint32_t effective_qp_count = (uint32_t)proc_usage.qp_count;
        if ((effective_qp_count + 1) > g_intercept_state.config.max_qp_per_process) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP creation denied (create_qp_ex callback): per-process limit\n");
            return false;
        }
    }

    resource_usage_t global_usage;
    int global_err = get_global_resources_via_shared_memory(&global_usage);
    if (global_err == 0) {
        if ((uint32_t)global_usage.qp_count >= g_intercept_state.config.max_global_qp) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP creation denied (create_qp_ex callback): global limit\n");
            return false;
        }
    }

    return true;
}

/* 在verbs context表中查找对应的原始create_qp_ex回调 */
static ibv_create_qp_ex_fn find_original_create_qp_ex(struct ibv_context *context) {
    pthread_mutex_lock(&g_verb_ctx_mutex);
    for (int i = 0; i < g_verb_ctx_count; i++) {
        if (g_verb_ctx_table[i].context == context) {
            ibv_create_qp_ex_fn original = g_verb_ctx_table[i].original_create_qp_ex;
            pthread_mutex_unlock(&g_verb_ctx_mutex);
            return original;
        }
    }
    pthread_mutex_unlock(&g_verb_ctx_mutex);
    return NULL;
}

/* 替换create_qp_ex回调的包装函数 */
static struct ibv_qp *create_qp_ex_wrapper(struct ibv_context *context,
                                            struct ibv_qp_init_attr_ex *qp_init_attr_ex) {
    ibv_create_qp_ex_fn original = find_original_create_qp_ex(context);
    if (!original) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] create_qp_ex_wrapper: original callback not found!\n");
        errno = ENOSYS;
        return NULL;
    }

    if (!rdma_intercept_is_enabled()) {
        return original(context, qp_init_attr_ex);
    }

    struct ibv_pd *pd = qp_init_attr_ex ? qp_init_attr_ex->pd : NULL;

    if (!check_qp_creation_restrictions_extended(qp_init_attr_ex, pd)) {
        errno = EPERM;
        return NULL;
    }

    bool tenant_ok = true;
    uint32_t tenant_id = get_current_tenant_id();

    if (tenant_id != 0 && tenant_initialized) {
        if (!check_tenant_qp_limit(tenant_id)) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP creation denied (create_qp_ex callback): tenant %u QP limit reached\n", tenant_id);
            errno = EPERM;
            return NULL;
        }
    } else {
        tenant_ok = false;
    }

    if (g_intercept_state.config.enable_qp_control) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP callback attr before clip: max_send_wr=%u max_recv_wr=%u max_send_sge=%u max_recv_sge=%u\n",
               qp_init_attr_ex->cap.max_send_wr,
               qp_init_attr_ex->cap.max_recv_wr,
               qp_init_attr_ex->cap.max_send_sge,
               qp_init_attr_ex->cap.max_recv_sge);
        if (g_intercept_state.config.max_recv_wr_limit > 0 &&
            qp_init_attr_ex->cap.max_recv_wr > g_intercept_state.config.max_recv_wr_limit) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Capping max_recv_wr (callback): %u -> %u\n",
                   qp_init_attr_ex->cap.max_recv_wr, g_intercept_state.config.max_recv_wr_limit);
            qp_init_attr_ex->cap.max_recv_wr = g_intercept_state.config.max_recv_wr_limit;
        }
        if (g_intercept_state.config.max_send_wr_limit > 0 &&
            qp_init_attr_ex->cap.max_send_wr > g_intercept_state.config.max_send_wr_limit) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Capping max_send_wr (callback): %u -> %u\n",
                   qp_init_attr_ex->cap.max_send_wr, g_intercept_state.config.max_send_wr_limit);
            qp_init_attr_ex->cap.max_send_wr = g_intercept_state.config.max_send_wr_limit;
        }
    }

    struct ibv_qp *qp = original(context, qp_init_attr_ex);

    if (!qp) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] original create_qp_ex callback FAILED! errno=%d (%s)\n",
               errno, strerror(errno));
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT]   qp_type=%d max_send_wr=%u max_recv_wr=%u max_send_sge=%u max_recv_sge=%u\n",
               qp_init_attr_ex->qp_type,
               qp_init_attr_ex->cap.max_send_wr,
               qp_init_attr_ex->cap.max_recv_wr,
               qp_init_attr_ex->cap.max_send_sge,
               qp_init_attr_ex->cap.max_recv_sge);
    }

    if (qp) {
        pthread_mutex_lock(&g_intercept_state.resource_mutex);
        g_intercept_state.qp_count++;
        pthread_mutex_unlock(&g_intercept_state.resource_mutex);

        resource_usage_t new_usage;
        int pid = getpid();
        new_usage.qp_count = g_intercept_state.qp_count;
        new_usage.mr_count = g_intercept_state.mr_count;
        new_usage.memory_used = g_intercept_state.memory_used;
        shm_update_process_resources(pid, &new_usage);

        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP (callback) created: %p, total=%d\n", qp, g_intercept_state.qp_count);
    } else if (tenant_ok) {
        tenant_resource_usage_t usage;
        if (tenant_get_resource_usage(tenant_id, &usage) == 0) {
            if (usage.qp_count > 0) {
                usage.qp_count--;
            }
            if (usage.total_qp_creates > 0) {
                usage.total_qp_creates--;
            }
            tenant_update_resource_usage(tenant_id, &usage);
        }
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP (callback) creation failed, rolled back tenant %u count\n", tenant_id);
    }

    return qp;
}

/* 被拦截的ibv_open_device函数 - 在打开设备后替换create_qp_ex回调 */
struct ibv_context *ibv_open_device(struct ibv_device *device) {
    pthread_once(&hooks_init_once, init_function_pointers);

    if (!rdma_intercept_is_enabled() || !real_ibv_open_device) {
        if (real_ibv_open_device) {
            return real_ibv_open_device(device);
        }
        errno = ENOSYS;
        return NULL;
    }

    struct ibv_context *ctx = real_ibv_open_device(device);

    if (ctx) {
        struct verbs_context *vctx = verbs_get_ctx(ctx);
        if (vctx && vctx->create_qp_ex && vctx->create_qp_ex != create_qp_ex_wrapper) {
            pthread_mutex_lock(&g_verb_ctx_mutex);
            if (g_verb_ctx_count < MAX_VERBS_CONTEXTS) {
                g_verb_ctx_table[g_verb_ctx_count].context = ctx;
                g_verb_ctx_table[g_verb_ctx_count].original_create_qp_ex = vctx->create_qp_ex;
                g_verb_ctx_count++;
                DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Replacing create_qp_ex callback for context %p (table index %d)\n",
                       (void*)ctx, g_verb_ctx_count - 1);
            }
            vctx->create_qp_ex = create_qp_ex_wrapper;
            pthread_mutex_unlock(&g_verb_ctx_mutex);
        } else if (!vctx) {
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] Warning: verbs_get_ctx returned NULL (abi_compat mismatch?)\n");
            DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT]   ctx->abi_compat = %p, __VERBS_ABI_IS_EXTENDED = %p\n",
                   (void*)ctx->abi_compat, (void*)__VERBS_ABI_IS_EXTENDED);
        }
    }

    return ctx;
}

/* 被拦截的ibv_destroy_qp函数 */
int ibv_destroy_qp(struct ibv_qp *qp) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_destroy_qp) {
        if (real_ibv_destroy_qp) {
            return real_ibv_destroy_qp(qp);
        }
        errno = ENOSYS;
        return -1;
    }

    int result = real_ibv_destroy_qp(qp);
    
    if (result == 0) {
        pthread_mutex_lock(&g_intercept_state.resource_mutex);
        if (g_intercept_state.qp_count > 0) {
            g_intercept_state.qp_count--;
        }
        pthread_mutex_unlock(&g_intercept_state.resource_mutex);
        
        /* 更新租户资源 */
        update_tenant_resource_count(get_current_tenant_id(), 0, -1); // 0=QP
        
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] QP destroyed: %p\n", qp);
    }

    return result;
}

/* 被拦截的ibv_reg_mr函数 - 使用不同名称避免宏冲突 */
struct ibv_mr *__real_ibv_reg_mr_tenant(struct ibv_pd *pd, void *addr, size_t length, int access) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_reg_mr) {
        if (real_ibv_reg_mr) {
            return real_ibv_reg_mr(pd, addr, length, access);
        }
        errno = ENOSYS;
        return NULL;
    }

    uint32_t tenant_id = get_current_tenant_id();
    
    /* 检查租户MR限制 */
    if (!check_tenant_mr_limit_inline(tenant_id, length)) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] MR registration denied: tenant %u MR limit exceeded\n", tenant_id);
        errno = EPERM;
        return NULL;
    }
    
    /* 检查MR操作速率限制 */
    if (!check_mr_rate_limit()) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] MR registration denied: tenant %u rate limit exceeded\n", tenant_id);
        errno = EAGAIN;  /* 使用EAGAIN表示速率限制 */
        return NULL;
    }

    struct ibv_mr *mr = real_ibv_reg_mr(pd, addr, length, access);
    
    if (mr) {
        pthread_mutex_lock(&g_intercept_state.resource_mutex);
        g_intercept_state.mr_count++;
        g_intercept_state.memory_used += length;
        pthread_mutex_unlock(&g_intercept_state.resource_mutex);
        
        /* 更新租户资源 */
        update_tenant_resource_count(tenant_id, 1, 1); // 1=MR
        
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] MR registered: %p, length=%zu\n", mr, length);
    }

    return mr;
}

/* 被拦截的ibv_dereg_mr函数 - 使用不同名称避免宏冲突 */
int __real_ibv_dereg_mr_tenant(struct ibv_mr *mr) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_dereg_mr) {
        if (real_ibv_dereg_mr) {
            return real_ibv_dereg_mr(mr);
        }
        errno = ENOSYS;
        return -1;
    }
    
    /* 检查MR操作速率限制 */
    if (!check_mr_rate_limit()) {
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] MR deregistration denied: rate limit exceeded\n");
        errno = EAGAIN;  /* 使用EAGAIN表示速率限制 */
        return -1;
    }

    size_t mr_length = mr ? mr->length : 0;
    int result = real_ibv_dereg_mr(mr);
    
    if (result == 0) {
        pthread_mutex_lock(&g_intercept_state.resource_mutex);
        if (g_intercept_state.mr_count > 0) {
            g_intercept_state.mr_count--;
        }
        if (g_intercept_state.memory_used >= mr_length) {
            g_intercept_state.memory_used -= mr_length;
        }
        pthread_mutex_unlock(&g_intercept_state.resource_mutex);
        
        /* 更新租户资源 */
        update_tenant_resource_count(get_current_tenant_id(), 1, -1); // 1=MR
        
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] MR deregistered: %p\n", mr);
    }

    return result;
}

/* 被拦截的ibv_create_cq函数 */
struct ibv_cq *ibv_create_cq(struct ibv_context *context, int cqe, void *cq_context,
                            struct ibv_comp_channel *channel, int comp_vector) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_create_cq) {
        if (real_ibv_create_cq) {
            return real_ibv_create_cq(context, cqe, cq_context, channel, comp_vector);
        }
        errno = ENOSYS;
        return NULL;
    }

    struct ibv_cq *cq = real_ibv_create_cq(context, cqe, cq_context, channel, comp_vector);
    
    if (cq) {
        /* 更新租户资源 */
        update_tenant_resource_count(get_current_tenant_id(), 3, 1); // 3=CQ
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] CQ created: %p\n", cq);
    }

    return cq;
}

/* 被拦截的ibv_destroy_cq函数 */
int ibv_destroy_cq(struct ibv_cq *cq) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_destroy_cq) {
        if (real_ibv_destroy_cq) {
            return real_ibv_destroy_cq(cq);
        }
        errno = ENOSYS;
        return -1;
    }

    int result = real_ibv_destroy_cq(cq);
    
    if (result == 0) {
        /* 更新租户资源 */
        update_tenant_resource_count(get_current_tenant_id(), 3, -1); // 3=CQ
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] CQ destroyed: %p\n", cq);
    }

    return result;
}

/* 被拦截的ibv_alloc_pd函数 */
struct ibv_pd *ibv_alloc_pd(struct ibv_context *context) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_alloc_pd) {
        if (real_ibv_alloc_pd) {
            return real_ibv_alloc_pd(context);
        }
        errno = ENOSYS;
        return NULL;
    }

    struct ibv_pd *pd = real_ibv_alloc_pd(context);
    
    if (pd) {
        /* 更新租户资源 */
        update_tenant_resource_count(get_current_tenant_id(), 4, 1); // 4=PD
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] PD allocated: %p\n", pd);
    }

    return pd;
}

/* 被拦截的ibv_dealloc_pd函数 */
int ibv_dealloc_pd(struct ibv_pd *pd) {
    pthread_once(&hooks_init_once, init_function_pointers);
    
    if (!rdma_intercept_is_enabled() || !real_ibv_dealloc_pd) {
        if (real_ibv_dealloc_pd) {
            return real_ibv_dealloc_pd(pd);
        }
        errno = ENOSYS;
        return -1;
    }

    int result = real_ibv_dealloc_pd(pd);
    
    if (result == 0) {
        /* 更新租户资源 */
        update_tenant_resource_count(get_current_tenant_id(), 4, -1); // 4=PD
        DEBUG_FPRINTF(stderr, "[RDMA_HOOKS_TENANT] PD deallocated: %p\n", pd);
    }

    return result;
}

/* LD_PRELOAD使用的实际拦截函数 - 包装租户检查函数
 * 注意：需要在包含verbs.h之前#undef ibv_reg_mr，因为它是一个宏
 */
#ifdef ibv_reg_mr
#undef ibv_reg_mr
#endif
struct ibv_mr *ibv_reg_mr(struct ibv_pd *pd, void *addr, size_t length, int access) {
    return __real_ibv_reg_mr_tenant(pd, addr, length, access);
}

#ifdef ibv_dereg_mr
#undef ibv_dereg_mr
#endif
int ibv_dereg_mr(struct ibv_mr *mr) {
    return __real_ibv_dereg_mr_tenant(mr);
}
