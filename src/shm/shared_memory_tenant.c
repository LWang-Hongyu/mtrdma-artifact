#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "shared_memory_tenant.h"

// 共享内存大小
#define TENANT_SHM_SIZE (sizeof(tenant_shared_memory_t))

// 全局共享内存指针
static tenant_shared_memory_t *g_tenant_shm = NULL;
static int g_tenant_shm_fd = -1;

// 自旋锁实现
static void spin_lock(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            __sync_synchronize();
        }
    }
}

static void spin_unlock(volatile int *lock) {
    __sync_lock_release(lock);
}

// 初始化租户共享内存
int tenant_shm_init(void) {
    if (g_tenant_shm != NULL) {
        return 0; // 已经初始化
    }
    
    // 创建或打开共享内存对象
    g_tenant_shm_fd = shm_open(TENANT_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_tenant_shm_fd < 0) {
        perror("[TENANT_SHM] shm_open failed");
        return -1;
    }
    
    // 设置共享内存大小
    if (ftruncate(g_tenant_shm_fd, TENANT_SHM_SIZE) < 0) {
        perror("[TENANT_SHM] ftruncate failed");
        close(g_tenant_shm_fd);
        g_tenant_shm_fd = -1;
        return -1;
    }
    
    // 映射共享内存
    g_tenant_shm = mmap(NULL, TENANT_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_tenant_shm_fd, 0);
    if (g_tenant_shm == MAP_FAILED) {
        perror("[TENANT_SHM] mmap failed");
        close(g_tenant_shm_fd);
        g_tenant_shm_fd = -1;
        g_tenant_shm = NULL;
        return -1;
    }
    
    // 初始化共享内存（如果是新创建的）
    struct stat shm_stat;
    if (fstat(g_tenant_shm_fd, &shm_stat) == 0 && shm_stat.st_size == TENANT_SHM_SIZE) {
        // 检查是否已经初始化（通过检查version字段）
        if (g_tenant_shm->version == 0) {
            memset(g_tenant_shm, 0, TENANT_SHM_SIZE);
            g_tenant_shm->version = 1;
            g_tenant_shm->last_update_time = time(NULL);
            fprintf(stderr, "[TENANT_SHM] 初始化新的租户共享内存\n");
        } else {
            fprintf(stderr, "[TENANT_SHM] 连接到现有的租户共享内存 (version=%lu)\n", 
                    g_tenant_shm->version);
        }
    }
    
    fprintf(stderr, "[TENANT_SHM] 租户共享内存初始化成功\n");
    return 0;
}

// 销毁租户共享内存
int tenant_shm_destroy(void) {
    if (g_tenant_shm == NULL) {
        return 0;
    }
    
    munmap(g_tenant_shm, TENANT_SHM_SIZE);
    g_tenant_shm = NULL;
    
    if (g_tenant_shm_fd >= 0) {
        close(g_tenant_shm_fd);
        g_tenant_shm_fd = -1;
    }
    
    shm_unlink(TENANT_SHM_NAME);
    
    fprintf(stderr, "[TENANT_SHM] 租户共享内存已销毁\n");
    return 0;
}

// 获取租户共享内存指针
tenant_shared_memory_t* tenant_shm_get_ptr(void) {
    if (g_tenant_shm == NULL) {
        tenant_shm_init();
    }
    return g_tenant_shm;
}

// 加锁
void tenant_shm_lock(tenant_shared_memory_t* data) {
    if (data) {
        spin_lock(&data->tenant_shm_lock);
    }
}

// 解锁
void tenant_shm_unlock(tenant_shared_memory_t* data) {
    if (data) {
        spin_unlock(&data->tenant_shm_lock);
        data->version++;
        data->last_update_time = time(NULL);
    }
}

