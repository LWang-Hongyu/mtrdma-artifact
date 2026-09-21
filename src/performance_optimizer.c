#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include "performance_optimizer.h"
#include "shm/shared_memory.h"

// 全局变量
static resource_cache_t g_cache;
static batch_buffer_t g_batch_buffer;
static perf_optimizer_stats_t g_stats;
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_batch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_flush_thread;
static volatile int g_running = 0;
static volatile int g_cache_enabled = 1;
static volatile int g_batching_enabled = 1;

// 获取当前时间（毫秒）
static uint64_t get_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 获取当前时间（纳秒）
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// 简单的哈希函数
static uint32_t hash_pid(pid_t pid) {
    return ((uint32_t)pid * 2654435761U) % CACHE_BUCKET_COUNT;
}

// 自旋锁实现
static void spin_lock(volatile uint32_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            __sync_synchronize();
        }
    }
}

static void spin_unlock(volatile uint32_t *lock) {
    __sync_lock_release(lock);
}

// 刷新线程函数
static void *flush_thread_func(void *arg) {
    (void)arg;
    
    while (g_running) {
        uint32_t interval = g_batch_buffer.flush_interval_ms;
        if (interval < 10) interval = 10;  // 最小10ms
        
        usleep(interval * 1000);
        
        if (!g_running) break;
        
        if (g_batching_enabled) {
            perf_optimizer_flush_batch_buffer();
        }
        
        // 定期自适应调整
        static int adaptive_counter = 0;
        if (++adaptive_counter >= 100) {  // 每100次刷新调整一次
            perf_optimizer_adaptive_adjust();
            adaptive_counter = 0;
        }
    }
    
    return NULL;
}

// 初始化性能优化器
int perf_optimizer_init(void) {
    fprintf(stderr, "[PERF_OPT] 初始化性能优化器\n");
    
    memset(&g_cache, 0, sizeof(g_cache));
    memset(&g_batch_buffer, 0, sizeof(g_batch_buffer));
    memset(&g_stats, 0, sizeof(g_stats));
    
    // 设置默认值
    g_cache.ttl_ms = 100;  // 100ms TTL
    g_batch_buffer.flush_interval_ms = 50;  // 50ms刷新间隔
    
    // 初始化缓存条目
    for (int i = 0; i < CACHE_BUCKET_COUNT; i++) {
        g_cache.entries[i].valid = 0;
        g_cache.entries[i].lock = 0;
        g_cache.entries[i].pid = 0;
    }
    
    g_stats.start_time = time(NULL);
    g_stats.min_latency_ns = UINT64_MAX;
    
    // 启动刷新线程
    g_running = 1;
    if (pthread_create(&g_flush_thread, NULL, flush_thread_func, NULL) != 0) {
        fprintf(stderr, "[PERF_OPT] 创建刷新线程失败\n");
        g_running = 0;
        return -1;
    }
    
    fprintf(stderr, "[PERF_OPT] 性能优化器初始化成功\n");
    return 0;
}

// 清理性能优化器
void perf_optimizer_cleanup(void) {
    fprintf(stderr, "[PERF_OPT] 清理性能优化器\n");
    
    g_running = 0;
    pthread_join(g_flush_thread, NULL);
    
    // 刷新剩余的批量更新
    perf_optimizer_flush_batch_buffer();
    
    fprintf(stderr, "[PERF_OPT] 性能优化器已清理\n");
}

