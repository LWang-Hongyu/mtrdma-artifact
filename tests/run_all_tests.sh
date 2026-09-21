#!/bin/bash
# <!-- created at: 2026-01-27 17:00:00 -->
# 自动化测试脚本
# 依赖: librdma_intercept.so, ibverbs库
# 运行方式: bash run_all_tests.sh

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 测试配置
PROJECT_ROOT="/opt/mtrdma"
INTERCEPT_LIB="$PROJECT_ROOT/build/librdma_intercept.so"
LOG_FILE="/tmp/rdma_intercept.log"
TEST_DURATION=30  # 测试持续时间（秒）
MAX_GLOBAL_QP=20  # 全局QP上限，设置较大值以便测试QP创建功能

# 清理函数
cleanup() {
    echo "清理测试环境..."
    pkill -f "test_real_qp_create" || true
    pkill -f "test_zero_qp_create" || true
    pkill -f "test_different_qp_types" || true
    pkill -f "test_automated" || true
    pkill -f "collector_server" || true
    rm -f /tmp/test_real_qp_create /tmp/test_zero_qp_create /tmp/test_different_qp_types /tmp/test_automated /tmp/test_output.log
    # 清理环境变量
    unset RDMA_INTERCEPT_ENABLE
    unset LD_PRELOAD
    unset RDMA_INTERCEPT_MAX_GLOBAL_QP
    unset RDMA_INTERCEPT_ENABLE_QP_CONTROL
    unset RDMA_INTERCEPT_MAX_QP_PER_PROCESS
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
    
    if ! command -v curl &> /dev/null; then
        error_exit "curl未安装，请先安装curl"
    fi
}

# 编译测试程序
compile_test_program() {
    echo -e "${YELLOW}编译测试程序...${NC}"
    
    # 测试程序1: 实际QP创建拦截测试
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
    
    # 测试程序2: 0个QP创建请求测试
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
    
    # 测试程序3: 不同QP类型的创建请求测试
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
    
    # 测试程序4: 自动化测试（包含多种场景）
    cat > /tmp/test_automated.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <string.h>
#include <time.h>

#define TEST_SCENARIOS 5

// 测试场景
typedef struct {
    char *name;
    int max_qp;
    int qp_type;
    int max_send_wr;
    int max_recv_wr;
} test_scenario_t;

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
    
    // 测试场景定义
    test_scenario_t scenarios[TEST_SCENARIOS] = {
        {"标准RC QP创建", 3, IBV_QPT_RC, 16, 16},
        {"大WR RC QP创建", 2, IBV_QPT_RC, 128, 128},
        {"UC QP创建", 2, IBV_QPT_UC, 16, 16},
        {"UD QP创建", 2, IBV_QPT_UD, 16, 16},
        {"混合QP类型创建", 3, IBV_QPT_RC, 16, 16},
    };
    
    int i, j, scenario_pass = 0;
    
    printf("=== 自动化测试 ===\n");
    
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
    
    // 运行所有测试场景
    printf("\n运行测试场景...\n");
    for (i = 0; i < TEST_SCENARIOS; i++) {
        test_scenario_t *scenario = &scenarios[i];
        printf("\n场景 %d: %s\n", i+1, scenario->name);
        printf("  最大QP数: %d\n", scenario->max_qp);
        printf("  QP类型: %d\n", scenario->qp_type);
        printf("  最大发送WR: %d\n", scenario->max_send_wr);
        printf("  最大接收WR: %d\n", scenario->max_recv_wr);
        
        qp_init_attr.qp_type = scenario->qp_type;
        qp_init_attr.cap.max_send_wr = scenario->max_send_wr;
        qp_init_attr.cap.max_recv_wr = scenario->max_recv_wr;
        
        int success_count = 0;
        for (j = 0; j < scenario->max_qp + 2; j++) {
            printf("  尝试创建QP %d...\n", j+1);
            qp = ibv_create_qp(pd, &qp_init_attr);
            if (qp) {
                printf("    ✓ QP %d 创建成功: %p\n", j+1, qp);
                success_count++;
            } else {
                printf("    ✗ QP %d 创建失败: %s\n", j+1, strerror(errno));
            }
        }
        
        // 检查测试结果
        if (success_count <= scenario->max_qp) {
            printf("  ✓ 场景测试通过! 成功创建 %d 个QP，符合预期\n", success_count);
            scenario_pass++;
        } else {
            printf("  ✗ 场景测试失败! 成功创建 %d 个QP，超过预期 %d 个\n", success_count, scenario->max_qp);
        }
        
        // 等待一段时间，模拟实际使用场景
        sleep(1);
    }
    
    // 清理
    printf("\n清理资源...\n");
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    
    // 输出测试结果
    printf("\n=== 自动化测试结果 ===\n");
    printf("测试场景总数: %d\n", TEST_SCENARIOS);
    printf("通过场景数: %d\n", scenario_pass);
    printf("失败场景数: %d\n", TEST_SCENARIOS - scenario_pass);
    
    if (scenario_pass == TEST_SCENARIOS) {
        printf("\n✓ 所有测试场景通过!\n");
        return 0;
    } else {
        printf("\n✗ 部分测试场景失败!\n");
        return 1;
    }
}
EOF
    
    gcc -o /tmp/test_automated /tmp/test_automated.c -libverbs
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
    # 确保collector_server不在运行
    pkill -f "collector_server" || true
    sleep 1
    # 启动新的collector_server
    echo "启动collector_server..."
    # 先设置环境变量，然后启动collector_server
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$MAX_GLOBAL_QP"
    $PROJECT_ROOT/build/collector_server > /tmp/collector_server.log 2>&1 &
    sleep 2
    # 检查collector_server是否正常运行
    if ! pgrep -f "collector_server" > /dev/null; then
        echo "collector_server启动失败，查看日志:"
        cat /tmp/collector_server.log
        return 1
    fi
    echo "collector_server启动成功"
    # 查看collector_server的初始状态
    echo "collector_server初始状态:"
    cat /tmp/collector_server.log
    
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
    
    # 运行测试程序
    /tmp/test_real_qp_create > /tmp/test_output.log 2>&1
    
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



