#!/bin/bash
# <!-- created at: 2026-01-27 16:00:00 -->
# 实际RDMA QP创建拦截测试脚本
# 依赖: librdma_intercept.so, ibverbs库
# 运行方式: bash test_real_qp_intercept.sh

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 测试配置
INTERCEPT_LIB="/opt/mtrdma/build/librdma_intercept.so"
LOG_FILE="/tmp/rdma_intercept.log"
TEST_DURATION=30  # 测试持续时间（秒）
MAX_GLOBAL_QP=5  # 全局QP上限，设置较小值便于测试拦截

# 清理函数
cleanup() {
    echo "清理测试环境..."
    pkill -f "test_real_qp_create" || true
    rm -f /tmp/test_real_qp_create /tmp/test_output.log
    # 清理环境变量
    unset RDMA_INTERCEPT_ENABLE
    unset LD_PRELOAD
    unset RDMA_INTERCEPT_MAX_GLOBAL_QP
}

# 错误处理
error_exit() {
    echo -e "${RED}测试失败: $1${NC}"
    cleanup
    exit 1
}

# 检查文件存在
check_files() {
    if [[ ! -f "$INTERCEPT_LIB" ]]; then
        error_exit "拦截库缺失: $INTERCEPT_LIB，请先运行 'make' 编译项目"
    fi
    
    if ! command -v gcc &> /dev/null; then
        error_exit "gcc未安装，请先安装gcc"
    fi
}

# 编译测试程序
compile_test_program() {
    echo -e "${YELLOW}编译测试程序...${NC}"
    cat > /tmp/test_real_qp_create.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <string.h>

int main() {
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_qp_init_attr qp_init_attr = {
        .qp_type = IBV_QPT_RC,
        .send_cq = NULL,
        .recv_cq = NULL,
        .srq = NULL,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .sq_sig_all = 1,
    };
    int i, num_qp = 10;
    
    printf("=== RDMA QP创建测试 ===\n");
    
    // 获取设备列表
    printf("获取RDMA设备列表...\n");
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "错误: 无法获取RDMA设备列表: %s\n", strerror(errno));
        return 1;
    }
    
    if (!dev_list[0]) {
        fprintf(stderr, "错误: 未找到RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    printf("找到RDMA设备: %s\n", ibv_get_device_name(dev_list[0]));
    
    // 打开设备
    printf("打开RDMA设备...\n");
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "错误: 无法打开RDMA设备: %s\n", strerror(errno));
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    // 分配PD
    printf("分配保护域(PD)...\n");
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "错误: 无法分配PD: %s\n", strerror(errno));
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    // 创建CQ
    printf("创建完成队列(CQ)...\n");
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "错误: 无法创建CQ: %s\n", strerror(errno));
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    
    // 尝试创建多个QP
    printf("\n尝试创建 %d 个QP...\n", num_qp);
    for (i = 0; i < num_qp; i++) {
        printf("尝试创建QP %d...\n", i+1);
        qp = ibv_create_qp(pd, &qp_init_attr);
        if (qp) {
            printf("  ✓ QP %d 创建成功: %p\n", i+1, qp);
        } else {
            printf("  ✗ QP %d 创建失败: %s\n", i+1, strerror(errno));
        }
    }
    
    // 清理
    printf("\n清理资源...\n");
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    
    printf("测试完成\n");
    return 0;
}
EOF
    
    gcc -o /tmp/test_real_qp_create /tmp/test_real_qp_create.c -libverbs
    if [[ $? -ne 0 ]]; then
        error_exit "编译测试程序失败"
    fi
    echo -e "${GREEN}编译测试程序成功${NC}"
}

