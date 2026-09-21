#!/bin/bash
# <!-- created at: 2026-02-04 20:00:00 -->

echo "=== RDMA拦截系统功能验证测试 ==="
echo ""

# 清理现有进程
echo "1. 清理现有进程..."
sudo pkill -f rdma_monitor 2>/dev/null
sudo pkill -f collector_server 2>/dev/null

# 清空现有的eBPF映射数据
sudo rm -rf /sys/fs/bpf/process_resources /sys/fs/bpf/global_resources

echo ""
echo "2. 启动eBPF监控程序..."
sudo timeout 35s ./rdma_monitor &
MONITOR_PID=$!
sleep 3  # 等待监控程序启动

echo ""
echo "3. 启动collector_server (设置QP全局限制为3)..."
export RDMA_INTERCEPT_MAX_GLOBAL_QP=3
sudo -E timeout 35s ./collector_server &
COLLECTOR_PID=$!
sleep 3  # 等待collector_server启动

echo ""
echo "4. 初始状态检查:"
./test_collector_comm

echo ""
echo "5. 验证eBPF和collector_server协同工作..."

# 设置拦截库环境变量
export RDMA_INTERCEPT_ENABLE=1
export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=10  # 进程限制设为10，以便测试全局限制

# 创建一个测试程序来验证拦截功能
cat > /tmp/test_validation.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <unistd.h>

int main() {
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_context *context = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qp[5] = {NULL};  // 尝试创建5个QP
    int successful_creations = 0;

    // 获取设备列表
    device_list = ibv_get_device_list(NULL);
    if (!device_list) {
        fprintf(stderr, "无法获取IB设备列表\n");
        return 1;
    }

    // 选择第一个设备
    device = device_list[0];
    if (!device) {
        fprintf(stderr, "没有找到IB设备\n");
        ibv_free_device_list(device_list);
        return 1;
    }

    printf("使用设备: %s\n", device->name);

    // 打开设备上下文
    context = ibv_open_device(device);
    if (!context) {
        fprintf(stderr, "无法打开设备上下文\n");
        ibv_free_device_list(device_list);
        return 1;
    }

    // 分配保护域
    pd = ibv_alloc_pd(context);
    if (!pd) {
        fprintf(stderr, "无法分配保护域\n");
        ibv_close_device(context);
        ibv_free_device_list(device_list);
        return 1;
    }

    // 创建CQ
    cq = ibv_create_cq(context, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "无法创建CQ\n");
        goto cleanup;
    }

    // 创建QP - 测试全局限制
    struct ibv_qp_init_attr init_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 1,
            .max_recv_wr = 1,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };

    printf("开始创建QP，全局限制为3...\n");
    for (int i = 0; i < 5; i++) {
        printf("尝试创建QP #%d... ", i+1);
        qp[i] = ibv_create_qp(pd, &init_attr);
        if (qp[i]) {
            successful_creations++;
            printf("成功\n");
        } else {
            printf("失败 (预期: 达到全局限制)\n");
            break;  // 一旦失败就停止
        }
    }

    printf("\n总共成功创建了 %d 个QP\n", successful_creations);
    
    // 验证是否符合预期 (应该创建3个成功，第4个失败)
    if (successful_creations == 3) {
        printf("✅ 全局限制功能验证成功！(创建了3个QP，符合全局限制3)\n");
    } else {
        printf("❌ 全局限制功能验证失败！(期望创建3个QP，实际创建%d个)\n", successful_creations);
    }

cleanup:
    // 清理成功创建的QP
    for (int i = 0; i < successful_creations; i++) {
        if (qp[i]) {
            ibv_destroy_qp(qp[i]);
        }
    }
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (context) ibv_close_device(context);
    ibv_free_device_list(device_list);

    return 0;
}
EOF

# 编译测试程序
gcc -o /tmp/test_validation /tmp/test_validation.c -libverbs -lrdmacm

echo ""
echo "6. 运行拦截功能验证测试..."
LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH LD_PRELOAD=./librdma_intercept.so /tmp/test_validation

echo ""
echo "7. 测试后状态检查:"
./test_collector_comm

# 结束进程
echo ""
echo "8. 清理进程..."
sudo kill $MONITOR_PID $COLLECTOR_PID 2>/dev/null || true

echo ""
echo "=== 功能验证测试完成 ==="