// 从缓存获取进程资源
bool perf_optimizer_get_cached_resources(pid_t pid, resource_usage_t *usage) {
    if (!g_cache_enabled || !usage) {
        return false;
    }
    
    uint32_t idx = hash_pid(pid);
    cache_entry_t *entry = &g_cache.entries[idx];
    
    // 快速检查（无锁）
    if (!entry->valid || entry->pid != pid) {
        pthread_mutex_lock(&g_stats_mutex);
        g_stats.cache_misses++;
        g_cache.miss_count++;
        pthread_mutex_unlock(&g_stats_mutex);
        return false;
    }
    
    // 获取锁
    spin_lock(&entry->lock);
    
    // 重新检查（获取锁后）
    if (!entry->valid || entry->pid != pid) {
        spin_unlock(&entry->lock);
        pthread_mutex_lock(&g_stats_mutex);
        g_stats.cache_misses++;
        g_cache.miss_count++;
        pthread_mutex_unlock(&g_stats_mutex);
        return false;
    }
    
    // 检查TTL
    uint64_t current_time = get_current_time_ms();
    if (current_time - entry->last_update > g_cache.ttl_ms) {
        entry->valid = 0;
        spin_unlock(&entry->lock);
        pthread_mutex_lock(&g_stats_mutex);
        g_stats.cache_misses++;
        g_cache.miss_count++;
        pthread_mutex_unlock(&g_stats_mutex);
        return false;
    }
    
    // 复制数据
    memcpy(usage, &entry->usage, sizeof(resource_usage_t));
    
    spin_unlock(&entry->lock);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.cache_hits++;
    g_cache.hit_count++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return true;
}

// 更新缓存中的进程资源
void perf_optimizer_update_cached_resources(pid_t pid, const resource_usage_t *usage) {
    if (!g_cache_enabled || !usage) {
        return;
    }
    
    uint32_t idx = hash_pid(pid);
    cache_entry_t *entry = &g_cache.entries[idx];
    
    spin_lock(&entry->lock);
    
    entry->pid = pid;
    memcpy(&entry->usage, usage, sizeof(resource_usage_t));
    entry->last_update = get_current_time_ms();
    entry->valid = 1;
    
    spin_unlock(&entry->lock);
}

// 使缓存条目失效
void perf_optimizer_invalidate_cache(pid_t pid) {
    uint32_t idx = hash_pid(pid);
    cache_entry_t *entry = &g_cache.entries[idx];
    
    spin_lock(&entry->lock);
    
    if (entry->pid == pid) {
        entry->valid = 0;
    }
    
    spin_unlock(&entry->lock);
}

// 清空缓存
void perf_optimizer_clear_cache(void) {
    for (int i = 0; i < CACHE_BUCKET_COUNT; i++) {
        spin_lock(&g_cache.entries[i].lock);
        g_cache.entries[i].valid = 0;
        spin_unlock(&g_cache.entries[i].lock);
    }
    
    pthread_mutex_lock(&g_stats_mutex);
    g_cache.hit_count = 0;
    g_cache.miss_count = 0;
    pthread_mutex_unlock(&g_stats_mutex);
}

// 批量更新资源
int perf_optimizer_batch_update_resource(pid_t pid, const resource_usage_t *usage) {
    if (!g_batching_enabled || !usage) {
        // 直接更新共享内存
        return shm_update_process_resources(pid, usage);
    }
    
    pthread_mutex_lock(&g_batch_mutex);
    
    // 查找是否已有该PID的待更新
    int found = -1;
    for (int i = 0; i < g_batch_buffer.count; i++) {
        if (g_batch_buffer.pids[i] == pid) {
            found = i;
            break;
        }
    }
    
    if (found >= 0) {
        // 合并更新
        memcpy(&g_batch_buffer.pending_updates[found], usage, sizeof(resource_usage_t));
    } else if (g_batch_buffer.count < CACHE_BUCKET_COUNT) {
        // 添加新条目
        g_batch_buffer.pids[g_batch_buffer.count] = pid;
        memcpy(&g_batch_buffer.pending_updates[g_batch_buffer.count], usage, sizeof(resource_usage_t));
        g_batch_buffer.count++;
    } else {
        // 缓冲区满，先刷新
        pthread_mutex_unlock(&g_batch_mutex);
        perf_optimizer_flush_batch_buffer();
        
        // 重新尝试
        pthread_mutex_lock(&g_batch_mutex);
        g_batch_buffer.pids[0] = pid;
        memcpy(&g_batch_buffer.pending_updates[0], usage, sizeof(resource_usage_t));
        g_batch_buffer.count = 1;
    }
    
    pthread_mutex_unlock(&g_batch_mutex);
    
    // 同时更新缓存
    perf_optimizer_update_cached_resources(pid, usage);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.batched_updates++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    return 0;
}