# 测试1: 实际QP创建拦截功能
test_real_qp_intercept() {
    echo -e "${YELLOW}=== 测试: 实际RDMA QP创建拦截 ===${NC}"
    
    # 清理日志
    > "$LOG_FILE"
    
    # 设置环境变量启用拦截和全局QP限制
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export LD_PRELOAD="$INTERCEPT_LIB"
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$MAX_GLOBAL_QP"
    export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=5
    
    echo "启动数据收集服务..."
    # 确保collector_server正在运行
    if ! pgrep -f "collector_server" > /dev/null; then
        echo "启动collector_server..."
        /opt/mtrdma/build/collector_server &
        sleep 2
    fi
    
    echo "运行测试程序，尝试创建10个QP..."
    echo "全局QP上限设置为: $MAX_GLOBAL_QP"
    
    # 运行测试程序
    echo "启用LD_PRELOAD后运行测试程序..."
    echo "环境变量:"
    echo "  RDMA_INTERCEPT_ENABLE: $RDMA_INTERCEPT_ENABLE"
    echo "  RDMA_INTERCEPT_ENABLE_QP_CONTROL: $RDMA_INTERCEPT_ENABLE_QP_CONTROL"
    echo "  LD_PRELOAD: $LD_PRELOAD"
    echo "  RDMA_INTERCEPT_MAX_GLOBAL_QP: $RDMA_INTERCEPT_MAX_GLOBAL_QP"
    echo "  RDMA_INTERCEPT_MAX_QP_PER_PROCESS: $RDMA_INTERCEPT_MAX_QP_PER_PROCESS"
    
    # 不使用timeout，直接运行测试程序
    echo "直接运行测试程序..."
    /tmp/test_real_qp_create > /tmp/test_output.log 2>&1
    
    echo "测试程序返回码: $?"
    echo "测试程序完整输出:"
    cat /tmp/test_output.log
    echo "拦截器完整日志:"
    cat /tmp/rdma_intercept.log
    
    echo "测试程序返回码: $?"
    echo "测试程序标准输出:"
    cat /tmp/test_output.log
    echo "测试程序标准错误:"
    cat /tmp/rdma_intercept.log
    
    # 检查测试输出
    echo -e "${YELLOW}测试程序输出:${NC}"
    cat /tmp/test_output.log
    echo
    
    # 检查日志内容
    echo -e "${YELLOW}拦截器日志:${NC}"
    grep -E "(QP creation denied|QP created successfully|全局QP上限已达到)" "$LOG_FILE" || true
    echo
    
    # 分析结果
    local success_count=$(grep -c "✓ QP" /tmp/test_output.log)
    local failure_count=$(grep -c "✗ QP" /tmp/test_output.log)
    
    echo -e "${YELLOW}测试结果分析:${NC}"
    echo -e "成功创建的QP数量: ${GREEN}$success_count${NC}"
    echo -e "失败创建的QP数量: ${RED}$failure_count${NC}"
    
    # 检查是否达到拦截效果
    if [[ $success_count -le $MAX_GLOBAL_QP && $failure_count -gt 0 ]]; then
        echo -e "${GREEN}✓ 拦截功能测试通过!${NC}"
        echo -e "  成功创建了 $success_count 个QP，达到上限后开始拒绝"
        return 0
    else
        echo -e "${RED}✗ 拦截功能测试失败${NC}"
        echo -e "  预期: 成功创建不超过 $MAX_GLOBAL_QP 个QP，后续创建失败"
        echo -e "  实际: 成功创建 $success_count 个QP，失败 $failure_count 个"
        return 1
    fi
}

