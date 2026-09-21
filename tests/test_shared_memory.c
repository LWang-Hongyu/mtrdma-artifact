/*
 * 共享内存功能单元测试
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../src/shm/shared_memory.h"
#include "../src/shm/shared_memory_tenant.h"

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", msg); \
        return -1; \
    } else { \
        printf("  [PASS] %s\n", msg); \
    } \
} while(0)

// 测试基本共享内存功能
int test_basic_shm() {
    printf("\n[Test] 基本共享内存功能\n");
    
    // 清理
    shm_destroy();
    
    // 初始化
    TEST_ASSERT(shm_init() == 0, "共享内存初始化成功");
    
    // 获取指针
    shared_memory_data_t *shm = shm_get_ptr();
    TEST_ASSERT(shm != NULL, "获取共享内存指针成功");
    
    // 设置全局限制
    TEST_ASSERT(shm_set_global_limits(100, 1000, 1024*1024*1024) == 0, 
                "设置全局限制成功");
    
    // 验证设置
    TEST_ASSERT(shm->max_global_qp == 100, "QP限制设置正确");
    TEST_ASSERT(shm->max_global_mr == 1000, "MR限制设置正确");
    
    // 更新进程资源
    resource_usage_t usage = {.qp_count = 5, .mr_count = 10, .memory_used = 1024};
    TEST_ASSERT(shm_update_process_resources(getpid(), &usage) == 0,
                "更新进程资源成功");
    
    // 读取进程资源
    resource_usage_t read_usage;
    TEST_ASSERT(shm_get_process_resources(getpid(), &read_usage) == 0,
                "读取进程资源成功");
    TEST_ASSERT(read_usage.qp_count == 5, "QP计数正确");
    TEST_ASSERT(read_usage.mr_count == 10, "MR计数正确");
    
    // 清理
    shm_destroy();
    
    printf("[Test] 基本共享内存功能 - PASSED\n");
    return 0;
}

// 测试多进程共享内存
int test_multi_process_shm() {
    printf("\n[Test] 多进程共享内存\n");
    
    shm_destroy();
    TEST_ASSERT(shm_init() == 0, "父进程初始化共享内存");
    
    // 设置初始值
    resource_usage_t usage = {.qp_count = 10, .mr_count = 20, .memory_used = 2048};
    shm_update_process_resources(getpid(), &usage);
    
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程
        shared_memory_data_t *shm = shm_get_ptr();
        if (!shm) {
            printf("  [FAIL] 子进程获取共享内存失败\n");
            exit(1);
        }
        
        // 子进程读取父进程设置的值
        resource_usage_t parent_usage;
        if (shm_get_process_resources(getppid(), &parent_usage) != 0) {
            printf("  [FAIL] 子进程读取父进程资源失败\n");
            exit(1);
        }
        
        if (parent_usage.qp_count != 10) {
            printf("  [FAIL] 子进程读取QP计数错误: %d != 10\n", parent_usage.qp_count);
            exit(1);
        }
        
        // 子进程添加自己的资源
        resource_usage_t child_usage = {.qp_count = 5, .mr_count = 5, .memory_used = 512};
        shm_update_process_resources(getpid(), &child_usage);
        
        exit(0);
    } else if (pid > 0) {
        // 父进程等待子进程
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("  [PASS] 子进程共享内存访问成功\n");
        } else {
            printf("  [FAIL] 子进程测试失败\n");
            return -1;
        }
        
        // 验证子进程的资源被记录
        resource_usage_t child_usage;
        if (shm_get_process_resources(pid, &child_usage) == 0) {
            TEST_ASSERT(child_usage.qp_count == 5, "父进程可读取子进程资源");
        }
    } else {
        printf("  [FAIL] fork失败\n");
        return -1;
    }
    
    shm_destroy();
    printf("[Test] 多进程共享内存 - PASSED\n");
    return 0;
}

// 测试租户共享内存
int test_tenant_shm() {
    printf("\n[Test] 租户共享内存功能\n");
    
    tenant_shm_destroy();
    TEST_ASSERT(tenant_shm_init() == 0, "租户共享内存初始化成功");
    
    // 创建租户
    tenant_quota_t quota = {
        .max_qp_per_tenant = 50,
        .max_mr_per_tenant = 500,
        .max_memory_per_tenant = 1024*1024*100,
        .max_cq_per_tenant = 50,
        .max_pd_per_tenant = 50
    };
    
    TEST_ASSERT(tenant_create(1, "TestTenant", &quota) == 0, "创建租户成功");
    
    // 获取租户信息
    tenant_info_t info;
    TEST_ASSERT(tenant_get_info(1, &info) == 0, "获取租户信息成功");
    TEST_ASSERT(info.tenant_id == 1, "租户ID正确");
    TEST_ASSERT(strcmp(info.tenant_name, "TestTenant") == 0, "租户名称正确");
    TEST_ASSERT(info.status == TENANT_STATUS_ACTIVE, "租户状态为活跃");
    TEST_ASSERT(info.quota.max_qp_per_tenant == 50, "租户QP配额正确");
    
    // 绑定进程到租户
    pid_t test_pid = 12345;
    TEST_ASSERT(tenant_bind_process(test_pid, 1) == 0, "绑定进程到租户成功");
    
    // 查询进程所属租户
    uint32_t tenant_id;
    TEST_ASSERT(tenant_get_process_tenant(test_pid, &tenant_id) == 0, 
                "查询进程租户成功");
    TEST_ASSERT(tenant_id == 1, "进程租户ID正确");
    
    // 更新资源使用
    tenant_resource_usage_t usage = {
        .qp_count = 10,
        .mr_count = 20,
        .memory_used = 10240,
        .total_qp_creates = 15
    };
    TEST_ASSERT(tenant_update_resource_usage(1, &usage) == 0, 
                "更新租户资源使用成功");
    
    // 读取资源使用
    tenant_resource_usage_t read_usage;
    TEST_ASSERT(tenant_get_resource_usage(1, &read_usage) == 0,
                "读取租户资源使用成功");
    TEST_ASSERT(read_usage.qp_count == 10, "租户QP计数正确");
    TEST_ASSERT(read_usage.total_qp_creates == 15, "租户QP创建统计正确");
    
    // 检查资源限制
    TEST_ASSERT(tenant_check_resource_limit(1, 0, 40) == false, 
                "租户资源未超限 (40 < 50)");
    TEST_ASSERT(tenant_check_resource_limit(1, 0, 50) == true, 
                "租户资源超限检测正确 (50+10 > 50)");
    
    // 解绑进程
    TEST_ASSERT(tenant_unbind_process(test_pid) == 0, "解绑进程成功");
    
    // 删除租户
    TEST_ASSERT(tenant_delete(1) == 0, "删除租户成功");
    
    // 验证删除
    TEST_ASSERT(tenant_get_info(1, &info) != 0, "已删除租户无法获取信息");
    
    tenant_shm_destroy();
    printf("[Test] 租户共享内存功能 - PASSED\n");
    return 0;
}

// 测试并发访问
int test_concurrent_access() {
    printf("\n[Test] 并发访问测试\n");
    
    shm_destroy();
    TEST_ASSERT(shm_init() == 0, "初始化共享内存");
    
    #define NUM_PROCESSES 10
    #define NUM_UPDATES 100
    
    pid_t pids[NUM_PROCESSES];
    
    // 创建多个进程并发更新
    for (int i = 0; i < NUM_PROCESSES; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程
            for (int j = 0; j < NUM_UPDATES; j++) {
                resource_usage_t usage;
                if (shm_get_process_resources(getpid(), &usage) == 0) {
                    usage.qp_count++;
                    usage.mr_count++;
                    shm_update_process_resources(getpid(), &usage);
                }
                usleep(10); // 短暂延迟
            }
            exit(0);
        } else if (pid > 0) {
            pids[i] = pid;
        } else {
            printf("  [FAIL] fork失败\n");
            return -1;
        }
    }
    
    // 等待所有子进程
    for (int i = 0; i < NUM_PROCESSES; i++) {
        waitpid(pids[i], NULL, 0);
    }
    
    // 验证结果
    int total_qp = 0;
    for (int i = 0; i < NUM_PROCESSES; i++) {
        resource_usage_t usage;
        if (shm_get_process_resources(pids[i], &usage) == 0) {
            total_qp += usage.qp_count;
        }
    }
    
    TEST_ASSERT(total_qp == NUM_PROCESSES * NUM_UPDATES, 
                "并发更新计数正确");
    
    shm_destroy();
    printf("[Test] 并发访问测试 - PASSED\n");
    return 0;
}

int main() {
    printf("======================================\n");
    printf("   共享内存功能单元测试\n");
    printf("======================================\n");
    
    int failed = 0;
    
    if (test_basic_shm() != 0) failed++;
    if (test_multi_process_shm() != 0) failed++;
    if (test_tenant_shm() != 0) failed++;
    if (test_concurrent_access() != 0) failed++;
    
    printf("\n======================================\n");
    if (failed == 0) {
        printf("   所有测试 PASSED!\n");
    } else {
        printf("   %d 个测试 FAILED\n", failed);
    }
    printf("======================================\n");
    
    return failed;
}
