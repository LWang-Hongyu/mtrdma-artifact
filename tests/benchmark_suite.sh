#!/bin/bash
# RDMA多租户系统论文实验套件
# 用于生成论文所需的性能数据

set -e

PROJECT_ROOT="/opt/mtrdma"
BUILD_DIR="$PROJECT_ROOT/build"
RESULTS_DIR="$PROJECT_ROOT/paper_results"

# 创建结果目录
mkdir -p $RESULTS_DIR/{latency,throughput,scalability,isolation}

echo "========================================"
echo "   RDMA多租户系统论文实验套件"
echo "========================================"

# 检查RDMA设备
check_rdma() {
    if ! ibstat 2>/dev/null | grep -q "State: Active"; then
        echo "错误: RDMA设备未就绪"
        exit 1
    fi
    echo "✓ RDMA设备检查通过"
}

# 实验1: 拦截开销测试 (基准vs拦截)
benchmark_overhead() {
    echo ""
    echo "[实验1] 拦截开销测试"
    
    cat > /tmp/benchmark_overhead.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <infiniband/verbs.h>
#include <string.h>

#define NUM_ITERATIONS 1000

static inline unsigned long long get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main() {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list || !dev_list[0]) {
        printf("未找到RDMA设备\n");
        return 1;
    }
    
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    
    struct ibv_qp_init_attr qp_init_attr = {
        .qp_type = IBV_QPT_RC,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = { .max_send_wr = 10, .max_recv_wr = 10, .max_send_sge = 1, .max_recv_sge = 1 }
    };
    
    // 预热
    for (int i = 0; i < 10; i++) {
        struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
        if (qp) ibv_destroy_qp(qp);
    }
    
    // QP创建延迟测试
    unsigned long long start = get_time_ns();
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
        if (qp) ibv_destroy_qp(qp);
    }
    unsigned long long end = get_time_ns();
    
    double avg_latency = (double)(end - start) / NUM_ITERATIONS / 1000.0; // us
    printf("QP_CREATE_LATENCY: %.3f us\n", avg_latency);
    
    // MR注册延迟测试
    char *buf = malloc(4096);
    start = get_time_ns();
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        struct ibv_mr *mr = ibv_reg_mr(pd, buf, 4096, IBV_ACCESS_LOCAL_WRITE);
        if (mr) ibv_dereg_mr(mr);
    }
    end = get_time_ns();
    
    avg_latency = (double)(end - start) / NUM_ITERATIONS / 1000.0;
    printf("MR_REG_LATENCY: %.3f us\n", avg_latency);
    
    free(buf);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    
    return 0;
}
EOF
    
    gcc -o /tmp/benchmark_overhead /tmp/benchmark_overhead.c -libverbs -O2
    
    echo "  测试无拦截基线..."
    /tmp/benchmark_overhead > $RESULTS_DIR/latency/baseline.txt 2>&1
    cat $RESULTS_DIR/latency/baseline.txt
    
    echo "  测试有拦截..."
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=10000
    export LD_PRELOAD=$BUILD_DIR/librdma_intercept.so
    /tmp/benchmark_overhead > $RESULTS_DIR/latency/with_intercept.txt 2>&1
    unset LD_PRELOAD
    cat $RESULTS_DIR/latency/with_intercept.txt
    
    echo "  ✓ 拦截开销测试完成"
}

# 实验2: 缓存性能测试
benchmark_cache() {
    echo ""
    echo "[实验2] 缓存性能测试"
    
    cat > /tmp/benchmark_cache.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "performance_optimizer.h"

#define NUM_OPS 100000

static inline unsigned long long get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main() {
    perf_optimizer_init();
    
    resource_usage_t usage = {.qp_count = 50, .mr_count = 100, .memory_used = 1024000};
    pid_t test_pid = 12345;
    
    // 预热缓存
    perf_optimizer_update_cached_resources(test_pid, &usage);
    
    // 缓存命中测试
    resource_usage_t read_usage;
    unsigned long long start = get_time_ns();
    for (int i = 0; i < NUM_OPS; i++) {
        perf_optimizer_get_cached_resources(test_pid, &read_usage);
    }
    unsigned long long end = get_time_ns();
    
    double avg_ns = (double)(end - start) / NUM_OPS;
    printf("CACHE_HIT_LATENCY: %.2f ns\n", avg_ns);
    
    // 获取统计
    perf_optimizer_stats_t stats;
    perf_optimizer_get_stats(&stats);
    printf("CACHE_HIT_RATE: %.2f%%\n", stats.cache_hit_rate);
    
    perf_optimizer_cleanup();
    return 0;
}
EOF
    
    gcc -o /tmp/benchmark_cache /tmp/benchmark_cache.c \
        -I$PROJECT_ROOT/include -I$PROJECT_ROOT/src \
        -L$BUILD_DIR -lperformance_optimizer -lshared_memory -lpthread -lrt
    
    /tmp/benchmark_cache > $RESULTS_DIR/latency/cache_performance.txt 2>&1
    cat $RESULTS_DIR/latency/cache_performance.txt
    
    echo "  ✓ 缓存性能测试完成"
}