// 强制刷新批量缓冲区
void perf_optimizer_flush_batch_buffer(void) {
    pthread_mutex_lock(&g_batch_mutex);
    
    if (g_batch_buffer.count == 0) {
        pthread_mutex_unlock(&g_batch_mutex);
        return;
    }
    
    // 批量写入共享内存
    for (int i = 0; i < g_batch_buffer.count; i++) {
        shm_update_process_resources(g_batch_buffer.pids[i], 
                                      &g_batch_buffer.pending_updates[i]);
    }
    
    int count = g_batch_buffer.count;
    g_batch_buffer.count = 0;
    g_batch_buffer.last_flush = get_current_time_ms();
    
    pthread_mutex_unlock(&g_batch_mutex);
    
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.batch_flushes++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    fprintf(stderr, "[PERF_OPT] 批量刷新 %d 个更新\n", count);
}

// 设置缓存TTL
void perf_optimizer_set_cache_ttl(uint32_t ttl_ms) {
    if (ttl_ms < 10) ttl_ms = 10;
    if (ttl_ms > 10000) ttl_ms = 10000;
    g_cache.ttl_ms = ttl_ms;
    fprintf(stderr, "[PERF_OPT] 缓存TTL设置为 %u ms\n", ttl_ms);
}

// 设置批量刷新间隔
void perf_optimizer_set_batch_interval(uint32_t interval_ms) {
    if (interval_ms < 10) interval_ms = 10;
    if (interval_ms > 1000) interval_ms = 1000;
    g_batch_buffer.flush_interval_ms = interval_ms;
    fprintf(stderr, "[PERF_OPT] 批量刷新间隔设置为 %u ms\n", interval_ms);
}

// 启用/禁用缓存
void perf_optimizer_enable_cache(bool enable) {
    g_cache_enabled = enable ? 1 : 0;
    fprintf(stderr, "[PERF_OPT] 缓存已%s\n", enable ? "启用" : "禁用");
}

// 启用/禁用批量更新
void perf_optimizer_enable_batching(bool enable) {
    g_batching_enabled = enable ? 1 : 0;
    fprintf(stderr, "[PERF_OPT] 批量更新已%s\n", enable ? "启用" : "禁用");
}

// 获取性能统计
void perf_optimizer_get_stats(perf_optimizer_stats_t *stats) {
    if (!stats) return;
    
    pthread_mutex_lock(&g_stats_mutex);
    memcpy(stats, &g_stats, sizeof(perf_optimizer_stats_t));
    
    // 计算缓存命中率
    uint64_t total = g_stats.cache_hits + g_stats.cache_misses;
    if (total > 0) {
        stats->cache_hit_rate = (double)g_stats.cache_hits / total * 100.0;
    } else {
        stats->cache_hit_rate = 0.0;
    }
    
    // 计算平均延迟
    if (g_stats.total_operations > 0) {
        stats->avg_latency_ns = (double)g_stats.total_latency_ns / g_stats.total_operations;
    } else {
        stats->avg_latency_ns = 0.0;
    }
    
    pthread_mutex_unlock(&g_stats_mutex);
}

// 重置性能统计
void perf_optimizer_reset_stats(void) {
    pthread_mutex_lock(&g_stats_mutex);
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.min_latency_ns = UINT64_MAX;
    g_stats.start_time = time(NULL);
    pthread_mutex_unlock(&g_stats_mutex);
    
    g_cache.hit_count = 0;
    g_cache.miss_count = 0;
    g_cache.eviction_count = 0;
}