// 创建租户
int tenant_create(uint32_t tenant_id, const char *name, const tenant_quota_t *quota) {
    if (tenant_id == 0 || tenant_id >= MAX_TENANTS) {
        fprintf(stderr, "[TENANT] 无效的租户ID: %u\n", tenant_id);
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    // 检查租户是否已存在
    if (tenant->status != TENANT_STATUS_INACTIVE) {
        fprintf(stderr, "[TENANT] 租户%u已存在\n", tenant_id);
        tenant_shm_unlock(shm);
        return -1;
    }
    
    // 初始化租户信息
    memset(tenant, 0, sizeof(tenant_info_t));
    tenant->tenant_id = tenant_id;
    strncpy(tenant->tenant_name, name ? name : "unnamed", TENANT_NAME_MAX - 1);
    tenant->tenant_name[TENANT_NAME_MAX - 1] = '\0';
    tenant->status = TENANT_STATUS_ACTIVE;
    tenant->created_at = time(NULL);
    tenant->last_active_at = tenant->created_at;
    
    // 设置配额
    if (quota) {
        memcpy(&tenant->quota, quota, sizeof(tenant_quota_t));
    } else {
        // 使用默认配额
        tenant->quota.max_qp_per_tenant = 100;
        tenant->quota.max_mr_per_tenant = 1000;
        tenant->quota.max_memory_per_tenant = 1024ULL * 1024 * 1024; // 1GB
        tenant->quota.max_cq_per_tenant = 100;
        tenant->quota.max_pd_per_tenant = 100;
    }
    
    shm->active_tenant_count++;
    
    tenant_shm_unlock(shm);
    
    fprintf(stderr, "[TENANT] 租户%u(%s)创建成功\n", tenant_id, tenant->tenant_name);
    return 0;
}

// 删除租户
int tenant_delete(uint32_t tenant_id) {
    if (tenant_id == 0 || tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    if (tenant->status == TENANT_STATUS_INACTIVE) {
        fprintf(stderr, "[TENANT] 租户%u不存在\n", tenant_id);
        tenant_shm_unlock(shm);
        return -1;
    }
    
    // 清理该租户绑定的所有进程
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (shm->pid_mappings[i].tenant_id == tenant_id) {
            shm->pid_mappings[i].pid = 0;
            shm->pid_mappings[i].tenant_id = 0;
            shm->total_process_count--;
        }
    }
    
    // 标记租户为未激活
    tenant->status = TENANT_STATUS_INACTIVE;
    tenant->process_count = 0;
    
    shm->active_tenant_count--;
    
    tenant_shm_unlock(shm);
    
    fprintf(stderr, "[TENANT] 租户%u已删除\n", tenant_id);
    return 0;
}

// 获取租户信息
int tenant_get_info(uint32_t tenant_id, tenant_info_t *info) {
    if (!info || tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    memcpy(info, tenant, sizeof(tenant_info_t));
    
    tenant_shm_unlock(shm);
    
    return (tenant->status != TENANT_STATUS_INACTIVE) ? 0 : -1;
}

// 设置租户状态
int tenant_set_status(uint32_t tenant_id, enum tenant_status status) {
    if (tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    if (tenant->status == TENANT_STATUS_INACTIVE) {
        tenant_shm_unlock(shm);
        return -1;
    }
    
    tenant->status = status;
    tenant->last_active_at = time(NULL);
    
    tenant_shm_unlock(shm);
    
    fprintf(stderr, "[TENANT] 租户%u状态更新为%d\n", tenant_id, status);
    return 0;
}

// 更新租户配额
int tenant_update_quota(uint32_t tenant_id, const tenant_quota_t *quota) {
    if (!quota || tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    if (tenant->status == TENANT_STATUS_INACTIVE) {
        tenant_shm_unlock(shm);
        return -1;
    }
    
    memcpy(&tenant->quota, quota, sizeof(tenant_quota_t));
    tenant->last_active_at = time(NULL);
    
    tenant_shm_unlock(shm);
    
    fprintf(stderr, "[TENANT] 租户%u配额已更新\n", tenant_id);
    return 0;
}

// 将进程绑定到租户
int tenant_bind_process(pid_t pid, uint32_t tenant_id) {
    if (tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    // 检查租户是否有效
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    if (tenant->status != TENANT_STATUS_ACTIVE) {
        fprintf(stderr, "[TENANT] 租户%u未激活\n", tenant_id);
        tenant_shm_unlock(shm);
        return -1;
    }
    
    // 查找或创建PID映射
    int found = -1;
    int empty_slot = -1;
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (shm->pid_mappings[i].pid == pid) {
            found = i;
            break;
        }
        if (empty_slot < 0 && shm->pid_mappings[i].pid == 0) {
            empty_slot = i;
        }
    }
    
    int idx = (found >= 0) ? found : empty_slot;
    
    if (idx < 0) {
        fprintf(stderr, "[TENANT] PID映射表已满\n");
        tenant_shm_unlock(shm);
        return -1;
    }
    
    // 如果是新绑定，增加租户进程计数
    if (found < 0) {
        tenant->process_count++;
        shm->total_process_count++;
    }
    
    // 更新映射
    shm->pid_mappings[idx].pid = pid;
    shm->pid_mappings[idx].tenant_id = tenant_id;
    shm->pid_mappings[idx].mapped_at = time(NULL);
    
    // 添加到租户进程列表
    if (found < 0) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (tenant->processes[i] == 0) {
                tenant->processes[i] = pid;
                break;
            }
        }
    }
    
    tenant->last_active_at = time(NULL);
    
    tenant_shm_unlock(shm);
    
    fprintf(stderr, "[TENANT] 进程%d绑定到租户%u\n", pid, tenant_id);
    return 0;
}

// 解除进程与租户的绑定
int tenant_unbind_process(pid_t pid) {
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    // 查找PID映射
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (shm->pid_mappings[i].pid == pid) {
            uint32_t tenant_id = shm->pid_mappings[i].tenant_id;
            
            // 从租户进程列表中移除
            tenant_info_t *tenant = &shm->tenants[tenant_id];
            for (int j = 0; j < MAX_PROCESSES; j++) {
                if (tenant->processes[j] == pid) {
                    tenant->processes[j] = 0;
                    break;
                }
            }
            
            if (tenant->process_count > 0) {
                tenant->process_count--;
            }
            
            // 清除映射
            shm->pid_mappings[i].pid = 0;
            shm->pid_mappings[i].tenant_id = 0;
            
            if (shm->total_process_count > 0) {
                shm->total_process_count--;
            }
            
            tenant_shm_unlock(shm);
            
            fprintf(stderr, "[TENANT] 进程%d从租户%u解绑\n", pid, tenant_id);
            return 0;
        }
    }
    
    tenant_shm_unlock(shm);
    
    fprintf(stderr, "[TENANT] 进程%d未绑定到任何租户\n", pid);
    return -1;
}

// 获取进程所属的租户ID
int tenant_get_process_tenant(pid_t pid, uint32_t *tenant_id) {
    if (!tenant_id) {
        return -1;
    }
    
    *tenant_id = 0; // 默认租户
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (shm->pid_mappings[i].pid == pid) {
            *tenant_id = shm->pid_mappings[i].tenant_id;
            tenant_shm_unlock(shm);
            return 0;
        }
    }
    
    tenant_shm_unlock(shm);
    return -1; // 未找到
}