# 实验3: 多租户可扩展性测试
benchmark_scalability() {
    echo ""
    echo "[实验3] 多租户可扩展性测试"
    
    cat > /tmp/benchmark_scalability.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <infiniband/verbs.h>

#define NUM_PROCESSES 10
#define QP_PER_PROCESS 10

static inline unsigned long long get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main() {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list || !dev_list[0]) {
        printf("未找到RDMA设备\n");
        return 1;
    }
    
    unsigned long long start = get_time_ns();
    
    for (int p = 0; p < NUM_PROCESSES; p++) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程
            struct ibv_context *ctx = ibv_open_device(dev_list[0]);
            struct ibv_pd *pd = ibv_alloc_pd(ctx);
            struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
            
            struct ibv_qp_init_attr qp_init_attr = {
                .qp_type = IBV_QPT_RC,
                .send_cq = cq,
                .recv_cq = cq,
                .cap = { .max_send_wr = 10, .max_recv_wr = 10, .max_send_sge = 1, .max_recv_sge = 1 }
            };
            
            int created = 0;
            for (int i = 0; i < QP_PER_PROCESS; i++) {
                struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
                if (qp) created++;
            }
            
            printf("Process %d created %d QPs\n", getpid(), created);
            exit(created == QP_PER_PROCESS ? 0 : 1);
        }
    }
    
    // 等待所有子进程
    int success = 0;
    for (int i = 0; i < NUM_PROCESSES; i++) {
        int status;
        wait(&status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) success++;
    }
    
    unsigned long long end = get_time_ns();
    double total_time_ms = (double)(end - start) / 1000000.0;
    
    printf("TOTAL_TIME: %.2f ms\n", total_time_ms);
    printf("SUCCESS_PROCESSES: %d/%d\n", success, NUM_PROCESSES);
    printf("THROUGHPUT: %.2f QPs/sec\n", NUM_PROCESSES * QP_PER_PROCESS * 1000.0 / total_time_ms);
    
    ibv_free_device_list(dev_list);
    return 0;
}
EOF
    
    gcc -o /tmp/benchmark_scalability /tmp/benchmark_scalability.c -libverbs -O2
    
    # 创建不同数量的租户进行测试
    for tenant_count in 1 5 10; do
        echo "  测试 $tenant_count 租户..."
        # 创建租户
        for i in $(seq 1 $tenant_count); do
            $BUILD_DIR/tenant_manager --create $((100+i)) --name "Tenant$i" --quota 100,1000,1024 2>/dev/null || true
        done
        
        export RDMA_INTERCEPT_ENABLE=1
        export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
        export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=10000
        export LD_PRELOAD=$BUILD_DIR/librdma_intercept.so
        
        /tmp/benchmark_scalability > $RESULTS_DIR/scalability/tenant_${tenant_count}.txt 2>&1
        unset LD_PRELOAD
        
        cat $RESULTS_DIR/scalability/tenant_${tenant_count}.txt
    done
    
    echo "  ✓ 可扩展性测试完成"
}