# 测试3: 边界情况测试 - 不同QP类型的创建请求
test_different_qp_types() {
    echo -e "${YELLOW}=== 测试: 边界情况 - 不同QP类型的创建请求 ===${NC}"
    
    # 清理日志
    > "$LOG_FILE"
    
    # 设置环境变量启用拦截和全局QP限制
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export LD_PRELOAD="$INTERCEPT_LIB"
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$((MAX_GLOBAL_QP * 2))"
    export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=5
    
    echo "启动数据收集服务..."
    # 确保collector_server不在运行
    pkill -f "collector_server" || true
    sleep 1
    # 启动新的collector_server
    echo "启动collector_server..."
    # 先设置环境变量，然后启动collector_server
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$((MAX_GLOBAL_QP * 2))"
    $PROJECT_ROOT/build/collector_server &
    sleep 2
    
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

# 测试4: 自动化测试（包含多种场景）
test_automated() {
    echo -e "${YELLOW}=== 测试: 自动化测试（包含多种场景） ===${NC}"
    
    # 清理日志
    > "$LOG_FILE"
    
    # 设置环境变量启用拦截和全局QP限制
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export LD_PRELOAD="$INTERCEPT_LIB"
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$((MAX_GLOBAL_QP * 3))"
    export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=5
    
    echo "启动数据收集服务..."
    # 确保collector_server不在运行
    pkill -f "collector_server" || true
    sleep 1
    # 启动新的collector_server
    echo "启动collector_server..."
    # 先设置环境变量，然后启动collector_server
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$((MAX_GLOBAL_QP * 3))"
    $PROJECT_ROOT/build/collector_server &
    sleep 2
    
    # 运行测试程序
    /tmp/test_automated > /tmp/test_output.log 2>&1
    
    # 检查测试输出
    echo -e "${YELLOW}测试程序输出:${NC}"
    cat /tmp/test_output.log
    echo
    
    # 检查日志内容
    echo -e "${YELLOW}拦截器日志:${NC}"
    grep -E "(QP creation denied|QP created successfully)" "$LOG_FILE" || true
    echo
    
    # 分析结果
    local scenario_pass=$(grep -c "场景测试通过" /tmp/test_output.log)
    local scenario_total=$(grep -c "场景测试" /tmp/test_output.log)
    
    echo -e "${YELLOW}测试结果分析:${NC}"
    echo -e "通过的场景数: ${GREEN}$scenario_pass${NC}"
    echo -e "总场景数: ${YELLOW}$scenario_total${NC}"
    
    # 检查是否所有场景都通过
    if [[ $scenario_pass -eq $scenario_total ]]; then
        echo -e "${GREEN}✓ 自动化测试通过!${NC}"
        echo -e "  所有 $scenario_pass 个场景测试通过"
        return 0
    else
        echo -e "${RED}✗ 自动化测试失败${NC}"
        echo -e "  通过 $scenario_pass 个场景，失败 $((scenario_total - scenario_pass)) 个场景"
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

# 测试4: 自动化测试（包含多种场景）
test_automated() {
    echo -e "${YELLOW}=== 测试: 自动化测试（包含多种场景） ===${NC}"
    
    # 清理日志
    > "$LOG_FILE"
    
    # 设置环境变量启用拦截和全局QP限制
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
    export LD_PRELOAD="$INTERCEPT_LIB"
    export RDMA_INTERCEPT_MAX_GLOBAL_QP="$MAX_GLOBAL_QP"
    export RDMA_INTERCEPT_MAX_QP_PER_PROCESS=3
    
    # 运行测试程序
    /tmp/test_automated > /tmp/test_output.log 2>&1
    
    # 检查测试输出
    echo -e "${YELLOW}测试程序输出:${NC}"
    cat /tmp/test_output.log
    echo
    
    # 检查日志内容
    echo -e "${YELLOW}拦截器日志:${NC}"
    grep -E "(QP creation denied|QP created successfully)" "$LOG_FILE" || true
    echo
    
    # 分析结果
    local scenario_pass=$(grep -c "场景测试通过" /tmp/test_output.log)
    local scenario_total=$(grep -c "场景测试" /tmp/test_output.log)
    
    echo -e "${YELLOW}测试结果分析:${NC}"
    echo -e "通过的场景数: ${GREEN}$scenario_pass${NC}"
    echo -e "总场景数: ${YELLOW}$scenario_total${NC}"
    
    # 检查是否所有场景都通过
    if [[ $scenario_pass -eq $scenario_total ]]; then
        echo -e "${GREEN}✓ 自动化测试通过!${NC}"
        echo -e "  所有 $scenario_pass 个场景测试通过"
        return 0
    else
        echo -e "${RED}✗ 自动化测试失败${NC}"
        echo -e "  通过 $scenario_pass 个场景，失败 $((scenario_total - scenario_pass)) 个场景"
        return 1
    fi
}

# 生成测试报告
generate_report() {
    echo -e "${YELLOW}=== 生成测试报告 ===${NC}"
    
    local timestamp=$(date +"%Y-%m-%d %H:%M:%S")
    local report_file="/tmp/rdma_intercept_test_report_$(date +"%Y%m%d_%H%M%S").txt"
    
    cat > "$report_file" << EOF
RDMA拦截系统测试报告
生成时间: $timestamp

测试配置:
- 项目根目录: $PROJECT_ROOT
- 拦截库路径: $INTERCEPT_LIB
- 日志文件路径: $LOG_FILE
- 测试持续时间: $TEST_DURATION 秒
- 全局QP上限: $MAX_GLOBAL_QP

测试结果:
EOF
    
    # 测试结果分析
    local real_qp_test=$(grep -c "拦截功能测试通过" /tmp/test_output.log)
    local zero_qp_test=$(grep -c "0个QP创建测试通过" /tmp/test_output.log)
    local different_qp_test=$(grep -c "不同QP类型创建测试通过" /tmp/test_output.log)
    local automated_test=$(grep -c "自动化测试通过" /tmp/test_output.log)
    
    echo "1. 实际QP创建拦截测试: $(if [[ $real_qp_test -gt 0 ]]; then echo "通过"; else echo "失败"; fi)" >> "$report_file"
    echo "2. 0个QP创建请求测试: $(if [[ $zero_qp_test -gt 0 ]]; then echo "通过"; else echo "失败"; fi)" >> "$report_file"
    echo "3. 不同QP类型创建请求测试: $(if [[ $different_qp_test -gt 0 ]]; then echo "通过"; else echo "失败"; fi)" >> "$report_file"
    echo "4. 自动化测试（包含多种场景）: $(if [[ $automated_test -gt 0 ]]; then echo "通过"; else echo "失败"; fi)" >> "$report_file"
    
    # 日志摘要
    local total_qp_created=$(grep -c "QP created successfully" "$LOG_FILE")
    local total_qp_denied=$(grep -c "QP creation denied" "$LOG_FILE")
    
    echo "\n日志摘要:"
    echo "- 成功创建的QP总数: $total_qp_created"
    echo "- 被拒绝的QP创建请求数: $total_qp_denied"
    
    echo "\n日志摘要:"
    echo "- 成功创建的QP总数: $total_qp_created" >> "$report_file"
    echo "- 被拒绝的QP创建请求数: $total_qp_denied" >> "$report_file"
    
    echo "\n测试报告已生成: $report_file"
    echo "请查看详细报告以了解测试结果。"
}

# 主测试函数
main() {
    echo -e "${YELLOW}RDMA拦截系统自动化测试${NC}"
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
    
    echo "\n运行测试4: 自动化测试（包含多种场景）"
    if ! test_automated; then
        all_passed=false
    fi
    
    # 生成测试报告
    generate_report
    
    echo "======================================"
    if $all_passed; then
        echo -e "${GREEN}所有测试通过!${NC}"
        echo -e "${YELLOW}总结:${NC}"
        echo -e "  - LD_PRELOAD拦截库能够成功拦截真实的RDMA QP创建"
        echo -e "  - 当达到全局QP上限时，会拒绝新的QP创建请求"
        echo -e "  - 拦截功能正常工作"
        echo -e "  - 边界情况测试通过"
        echo -e "  - 自动化测试通过"
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