// 更新租户资源使用
int tenant_update_resource_usage(uint32_t tenant_id, const tenant_resource_usage_t *usage) {
    if (!usage || tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    if (tenant->status != TENANT_STATUS_ACTIVE) {
        tenant_shm_unlock(shm);
        return -1;
    }
    
    memcpy(&tenant->usage, usage, sizeof(tenant_resource_usage_t));
    tenant->last_active_at = time(NULL);
    
    tenant_shm_unlock(shm);
    
    return 0;
}

// 获取租户资源使用
int tenant_get_resource_usage(uint32_t tenant_id, tenant_resource_usage_t *usage) {
    if (!usage || tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    if (tenant->status != TENANT_STATUS_ACTIVE) {
        tenant_shm_unlock(shm);
        return -1;
    }
    
    memcpy(usage, &tenant->usage, sizeof(tenant_resource_usage_t));
    
    tenant_shm_unlock(shm);
    
    return 0;
}

// 检查租户资源限制
bool tenant_check_resource_limit(uint32_t tenant_id, int resource_type, uint32_t requested_amount) {
    if (tenant_id >= MAX_TENANTS) {
        return false;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return false;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    if (tenant->status != TENANT_STATUS_ACTIVE) {
        tenant_shm_unlock(shm);
        return true; // 非活跃租户，拒绝请求
    }
    
    bool exceeded = false;
    
    switch (resource_type) {
        case 0: // QP
            if (tenant->usage.qp_count + requested_amount > tenant->quota.max_qp_per_tenant) {
                exceeded = true;
            }
            break;
        case 1: // MR
            if (tenant->usage.mr_count + requested_amount > tenant->quota.max_mr_per_tenant) {
                exceeded = true;
            }
            break;
        case 2: // Memory
            if (tenant->usage.memory_used + requested_amount > tenant->quota.max_memory_per_tenant) {
                exceeded = true;
            }
            break;
        case 3: // CQ
            if (tenant->usage.cq_count + requested_amount > tenant->quota.max_cq_per_tenant) {
                exceeded = true;
            }
            break;
        case 4: // PD
            if (tenant->usage.pd_count + requested_amount > tenant->quota.max_pd_per_tenant) {
                exceeded = true;
            }
            break;
    }
    
    tenant_shm_unlock(shm);
    
    if (exceeded) {
        fprintf(stderr, "[TENANT] 租户%u超出资源限制(type=%d, request=%u)\n", 
                tenant_id, resource_type, requested_amount);
    }
    
    return exceeded;
}

bool tenant_try_allocate_qp(uint32_t tenant_id) {
    if (tenant_id >= MAX_TENANTS) {
        return false;
    }

    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return false;
    }

    tenant_shm_lock(shm);

    tenant_info_t *tenant = &shm->tenants[tenant_id];
    if (tenant->status != TENANT_STATUS_ACTIVE) {
        tenant_shm_unlock(shm);
        return false;
    }

    if (tenant->usage.qp_count < 0) {
        tenant->usage.qp_count = 0;
    }

    if ((uint32_t)tenant->usage.qp_count >= tenant->quota.max_qp_per_tenant) {
        tenant_shm_unlock(shm);
        return false;
    }

    tenant->usage.qp_count++;
    tenant->usage.total_qp_creates++;
    tenant->last_active_at = time(NULL);

    tenant_shm_unlock(shm);

    return true;
}

// 获取所有活跃租户列表
int tenant_get_active_list(tenant_info_t *tenants, int max_count) {
    if (!tenants || max_count <= 0) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    int count = 0;
    for (int i = 0; i < MAX_TENANTS && count < max_count; i++) {
        if (shm->tenants[i].status == TENANT_STATUS_ACTIVE) {
            memcpy(&tenants[count], &shm->tenants[i], sizeof(tenant_info_t));
            count++;
        }
    }
    
    tenant_shm_unlock(shm);
    
    return count;
}

// 打印所有租户信息
void tenant_print_all(void) {
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return;
    }
    
    tenant_shm_lock(shm);
    
    fprintf(stderr, "\n========== 租户列表 ==========\n");
    fprintf(stderr, "活跃租户数: %u, 总进程数: %u\n", 
            shm->active_tenant_count, shm->total_process_count);
    fprintf(stderr, "------------------------------\n");
    
    for (int i = 0; i < MAX_TENANTS; i++) {
        tenant_info_t *tenant = &shm->tenants[i];
        if (tenant->status == TENANT_STATUS_ACTIVE) {
            fprintf(stderr, "租户ID: %u\n", tenant->tenant_id);
            fprintf(stderr, "  名称: %s\n", tenant->tenant_name);
            fprintf(stderr, "  状态: %s\n", 
                    tenant->status == TENANT_STATUS_ACTIVE ? "活跃" : 
                    (tenant->status == TENANT_STATUS_SUSPENDED ? "暂停" : "未激活"));
            fprintf(stderr, "  QP: %d/%u\n", tenant->usage.qp_count, tenant->quota.max_qp_per_tenant);
            fprintf(stderr, "  MR: %d/%u\n", tenant->usage.mr_count, tenant->quota.max_mr_per_tenant);
            fprintf(stderr, "  内存: %llu/%llu bytes\n", 
                    (unsigned long long)tenant->usage.memory_used,
                    (unsigned long long)tenant->quota.max_memory_per_tenant);
            fprintf(stderr, "  进程数: %u\n", tenant->process_count);
            fprintf(stderr, "  创建时间: %s", ctime(&tenant->created_at));
        }
    }
    
    fprintf(stderr, "==============================\n\n");
    
    tenant_shm_unlock(shm);
}

// 获取租户统计信息
int tenant_get_statistics(uint32_t tenant_id, uint64_t *total_qp_creates, uint64_t *total_mr_regs) {
    if (!total_qp_creates || !total_mr_regs || tenant_id >= MAX_TENANTS) {
        return -1;
    }
    
    tenant_shared_memory_t *shm = tenant_shm_get_ptr();
    if (!shm) {
        return -1;
    }
    
    tenant_shm_lock(shm);
    
    tenant_info_t *tenant = &shm->tenants[tenant_id];
    
    if (tenant->status != TENANT_STATUS_ACTIVE) {
        tenant_shm_unlock(shm);
        return -1;
    }
    
    *total_qp_creates = tenant->usage.total_qp_creates;
    *total_mr_regs = tenant->usage.total_mr_regs;
    
    tenant_shm_unlock(shm);
    
    return 0;
}
