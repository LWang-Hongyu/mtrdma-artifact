#!/bin/bash
# QP拦截功能实际测试脚本

set -e

echo "========================================"
echo "   RDMA QP拦截功能实际测试"
echo "========================================"

cd /opt/mtrdma/build

# 编译QP创建测试程序
cat > /tmp/test_qp_create.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int expected_max = (argc > 1) ? atoi(argv[1]) : 3;
    
    printf("尝试创建QP，期望最多成功%d个\n", expected_max);
    
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
    
    int created = 0;
    int failed = 0;
    
    // 尝试创建 expected_max + 2 个QP
    for (int i = 0; i < expected_max + 2; i++) {
        struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
        if (qp) {
            created++;
            printf("  [CREATED] QP %d\n", i+1);
        } else {
            failed++;
            printf("  [DENIED]  QP %d (被拦截，符合预期)\n", i+1);
            break;
        }
    }
    
    printf("\n结果: 成功创建 %d 个QP，被拦截 %d 次\n", created, failed);
    
    if (created <= expected_max) {
        printf("✓ 拦截功能正常: 创建数(%d) <= 限制(%d)\n", created, expected_max);
    } else {
        printf("✗ 拦截失效: 创建数(%d) > 限制(%d)\n", created, expected_max);
    }
    
    // 清理 - 需要销毁创建的QP
    // 这里简化处理，实际应该跟踪所有QP
    
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    
    return (created <= expected_max) ? 0 : 1;
}
EOF

gcc -o /tmp/test_qp_create /tmp/test_qp_create.c -libverbs

echo ""
echo "--- 测试1: 无拦截（基线）---"
/tmp/test_qp_create 3 || true

echo ""
echo "--- 测试2: 带拦截，租户10，QP限制=3 ---"
export RDMA_INTERCEPT_ENABLE=1
export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=100  # 设置一个较大的值，让租户限制起作用
export RDMA_TENANT_ID=10
export LD_PRELOAD=$PWD/librdma_intercept.so

/tmp/test_qp_create 3 || true

echo ""
echo "========================================"
echo "   测试完成"
echo "========================================"
