#ifndef PERFORMANCE_OPTIMIZER_H
#define PERFORMANCE_OPTIMIZER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "../src/shm/shared_memory.h"

// 缓存大小
#define RESOURCE_CACHE_SIZE 1024
#define CACHE_LINE_SIZE 64
#define CACHE_BUCKET_COUNT 256

// 缓存条目结构（使用缓存行对齐避免伪共享）
typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) {
    pid_t pid;
    resource_usage_t usage;
    uint64_t last_update;
    volatile uint32_t valid;  // 0=无效, 1=有效
    volatile uint32_t lock;
} cache_entry_t;

// 资源缓存结构
typedef struct {
    cache_entry_t entries[CACHE_BUCKET_COUNT];
    uint64_t hit_count;
    uint64_t miss_count;
    uint64_t eviction_count;
    uint32_t ttl_ms;  // 缓存TTL（毫秒）
} resource_cache_t;

// 批量更新缓冲区
typedef struct {
    resource_usage_t pending_updates[CACHE_BUCKET_COUNT];
    pid_t pids[CACHE_BUCKET_COUNT];
    int count;
    uint64_t last_flush;
    uint32_t flush_interval_ms;
} batch_buffer_t;

// 性能统计结构
typedef struct {
    uint64_t total_operations;
    uint64_t intercepted_operations;
    uint64_t allowed_operations;
    uint64_t denied_operations;
    uint64_t total_latency_ns;
    uint64_t min_latency_ns;
    uint64_t max_latency_ns;
    double avg_latency_ns;
    
    // 缓存统计
    uint64_t cache_hits;
    uint64_t cache_misses;
    double cache_hit_rate;
    
    // 批量更新统计
    uint64_t batch_flushes;
    uint64_t batched_updates;
    
    time_t start_time;
} perf_optimizer_stats_t;

// 初始化性能优化器
int perf_optimizer_init(void);

// 清理性能优化器
void perf_optimizer_cleanup(void);

// 从缓存获取进程资源（快速路径）
bool perf_optimizer_get_cached_resources(pid_t pid, resource_usage_t *usage);

// 更新缓存中的进程资源
void perf_optimizer_update_cached_resources(pid_t pid, const resource_usage_t *usage);

// 使缓存条目失效
void perf_optimizer_invalidate_cache(pid_t pid);

// 清空缓存
void perf_optimizer_clear_cache(void);

// 批量更新资源（延迟写入共享内存）
int perf_optimizer_batch_update_resource(pid_t pid, const resource_usage_t *usage);

// 强制刷新批量缓冲区
void perf_optimizer_flush_batch_buffer(void);

// 设置缓存TTL
void perf_optimizer_set_cache_ttl(uint32_t ttl_ms);

// 设置批量刷新间隔
void perf_optimizer_set_batch_interval(uint32_t interval_ms);

// 启用/禁用缓存
void perf_optimizer_enable_cache(bool enable);

// 启用/禁用批量更新
void perf_optimizer_enable_batching(bool enable);

// 获取性能统计
void perf_optimizer_get_stats(perf_optimizer_stats_t *stats);

// 重置性能统计
void perf_optimizer_reset_stats(void);

// 打印性能统计
void perf_optimizer_print_stats(void);

// 优化的资源检查函数（带缓存）
bool perf_optimizer_check_resource_fast(pid_t pid, int resource_type, 
                                         uint32_t current, uint32_t requested,
                                         uint32_t limit);

// 预取资源到缓存（用于热点进程）
void perf_optimizer_prefetch_resources(pid_t pid);

// 自适应缓存调整（根据命中率动态调整TTL）
void perf_optimizer_adaptive_adjust(void);

#endif // PERFORMANCE_OPTIMIZER_H
