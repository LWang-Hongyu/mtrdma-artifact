#!/bin/bash

# 测试QP生命周期管理：创建-注销-再创建

# 清理之前的日志
rm -f /tmp/rdma_intercept.log

echo "======================================"
echo "测试QP生命周期管理"
echo "======================================"
echo "1. 计划：创建5个QP，注销5个QP，再创建5个QP"
echo "2. 预期：总QP数量不超过5，不应触发拦截"
echo "======================================"

# 设置环境变量
export RDMA_INTERCEPT_ENABLE=1
export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
export RDMA_INTERCEPT_MAX_GLOBAL_QP=10
export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=5
export LD_PRELOAD=/opt/mtrdma/build/librdma_intercept.so

# 编写测试程序
cat > test_qp_lifecycle.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main() {
    struct ibv_device **dev_list;
    struct ibv_device *dev;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_qp *qps[10];
    int num_devices;
    int i;
    int ret;

    printf("=== QP生命周期测试 ===\n");

    // 获取设备列表
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        fprintf(stderr, "无法获取RDMA设备列表\n");
        return 1;
    }

    if (num_devices == 0) {
        fprintf(stderr, "未找到RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }

    dev = dev_list[0];
    printf("找到RDMA设备: %s\n", ibv_get_device_name(dev));

    // 打开设备
    ctx = ibv_open_device(dev);
    if (!ctx) {
        fprintf(stderr, "无法打开RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }

    // 分配PD
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "无法分配PD\n");
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    printf("分配PD成功\n");

    // 创建CQ
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "无法创建CQ\n");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    printf("创建CQ成功\n");

    // 初始化QP属性
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 16;
    qp_init_attr.cap.max_recv_wr = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    // 第一阶段：创建5个QP
    printf("\n=== 第一阶段：创建5个QP ===\n");
    for (i = 0; i < 5; i++) {
        qps[i] = ibv_create_qp(pd, &qp_init_attr);
        if (!qps[i]) {
            fprintf(stderr, "创建QP %d 失败: %s\n", i+1, strerror(errno));
            // 清理已创建的QP
            for (int j = 0; j < i; j++) {
                if (qps[j]) {
                    ibv_destroy_qp(qps[j]);
                }
            }
            ibv_destroy_cq(cq);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            ibv_free_device_list(dev_list);
            return 1;
        }
        printf("  ✓ QP %d 创建成功: %p\n", i+1, qps[i]);
    }

    // 第二阶段：注销5个QP
    printf("\n=== 第二阶段：注销5个QP ===\n");
    for (i = 0; i < 5; i++) {
        ret = ibv_destroy_qp(qps[i]);
        if (ret) {
            fprintf(stderr, "注销QP %d 失败: %s\n", i+1, strerror(errno));
            continue;
        }
        printf("  ✓ QP %d 注销成功\n", i+1);
        qps[i] = NULL;
    }

    // 第三阶段：再创建5个QP
    printf("\n=== 第三阶段：再创建5个QP ===\n");
    for (i = 0; i < 5; i++) {
        qps[i] = ibv_create_qp(pd, &qp_init_attr);
        if (!qps[i]) {
            fprintf(stderr, "  ✗ QP %d 创建失败: %s\n", i+1, strerror(errno));
            // 清理已创建的QP
            for (int j = 0; j < i; j++) {
                if (qps[j]) {
                    ibv_destroy_qp(qps[j]);
                }
            }
            ibv_destroy_cq(cq);
            ibv_dealloc_pd(pd);
            ibv_close_device(ctx);
            ibv_free_device_list(dev_list);
            return 1;
        }
        printf("  ✓ QP %d 创建成功: %p\n", i+1, qps[i]);
    }

    // 清理所有QP
    printf("\n=== 清理所有QP ===\n");
    for (i = 0; i < 5; i++) {
        if (qps[i]) {
            ret = ibv_destroy_qp(qps[i]);
            if (ret) {
                fprintf(stderr, "  ✗ QP %d 注销失败: %s\n", i+1, strerror(errno));
            } else {
                printf("  ✓ QP %d 注销成功\n", i+1);
            }
        }
    }

    // 清理资源
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);

    printf("\n=== 测试完成 ===\n");
    return 0;
}
EOF

# 编译测试程序
gcc -o test_qp_lifecycle test_qp_lifecycle.c -libverbs

if [ $? -ne 0 ]; then
    echo "编译失败"
    rm -f test_qp_lifecycle.c
    exit 1
fi

# 运行测试程序
echo "\n运行测试程序..."
./test_qp_lifecycle

test_result=$?

# 分析日志
echo "\n======================================"
echo "分析日志..."
echo "======================================"

# 检查是否有拦截错误
intercept_errors=$(grep -c "QP creation denied" /tmp/rdma_intercept.log)

# 检查QP计数
qp_created=$(grep -c "QP created successfully" /tmp/rdma_intercept.log)
qp_destroyed=$(grep -c "QP destroyed successfully" /tmp/rdma_intercept.log)

# 检查最大QP计数
max_qp_count=0
while read line; do
    if [[ $line =~ "current count: ([0-9]+)" ]]; then
        count=${BASH_REMATCH[1]}
        if (( count > max_qp_count )); then
            max_qp_count=$count
        fi
    fi
done < /tmp/rdma_intercept.log

echo "日志分析结果:"
echo "- QP创建次数: $qp_created"
echo "- QP注销次数: $qp_destroyed"
echo "- 最大QP计数: $max_qp_count"
echo "- 拦截错误次数: $intercept_errors"

echo "\n======================================"
if [ $test_result -eq 0 ] && [ $intercept_errors -eq 0 ] && [ $max_qp_count -le 5 ]; then
    echo "✅ 测试通过!"
    echo "- 所有QP操作成功完成"
    echo "- 未触发拦截"
    echo "- 最大QP计数: $max_qp_count (≤ 5)"
else
    echo "❌ 测试失败!"
    if [ $test_result -ne 0 ]; then
        echo "- 测试程序执行失败"
    fi
    if [ $intercept_errors -gt 0 ]; then
        echo "- 触发了拦截错误: $intercept_errors 次"
    fi
    if [ $max_qp_count -gt 5 ]; then
        echo "- 最大QP计数: $max_qp_count (> 5)"
    fi
fi
echo "======================================"

# 清理临时文件
rm -f test_qp_lifecycle test_qp_lifecycle.c

# 清理环境变量
unset RDMA_INTERCEPT_ENABLE
unset RDMA_INTERCEPT_ENABLE_QP_CONTROL
unset RDMA_INTERCEPT_MAX_GLOBAL_QP
unset RDMA_INTERCEPT_MAX_QP_PER_PROCESS
unset LD_PRELOAD

echo "测试完成！"