// 打印性能统计
void perf_optimizer_print_stats(void) {
    perf_optimizer_stats_t stats;
    perf_optimizer_get_stats(&stats);
    
    time_t now = time(NULL);
    double runtime = difftime(now, stats.start_time);
    
    fprintf(stderr, "\n========== 性能优化器统计 ==========\n");
    fprintf(stderr, "运行时间: %.0f 秒\n", runtime);
    fprintf(stderr, "\n操作统计:\n");
    fprintf(stderr, "  总操作数: %lu\n", stats.total_operations);
    fprintf(stderr, "  拦截操作: %lu\n", stats.intercepted_operations);
    fprintf(stderr, "  允许操作: %lu\n", stats.allowed_operations);
    fprintf(stderr, "  拒绝操作: %lu\n", stats.denied_operations);
    
    if (stats.total_operations > 0) {
        fprintf(stderr, "\n延迟统计:\n");
        fprintf(stderr, "  平均延迟: %.2f ns\n", stats.avg_latency_ns);
        fprintf(stderr, "  最小延迟: %lu ns\n", stats.min_latency_ns);
        fprintf(stderr, "  最大延迟: %lu ns\n", stats.max_latency_ns);
    }
    
    fprintf(stderr, "\n缓存统计:\n");
    fprintf(stderr, "  缓存命中: %lu\n", stats.cache_hits);
    fprintf(stderr, "  缓存未命中: %lu\n", stats.cache_misses);
    fprintf(stderr, "  命中率: %.2f%%\n", stats.cache_hit_rate);
    
    fprintf(stderr, "\n批量更新统计:\n");
    fprintf(stderr, "  刷新次数: %lu\n", stats.batch_flushes);
    fprintf(stderr, "  批量更新数: %lu\n", stats.batched_updates);
    if (stats.batch_flushes > 0) {
        fprintf(stderr, "  平均每批: %.2f\n", 
                (double)stats.batched_updates / stats.batch_flushes);
    }
    
    fprintf(stderr, "====================================\n\n");
}

// 优化的资源检查函数（带缓存）
bool perf_optimizer_check_resource_fast(pid_t pid, int resource_type, 
                                         uint32_t current, uint32_t requested,
                                         uint32_t limit) {
    uint64_t start = get_current_time_ns();
    
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.total_operations++;
    pthread_mutex_unlock(&g_stats_mutex);
    
    // 尝试从缓存获取
    resource_usage_t cached_usage;
    bool cache_hit = perf_optimizer_get_cached_resources(pid, &cached_usage);
    
    uint32_t actual_current = current;
    if (cache_hit) {
        switch (resource_type) {
            case 0: actual_current = cached_usage.qp_count; break;
            case 1: actual_current = cached_usage.mr_count; break;
            case 2: actual_current = (uint32_t)(cached_usage.memory_used / (1024*1024)); break; // MB
        }
    }
    
    bool allowed = (actual_current + requested) <= limit;
    
    uint64_t end = get_current_time_ns();
    uint64_t latency = end - start;
    
    pthread_mutex_lock(&g_stats_mutex);
    g_stats.total_latency_ns += latency;
    if (latency < g_stats.min_latency_ns) g_stats.min_latency_ns = latency;
    if (latency > g_stats.max_latency_ns) g_stats.max_latency_ns = latency;
    
    if (allowed) {
        g_stats.allowed_operations++;
    } else {
        g_stats.denied_operations++;
    }
    pthread_mutex_unlock(&g_stats_mutex);
    
    return allowed;
}

// 预取资源到缓存
void perf_optimizer_prefetch_resources(pid_t pid) {
    resource_usage_t usage;
    if (shm_get_process_resources(pid, &usage) == 0) {
        perf_optimizer_update_cached_resources(pid, &usage);
        fprintf(stderr, "[PERF_OPT] 预取PID %d 的资源到缓存\n", pid);
    }
}

// 自适应缓存调整
void perf_optimizer_adaptive_adjust(void) {
    pthread_mutex_lock(&g_stats_mutex);
    uint64_t hits = g_stats.cache_hits;
    uint64_t misses = g_stats.cache_misses;
    pthread_mutex_unlock(&g_stats_mutex);
    
    uint64_t total = hits + misses;
    if (total < 100) return;  // 样本太少，不调整
    
    double hit_rate = (double)hits / total * 100.0;
    
    // 根据命中率调整TTL
    if (hit_rate < 50.0 && g_cache.ttl_ms > 50) {
        // 命中率低，减少TTL
        g_cache.ttl_ms -= 10;
        fprintf(stderr, "[PERF_OPT] 自适应调整: 降低TTL至 %u ms (命中率=%.1f%%)\n", 
                g_cache.ttl_ms, hit_rate);
    } else if (hit_rate > 90.0 && g_cache.ttl_ms < 500) {
        // 命中率高，增加TTL
        g_cache.ttl_ms += 10;
        fprintf(stderr, "[PERF_OPT] 自适应调整: 增加TTL至 %u ms (命中率=%.1f%%)\n", 
                g_cache.ttl_ms, hit_rate);
    }
}