# 测试2: 边界情况测试 - 0个QP创建请求
test_zero_qp_request() {
    echo -e "${YELLOW}=== 测试: 边界情况 - 0个QP创建请求 ===${NC}"
    
    # 清理日志
    > "$LOG_FILE"
    
    # 设置环境变量启用拦截和全局QP限制
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export LD_PRELOAD="$INTERCEPT_LIB"
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$MAX_GLOBAL_QP"
    export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=5
    
    # 编译测试程序
    cat > /tmp/test_zero_qp_create.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <string.h>

int main() {
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp_init_attr qp_init_attr = {
        .qp_type = IBV_QPT_RC,
        .send_cq = NULL,
        .recv_cq = NULL,
        .srq = NULL,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .sq_sig_all = 1,
    };
    
    printf("=== 0个QP创建测试 ===\n");
    
    // 获取设备列表
    printf("获取RDMA设备列表...\n");
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "错误: 无法获取RDMA设备列表: %s\n", strerror(errno));
        return 1;
    }
    
    if (!dev_list[0]) {
        fprintf(stderr, "错误: 未找到RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    printf("找到RDMA设备: %s\n", ibv_get_device_name(dev_list[0]));
    
    // 打开设备
    printf("打开RDMA设备...\n");
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "错误: 无法打开RDMA设备: %s\n", strerror(errno));
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    // 分配PD
    printf("分配保护域(PD)...\n");
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "错误: 无法分配PD: %s\n", strerror(errno));
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    // 创建CQ
    printf("创建完成队列(CQ)...\n");
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "错误: 无法创建CQ: %s\n", strerror(errno));
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    
    // 不创建任何QP
    printf("不创建任何QP...\n");
    
    // 清理
    printf("\n清理资源...\n");
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    
    printf("测试完成\n");
    return 0;
}
EOF
    
    gcc -o /tmp/test_zero_qp_create /tmp/test_zero_qp_create.c -libverbs
    if [[ $? -ne 0 ]]; then
        error_exit "编译测试程序失败"
    fi
    
    # 运行测试程序
    /tmp/test_zero_qp_create > /tmp/test_output.log 2>&1
    
    # 检查测试输出
    echo -e "${YELLOW}测试程序输出:${NC}"
    cat /tmp/test_output.log
    echo
    
    # 检查日志内容
    echo -e "${YELLOW}拦截器日志:${NC}"
    grep -E "(QP creation denied|QP created successfully)" "$LOG_FILE" || true
    echo
    
    # 分析结果
    local qp_creation_count=$(grep -c "QP created successfully" "$LOG_FILE")
    
    echo -e "${YELLOW}测试结果分析:${NC}"
    echo -e "成功创建的QP数量: ${GREEN}$qp_creation_count${NC}"
    
    # 检查是否没有创建任何QP
    if [[ $qp_creation_count -eq 0 ]]; then
        echo -e "${GREEN}✓ 0个QP创建测试通过!${NC}"
        echo -e "  没有创建任何QP，符合预期"
        return 0
    else
        echo -e "${RED}✗ 0个QP创建测试失败${NC}"
        echo -e "  预期: 没有创建任何QP"
        echo -e "  实际: 成功创建 $qp_creation_count 个QP"
        return 1
    fi
}

