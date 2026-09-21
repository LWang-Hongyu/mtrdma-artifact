/*
 * 动态策略功能测试
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../include/dynamic_policy.h"

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", msg); \
        return -1; \
    } else { \
        printf("  [PASS] %s\n", msg); \
    } \
} while(0)

int test_dynamic_policy_basic() {
    printf("\n[Test] 动态策略基本功能\n");
    
    TEST_ASSERT(dynamic_policy_init() == 0, "动态策略初始化成功");
    
    // 获取默认配置
    dynamic_policy_config_t config;
    TEST_ASSERT(dynamic_policy_get_config(&config) == 0, "获取默认配置成功");
    
    // 验证默认配置
    TEST_ASSERT(config.qp_policy.limit.max_per_process == 100, "默认QP每进程限制正确");
    TEST_ASSERT(config.qp_policy.limit.max_global == 1000, "默认QP全局限制正确");
    
    // 设置新的资源限制
    resource_limit_t new_limit;
    new_limit.max_per_process = 50;
    new_limit.max_global = 500;
    new_limit.max_memory = 1024 * 1024 * 1024;
    
    TEST_ASSERT(dynamic_policy_set_limit(RESOURCE_QP, &new_limit) == 0, 
                "设置QP限制成功");
    
    // 验证新配置
    resource_limit_t read_limit;
    TEST_ASSERT(dynamic_policy_get_limit(RESOURCE_QP, &read_limit) == 0,
                "读取QP限制成功");
    TEST_ASSERT(read_limit.max_per_process == 50, "QP每进程限制更新正确");
    TEST_ASSERT(read_limit.max_global == 500, "QP全局限制更新正确");
    
    // 测试资源限制检查
    TEST_ASSERT(dynamic_policy_check_limit(RESOURCE_QP, 40, 5) == false,
                "资源检查: 40+5 <= 50 应该通过");
    TEST_ASSERT(dynamic_policy_check_limit(RESOURCE_QP, 48, 5) == true,
                "资源检查: 48+5 > 50 应该失败");
    
    // 测试自动调整参数设置
    TEST_ASSERT(dynamic_policy_set_auto_adjust(true, 30, 0.8f, 0.2f) == 0,
                "设置自动调整参数成功");
    
    dynamic_policy_cleanup();
    
    printf("[Test] 动态策略基本功能 - PASSED\n");
    return 0;
}

int test_tenant_policy() {
    printf("\n[Test] 租户策略功能\n");
    
    TEST_ASSERT(dynamic_policy_init() == 0, "动态策略初始化成功");
    
    // 创建租户策略
    tenant_policy_t policy;
    memset(&policy, 0, sizeof(policy));
    policy.tenant_id = 1;
    strncpy(policy.tenant_name, "TestTenant", sizeof(policy.tenant_name) - 1);
    policy.policy.qp_policy.limit.max_per_process = 30;
    policy.policy.qp_policy.limit.max_global = 300;
    policy.policy.mr_policy.limit.max_per_process = 60;
    policy.policy.mr_policy.limit.max_global = 600;
    
    TEST_ASSERT(dynamic_policy_set_tenant_policy(1, &policy) == 0,
                "设置租户策略成功");
    
    // 读取租户策略
    tenant_policy_t read_policy;
    TEST_ASSERT(dynamic_policy_get_tenant_policy(1, &read_policy) == 0,
                "读取租户策略成功");
    TEST_ASSERT(read_policy.tenant_id == 1, "租户ID正确");
    TEST_ASSERT(read_policy.policy.qp_policy.limit.max_per_process == 30,
                "租户QP限制正确");
    
    // 测试租户资源限制检查
    TEST_ASSERT(dynamic_policy_check_tenant_limit(1, RESOURCE_QP, 25, 3) == false,
                "租户资源检查: 25+3 <= 30 应该通过");
    TEST_ASSERT(dynamic_policy_check_tenant_limit(1, RESOURCE_QP, 28, 5) == true,
                "租户资源检查: 28+5 > 30 应该失败");
    
    // 删除租户策略
    TEST_ASSERT(dynamic_policy_delete_tenant_policy(1) == 0,
                "删除租户策略成功");
    
    dynamic_policy_cleanup();
    
    printf("[Test] 租户策略功能 - PASSED\n");
    return 0;
}

int test_policy_config_file() {
    printf("\n[Test] 策略配置文件\n");
    
    TEST_ASSERT(dynamic_policy_init() == 0, "动态策略初始化成功");
    
    // 设置一些配置
    resource_limit_t limit = {.max_per_process = 75, .max_global = 750, .max_memory = 0};
    dynamic_policy_set_limit(RESOURCE_QP, &limit);
    dynamic_policy_set_auto_adjust(false, 60, 0.75f, 0.25f);
    
    // 保存配置
    TEST_ASSERT(dynamic_policy_save_config("/tmp/test_policy.conf") == 0,
                "保存策略配置成功");
    
    // 读取配置文件验证
    FILE *fp = fopen("/tmp/test_policy.conf", "r");
    TEST_ASSERT(fp != NULL, "配置文件存在");
    
    char line[256];
    int found_max_qp = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "max_qp_per_process") && strstr(line, "75")) {
            found_max_qp = 1;
        }
    }
    fclose(fp);
    TEST_ASSERT(found_max_qp, "配置文件中包含正确的QP限制");
    
    // 加载配置（恢复到默认后加载）
    dynamic_policy_set_default();
    TEST_ASSERT(dynamic_policy_load_config("/tmp/test_policy.conf") == 0,
                "加载策略配置成功");
    
    // 验证加载的配置
    resource_limit_t read_limit;
    dynamic_policy_get_limit(RESOURCE_QP, &read_limit);
    TEST_ASSERT(read_limit.max_per_process == 75, "加载后QP限制正确");
    
    dynamic_policy_cleanup();
    
    printf("[Test] 策略配置文件 - PASSED\n");
    return 0;
}

int main() {
    printf("======================================\n");
    printf("   动态策略功能测试\n");
    printf("======================================\n");
    
    int failed = 0;
    
    if (test_dynamic_policy_basic() != 0) failed++;
    if (test_tenant_policy() != 0) failed++;
    if (test_policy_config_file() != 0) failed++;
    
    printf("\n======================================\n");
    if (failed == 0) {
        printf("   所有测试 PASSED!\n");
    } else {
        printf("   %d 个测试 FAILED\n", failed);
    }
    printf("======================================\n");
    
    return failed;
}