# 实验4: 资源隔离验证
benchmark_isolation() {
    echo ""
    echo "[实验4] 资源隔离验证"
    
    # 创建两个租户，不同配额
    $BUILD_DIR/tenant_manager --create 101 --name "SmallTenant" --quota 5,50,100 2>/dev/null || true
    $BUILD_DIR/tenant_manager --create 102 --name "LargeTenant" --quota 20,200,500 2>/dev/null || true
    
    cat > /tmp/benchmark_isolation.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main(int argc, char *argv[]) {
    int tenant_id = atoi(argv[1]);
    int max_expected = atoi(argv[2]);
    
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *ctx = ibv_open_device(dev_list[0]);
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    
    struct ibv_qp_init_attr qp_init_attr = {
        .qp_type = IBV_QPT_RC,
        .send_cq = cq,
        .recv_cq = cq,
        .cap = { .max_send_wr = 10, .max_recv_wr = 10, .max_send_sge = 1, .max_recv_sge = 1 }
    };
    
    int created = 0;
    int denied = 0;
    
    for (int i = 0; i < max_expected + 5; i++) {
        struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
        if (qp) {
            created++;
        } else {
            denied++;
            break;
        }
    }
    
    printf("TENANT_ID: %d\n", tenant_id);
    printf("EXPECTED_MAX: %d\n", max_expected);
    printf("CREATED: %d\n", created);
    printf("DENIED: %d\n", denied);
    printf("ISOLATION_CHECK: %s\n", (created <= max_expected) ? "PASS" : "FAIL");
    
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    
    return (created <= max_expected) ? 0 : 1;
}
EOF
    
    gcc -o /tmp/benchmark_isolation /tmp/benchmark_isolation.c -libverbs -O2
    
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export LD_PRELOAD=$BUILD_DIR/librdma_intercept.so
    
    echo "  测试小租户 (配额=5)..."
    export RDMA_TENANT_ID=101
    /tmp/benchmark_isolation 101 5 > $RESULTS_DIR/isolation/small_tenant.txt 2>&1
    cat $RESULTS_DIR/isolation/small_tenant.txt
    
    echo "  测试大租户 (配额=20)..."
    export RDMA_TENANT_ID=102
    /tmp/benchmark_isolation 102 20 > $RESULTS_DIR/isolation/large_tenant.txt 2>&1
    cat $RESULTS_DIR/isolation/large_tenant.txt
    
    unset LD_PRELOAD
    
    echo "  ✓ 资源隔离验证完成"
}

# 生成实验报告
generate_report() {
    echo ""
    echo "[报告生成] 汇总实验结果"
    
    cat > $RESULTS_DIR/summary.md << 'EOF'
# 实验结果汇总

## 1. 拦截开销
EOF
    
    echo "" >> $RESULTS_DIR/summary.md
    echo "### 基线性能 (无拦截)" >> $RESULTS_DIR/summary.md
    cat $RESULTS_DIR/latency/baseline.txt >> $RESULTS_DIR/summary.md 2>/dev/null || echo "N/A" >> $RESULTS_DIR/summary.md
    
    echo "" >> $RESULTS_DIR/summary.md
    echo "### 拦截后性能" >> $RESULTS_DIR/summary.md
    cat $RESULTS_DIR/latency/with_intercept.txt >> $RESULTS_DIR/summary.md 2>/dev/null || echo "N/A" >> $RESULTS_DIR/summary.md
    
    echo "" >> $RESULTS_DIR/summary.md
    echo "## 2. 缓存性能" >> $RESULTS_DIR/summary.md
    cat $RESULTS_DIR/latency/cache_performance.txt >> $RESULTS_DIR/summary.md 2>/dev/null || echo "N/A" >> $RESULTS_DIR/summary.md
    
    echo "" >> $RESULTS_DIR/summary.md
    echo "## 3. 可扩展性" >> $RESULTS_DIR/summary.md
    for f in $RESULTS_DIR/scalability/tenant_*.txt; do
        echo "" >> $RESULTS_DIR/summary.md
        echo "### $(basename $f)" >> $RESULTS_DIR/summary.md
        cat $f >> $RESULTS_DIR/summary.md 2>/dev/null || echo "N/A" >> $RESULTS_DIR/summary.md
    done
    
    echo "" >> $RESULTS_DIR/summary.md
    echo "## 4. 资源隔离" >> $RESULTS_DIR/summary.md
    for f in $RESULTS_DIR/isolation/*.txt; do
        echo "" >> $RESULTS_DIR/summary.md
        echo "### $(basename $f)" >> $RESULTS_DIR/summary.md
        cat $f >> $RESULTS_DIR/summary.md 2>/dev/null || echo "N/A" >> $RESULTS_DIR/summary.md
    done
    
    echo "  ✓ 实验报告已生成: $RESULTS_DIR/summary.md"
}

# 主函数
main() {
    check_rdma
    benchmark_overhead
    benchmark_cache
    benchmark_scalability
    benchmark_isolation
    generate_report
    
    echo ""
    echo "========================================"
    echo "   所有实验完成!"
    echo "   结果保存在: $RESULTS_DIR/"
    echo "========================================"
}

main "$@"
