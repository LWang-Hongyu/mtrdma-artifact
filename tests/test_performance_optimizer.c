/*
 * 性能优化器功能测试
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../include/performance_optimizer.h"

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s\n", msg); \
        return -1; \
    } else { \
        printf("  [PASS] %s\n", msg); \
    } \
} while(0)

int test_cache_basic() {
    printf("\n[Test] 缓存基本功能\n");
    
    TEST_ASSERT(perf_optimizer_init() == 0, "性能优化器初始化成功");
    
    // 设置缓存TTL
    perf_optimizer_set_cache_ttl(200);
    
    // 测试缓存更新
    pid_t test_pid = 12345;
    resource_usage_t usage = {.qp_count = 10, .mr_count = 20, .memory_used = 1024};
    perf_optimizer_update_cached_resources(test_pid, &usage);
    
    // 测试缓存读取
    resource_usage_t read_usage;
    TEST_ASSERT(perf_optimizer_get_cached_resources(test_pid, &read_usage) == true,
                "从缓存读取资源成功");
    TEST_ASSERT(read_usage.qp_count == 10, "缓存QP计数正确");
    TEST_ASSERT(read_usage.mr_count == 20, "缓存MR计数正确");
    
    // 测试缓存失效
    perf_optimizer_invalidate_cache(test_pid);
    TEST_ASSERT(perf_optimizer_get_cached_resources(test_pid, &read_usage) == false,
                "缓存失效后读取失败（符合预期）");
    
    // 测试清空缓存
    perf_optimizer_update_cached_resources(test_pid, &usage);
    TEST_ASSERT(perf_optimizer_get_cached_resources(test_pid, &read_usage) == true,
                "重新更新缓存后读取成功");
    perf_optimizer_clear_cache();
    TEST_ASSERT(perf_optimizer_get_cached_resources(test_pid, &read_usage) == false,
                "清空缓存后读取失败（符合预期）");
    
    perf_optimizer_cleanup();
    
    printf("[Test] 缓存基本功能 - PASSED\n");
    return 0;
}

int test_cache_ttl() {
    printf("\n[Test] 缓存TTL功能\n");
    
    TEST_ASSERT(perf_optimizer_init() == 0, "性能优化器初始化成功");
    
    // 设置短TTL
    perf_optimizer_set_cache_ttl(50); // 50ms
    
    pid_t test_pid = 12346;
    resource_usage_t usage = {.qp_count = 5, .mr_count = 10, .memory_used = 512};
    perf_optimizer_update_cached_resources(test_pid, &usage);
    
    // 立即读取应该成功
    resource_usage_t read_usage;
    TEST_ASSERT(perf_optimizer_get_cached_resources(test_pid, &read_usage) == true,
                "TTL内读取缓存成功");
    
    // 等待TTL过期
    usleep(60000); // 60ms
    
    // 过期后读取应该失败
    TEST_ASSERT(perf_optimizer_get_cached_resources(test_pid, &read_usage) == false,
                "TTL过期后读取缓存失败（符合预期）");
    
    perf_optimizer_cleanup();
    
    printf("[Test] 缓存TTL功能 - PASSED\n");
    return 0;
}

int test_performance_stats() {
    printf("\n[Test] 性能统计功能\n");
    
    TEST_ASSERT(perf_optimizer_init() == 0, "性能优化器初始化成功");
    
    // 执行一些操作来产生统计
    for (int i = 0; i < 100; i++) {
        pid_t pid = 1000 + i;
        resource_usage_t usage = {.qp_count = i, .mr_count = i * 2, .memory_used = i * 1024};
        perf_optimizer_update_cached_resources(pid, &usage);
        
        resource_usage_t read_usage;
        perf_optimizer_get_cached_resources(pid, &read_usage);
    }
    
    // 获取统计
    perf_optimizer_stats_t stats;
    perf_optimizer_get_stats(&stats);
    
    printf("  [INFO] 缓存命中: %lu, 未命中: %lu\n", stats.cache_hits, stats.cache_misses);
    printf("  [INFO] 命中率: %.2f%%\n", stats.cache_hit_rate);
    
    // 至少应该有一些命中
    TEST_ASSERT(stats.cache_hits > 0, "有缓存命中记录");
    
    // 重置统计
    perf_optimizer_reset_stats();
    perf_optimizer_get_stats(&stats);
    TEST_ASSERT(stats.cache_hits == 0, "重置后缓存命中为0");
    
    perf_optimizer_cleanup();
    
    printf("[Test] 性能统计功能 - PASSED\n");
    return 0;
}

int test_fast_resource_check() {
    printf("\n[Test] 快速资源检查\n");
    
    TEST_ASSERT(perf_optimizer_init() == 0, "性能优化器初始化成功");
    
    // 预加载一些缓存数据
    pid_t test_pid = 12347;
    resource_usage_t usage = {.qp_count = 30, .mr_count = 50, .memory_used = 10240};
    perf_optimizer_update_cached_resources(test_pid, &usage);
    
    // 测试快速检查 - 应该通过
    TEST_ASSERT(perf_optimizer_check_resource_fast(test_pid, 0, 30, 10, 50) == true,
                "快速检查: 30+10 <= 50 应该通过");
    
    // 测试快速检查 - 应该失败
    TEST_ASSERT(perf_optimizer_check_resource_fast(test_pid, 0, 48, 5, 50) == false,
                "快速检查: 48+5 > 50 应该失败");
    
    // 获取统计查看延迟
    perf_optimizer_stats_t stats;
    perf_optimizer_get_stats(&stats);
    printf("  [INFO] 平均延迟: %.2f ns\n", stats.avg_latency_ns);
    
    perf_optimizer_cleanup();
    
    printf("[Test] 快速资源检查 - PASSED\n");
    return 0;
}

int test_batch_update() {
    printf("\n[Test] 批量更新功能\n");
    
    TEST_ASSERT(perf_optimizer_init() == 0, "性能优化器初始化成功");
    
    // 启用批量更新
    perf_optimizer_enable_batching(true);
    perf_optimizer_set_batch_interval(100); // 100ms
    
    // 添加多个批量更新
    for (int i = 0; i < 10; i++) {
        pid_t pid = 2000 + i;
        resource_usage_t usage = {.qp_count = i, .mr_count = i * 2, .memory_used = i * 1024};
        perf_optimizer_batch_update_resource(pid, &usage);
    }
    
    // 获取统计
    perf_optimizer_stats_t stats;
    perf_optimizer_get_stats(&stats);
    printf("  [INFO] 批量更新数: %lu\n", stats.batched_updates);
    
    TEST_ASSERT(stats.batched_updates == 10, "批量更新计数正确");
    
    // 手动刷新
    perf_optimizer_flush_batch_buffer();
    
    perf_optimizer_get_stats(&stats);
    printf("  [INFO] 刷新次数: %lu\n", stats.batch_flushes);
    
    perf_optimizer_cleanup();
    
    printf("[Test] 批量更新功能 - PASSED\n");
    return 0;
}

int main() {
    printf("======================================\n");
    printf("   性能优化器功能测试\n");
    printf("======================================\n");
    
    int failed = 0;
    
    if (test_cache_basic() != 0) failed++;
    if (test_cache_ttl() != 0) failed++;
    if (test_performance_stats() != 0) failed++;
    if (test_fast_resource_check() != 0) failed++;
    if (test_batch_update() != 0) failed++;
    
    printf("\n======================================\n");
    if (failed == 0) {
        printf("   所有测试 PASSED!\n");
    } else {
        printf("   %d 个测试 FAILED\n", failed);
    }
    printf("======================================\n");
    
    return failed;
}