# 测试3: 边界情况测试 - 不同QP类型的创建请求
test_different_qp_types() {
    echo -e "${YELLOW}=== 测试: 边界情况 - 不同QP类型的创建请求 ===${NC}"
    
    # 清理日志
    > "$LOG_FILE"
    
    # 设置环境变量启用拦截和全局QP限制
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export LD_PRELOAD="$INTERCEPT_LIB"
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$MAX_GLOBAL_QP"
    export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=5
    
    # 编译测试程序
    cat > /tmp/test_different_qp_types.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <string.h>

int main() {
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = NULL,
        .recv_cq = NULL,
        .srq = NULL,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .sq_sig_all = 1,
    };
    int qp_types[] = {IBV_QPT_RC, IBV_QPT_UC, IBV_QPT_UD};
    const char *qp_type_names[] = {"RC", "UC", "UD"};
    int num_types = sizeof(qp_types) / sizeof(qp_types[0]);
    int i;
    
    printf("=== 不同QP类型创建测试 ===\n");
    
    // 获取设备列表
    printf("获取RDMA设备列表...\n");
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "错误: 无法获取RDMA设备列表: %s\n", strerror(errno));
        return 1;
    }
    
    if (!dev_list[0]) {
        fprintf(stderr, "错误: 未找到RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    printf("找到RDMA设备: %s\n", ibv_get_device_name(dev_list[0]));
    
    // 打开设备
    printf("打开RDMA设备...\n");
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "错误: 无法打开RDMA设备: %s\n", strerror(errno));
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    // 分配PD
    printf("分配保护域(PD)...\n");
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "错误: 无法分配PD: %s\n", strerror(errno));
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    // 创建CQ
    printf("创建完成队列(CQ)...\n");
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "错误: 无法创建CQ: %s\n", strerror(errno));
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    
    // 尝试创建不同类型的QP
    printf("\n尝试创建不同类型的QP...\n");
    for (i = 0; i < num_types; i++) {
        qp_init_attr.qp_type = qp_types[i];
        printf("尝试创建%s类型QP...\n", qp_type_names[i]);
        qp = ibv_create_qp(pd, &qp_init_attr);
        if (qp) {
            printf("  ✓ %s类型QP创建成功: %p\n", qp_type_names[i], qp);
        } else {
            printf("  ✗ %s类型QP创建失败: %s\n", qp_type_names[i], strerror(errno));
        }
    }
    
    // 清理
    printf("\n清理资源...\n");
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    
    printf("测试完成\n");
    return 0;
}
EOF
    
    gcc -o /tmp/test_different_qp_types /tmp/test_different_qp_types.c -libverbs
    if [[ $? -ne 0 ]]; then
        error_exit "编译测试程序失败"
    fi
    
    # 运行测试程序
    /tmp/test_different_qp_types > /tmp/test_output.log 2>&1
    
    # 检查测试输出
    echo -e "${YELLOW}测试程序输出:${NC}"
    cat /tmp/test_output.log
    echo
    
    # 检查日志内容
    echo -e "${YELLOW}拦截器日志:${NC}"
    grep -E "(QP creation denied|QP created successfully)" "$LOG_FILE" || true
    echo
    
    # 分析结果
    local qp_creation_count=$(grep -c "QP created successfully" "$LOG_FILE")
    
    echo -e "${YELLOW}测试结果分析:${NC}"
    echo -e "成功创建的QP数量: ${GREEN}$qp_creation_count${NC}"
    
    # 检查是否创建了至少一个QP
    if [[ $qp_creation_count -gt 0 ]]; then
        echo -e "${GREEN}✓ 不同QP类型创建测试通过!${NC}"
        echo -e "  成功创建 $qp_creation_count 个不同类型的QP"
        return 0
    else
        echo -e "${RED}✗ 不同QP类型创建测试失败${NC}"
        echo -e "  预期: 成功创建至少一个不同类型的QP"
        echo -e "  实际: 成功创建 $qp_creation_count 个QP"
        return 1
    fi
}

# 主测试函数
main() {
    echo -e "${YELLOW}实际RDMA QP创建拦截测试${NC}"
    echo "======================================"
    
    # 注册清理函数
    trap cleanup EXIT
    
    # 检查文件
    check_files
    
    # 编译测试程序
    compile_test_program
    
    # 运行测试
    local all_passed=true
    
    echo "运行测试1: 实际QP创建拦截功能"
    if ! test_real_qp_intercept; then
        all_passed=false
    fi
    
    echo "\n运行测试2: 边界情况 - 0个QP创建请求"
    if ! test_zero_qp_request; then
        all_passed=false
    fi
    
    echo "\n运行测试3: 边界情况 - 不同QP类型的创建请求"
    if ! test_different_qp_types; then
        all_passed=false
    fi
    
    echo "======================================"
    if $all_passed; then
        echo -e "${GREEN}所有测试通过!${NC}"
        echo -e "${YELLOW}总结:${NC}"
        echo -e "  - LD_PRELOAD拦截库能够成功拦截真实的RDMA QP创建"
        echo -e "  - 当达到全局QP上限时，会拒绝新的QP创建请求"
        echo -e "  - 拦截功能正常工作"
        echo -e "  - 边界情况测试通过"
        exit 0
    else
        echo -e "${RED}测试失败${NC}"
        echo -e "${YELLOW}可能的原因:${NC}"
        echo -e "  - RDMA设备未就绪或不可用"
        echo -e "  - collector_server未正常运行"
        echo -e "  - 权限问题导致无法加载eBPF程序"
        echo -e "  - 拦截逻辑存在问题"
        exit 1
    fi
}

# 运行主函数
main "$@"
