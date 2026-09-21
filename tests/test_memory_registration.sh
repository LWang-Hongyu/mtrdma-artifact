#!/bin/bash

# 测试内存注册和注销的RDMA原语监控和拦截功能
# 依赖：需要编译好的librdma_intercept.so库
# 运行方式：./test_memory_registration.sh

set -e

echo "=== 内存注册/注销监控测试 ==="

# 清理之前的测试环境
cleanup() {
    echo "清理测试环境..."
    pkill -f collector_server || true
    rm -f /tmp/rdma_collector.sock /tmp/rdma_intercept.log
    sleep 1
}

# 初始化测试环境
initialize() {
    echo "初始化测试环境..."
    cleanup
    
    # 项目已经构建完成，跳过编译步骤
    echo "项目已构建完成，跳过编译步骤..."
    cd "$(dirname "$0")/.."
    
    # 启动collector_server（设置全局内存上限为100MB）
    echo "启动collector_server..."
    export RDMA_INTERCEPT_MAX_GLOBAL_MEMORY=104857600  # 100MB
    ./build/collector_server &
    sleep 2
    
    echo "测试环境初始化完成"
}

# 测试1：基本内存注册和注销功能
test_basic_mr() {
    echo "\n测试1：基本内存注册和注销功能"
    
    # 创建测试程序
    cat > test_mr_basic.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main() {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    void *buf;
    size_t buf_size = 1024 * 1024;  // 1MB
    int i;

    // 分配内存
    buf = malloc(buf_size);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    // 获取RDMA设备上下文
    struct ibv_device **dev_list;
    int num_devices;
    
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "无法获取RDMA设备列表\n");
        free(buf);
        return 1;
    }
    
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "无法打开RDMA设备\n");
        ibv_free_device_list(dev_list);
        free(buf);
        return 1;
    }
    
    ibv_free_device_list(dev_list);

    // 分配PD
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        free(buf);
        return 1;
    }

    // 注册内存
    mr = ibv_reg_mr(pd, buf, buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(buf);
        return 1;
    }
    printf("成功注册内存: %p, 长度: %zu\n", mr, mr->length);

    // 注销内存
    if (ibv_dereg_mr(mr)) {
        perror("ibv_dereg_mr");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        free(buf);
        return 1;
    }
    printf("成功注销内存\n");

    // 清理资源
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    free(buf);

    printf("基本内存注册/注销测试成功\n");
    return 0;
}
EOF
    
    # 编译测试程序
    gcc -o test_mr_basic test_mr_basic.c -libverbs
    
    # 运行测试（启用拦截）
    echo "运行基本内存操作测试..."
    export LD_PRELOAD="./build/librdma_intercept.so"
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_MR_CONTROL=true
    export RDMA_INTERCEPT_MAX_MR_PER_PROCESS=10
    export RDMA_INTERCEPT_MAX_MEMORY_PER_PROCESS=52428800  # 50MB
    
    ./test_mr_basic
    
    echo "测试1通过：基本内存注册和注销功能正常"
    
    # 清理测试文件
    rm -f test_mr_basic.c test_mr_basic
}

# 测试2：内存数量限制测试
test_mr_count_limit() {
    echo "\n测试2：内存数量限制测试"
    
    # 创建测试程序
    cat > test_mr_count.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main() {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mrs[6];
    void *bufs[6];
    size_t buf_size = 1024 * 1024;  // 1MB
    int i, count = 0;

    // 获取RDMA设备上下文
    struct ibv_device **dev_list;
    int num_devices;
    
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "无法获取RDMA设备列表\n");
        return 1;
    }
    
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "无法打开RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    ibv_free_device_list(dev_list);

    // 分配PD
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        return 1;
    }

    // 尝试注册6个MR（超过限制5个）
    for (i = 0; i < 6; i++) {
        bufs[i] = malloc(buf_size);
        if (!bufs[i]) {
            perror("malloc");
            break;
        }

        mrs[i] = ibv_reg_mr(pd, bufs[i], buf_size, IBV_ACCESS_LOCAL_WRITE);
        if (mrs[i]) {
            printf("成功注册MR %d: %p\n", i+1, mrs[i]);
            count++;
        } else {
            printf("MR %d 注册失败（预期行为，达到数量限制）\n", i+1);
            free(bufs[i]);
            break;
        }
    }

    // 清理已注册的MR
    for (i = 0; i < count; i++) {
        if (mrs[i]) {
            ibv_dereg_mr(mrs[i]);
            free(bufs[i]);
        }
    }

    // 清理资源
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);

    if (count == 5 && i == 5) {
        printf("内存数量限制测试成功：成功限制为5个MR\n");
        return 0;
    } else {
        printf("内存数量限制测试失败：预期5个MR，实际注册了%d个\n", count);
        return 1;
    }
}
EOF
    
    # 编译测试程序
    gcc -o test_mr_count test_mr_count.c -libverbs
    
    # 运行测试（设置每个进程最大MR数量为5）
    echo "运行内存数量限制测试..."
    export LD_PRELOAD="./build/librdma_intercept.so"
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_MR_CONTROL=true
    export RDMA_INTERCEPT_MAX_MR_PER_PROCESS=5
    export RDMA_INTERCEPT_MAX_MEMORY_PER_PROCESS=104857600  # 100MB
    
    ./test_mr_count
    
    echo "测试2通过：内存数量限制功能正常"
    
    # 清理测试文件
    rm -f test_mr_count.c test_mr_count
}

# 测试3：内存大小限制测试
test_memory_size_limit() {
    echo "\n测试3：内存大小限制测试"
    
    # 创建测试程序
    cat > test_memory_size.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main() {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr1, *mr2;
    void *buf1, *buf2;
    size_t buf_size1 = 30 * 1024 * 1024;  // 30MB
    size_t buf_size2 = 30 * 1024 * 1024;  // 30MB

    // 获取RDMA设备上下文
    struct ibv_device **dev_list;
    int num_devices;
    
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "无法获取RDMA设备列表\n");
        return 1;
    }
    
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "无法打开RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    ibv_free_device_list(dev_list);

    // 分配PD
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        return 1;
    }

    // 分配内存
    buf1 = malloc(buf_size1);
    buf2 = malloc(buf_size2);
    if (!buf1 || !buf2) {
        perror("malloc");
        free(buf1);
        free(buf2);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    // 注册第一个MR（30MB）
    mr1 = ibv_reg_mr(pd, buf1, buf_size1, IBV_ACCESS_LOCAL_WRITE);
    if (!mr1) {
        perror("ibv_reg_mr (first)");
        free(buf1);
        free(buf2);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("成功注册第一个MR: %p, 大小: %zu MB\n", mr1, buf_size1 / (1024*1024));

    // 尝试注册第二个MR（30MB，应该失败，因为总大小会超过50MB限制）
    mr2 = ibv_reg_mr(pd, buf2, buf_size2, IBV_ACCESS_LOCAL_WRITE);
    if (mr2) {
        printf("第二个MR注册成功（预期应该失败）\n");
        ibv_dereg_mr(mr2);
        free(buf2);
        ibv_dereg_mr(mr1);
        free(buf1);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    } else {
        printf("第二个MR注册失败（预期行为，达到内存大小限制）\n");
    }

    // 清理资源
    ibv_dereg_mr(mr1);
    free(buf1);
    free(buf2);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);

    printf("内存大小限制测试成功：成功限制为50MB\n");
    return 0;
}
EOF
    
    # 编译测试程序
    gcc -o test_memory_size test_memory_size.c -libverbs
    
    # 运行测试（设置每个进程最大内存为50MB）
    echo "运行内存大小限制测试..."
    export LD_PRELOAD="./build/librdma_intercept.so"
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_MR_CONTROL=true
    export RDMA_INTERCEPT_MAX_MR_PER_PROCESS=10
    export RDMA_INTERCEPT_MAX_MEMORY_PER_PROCESS=52428800  # 50MB
    
    ./test_memory_size
    
    echo "测试3通过：内存大小限制功能正常"
    
    # 清理测试文件
    rm -f test_memory_size.c test_memory_size
}

# 测试4：全局内存限制测试
test_global_memory_limit() {
    echo "\n测试4：全局内存限制测试"
    
    # 重启collector_server，设置全局内存上限为60MB
    echo "重启collector_server（设置全局内存上限为60MB）..."
    pkill -f collector_server || true
    sleep 1
    export RDMA_INTERCEPT_MAX_GLOBAL_MEMORY=62914560  # 60MB
    ./build/collector_server &
    sleep 2
    
    # 创建测试程序
    cat > test_global_memory.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main() {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr1, *mr2;
    void *buf1, *buf2;
    size_t buf_size1 = 30 * 1024 * 1024;  // 30MB
    size_t buf_size2 = 35 * 1024 * 1024;  // 35MB

    // 获取RDMA设备上下文
    struct ibv_device **dev_list;
    int num_devices;
    
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "无法获取RDMA设备列表\n");
        return 1;
    }
    
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "无法打开RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    ibv_free_device_list(dev_list);

    // 分配PD
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        return 1;
    }

    // 分配内存
    buf1 = malloc(buf_size1);
    buf2 = malloc(buf_size2);
    if (!buf1 || !buf2) {
        perror("malloc");
        free(buf1);
        free(buf2);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    // 注册第一个MR（30MB）
    mr1 = ibv_reg_mr(pd, buf1, buf_size1, IBV_ACCESS_LOCAL_WRITE);
    if (!mr1) {
        perror("ibv_reg_mr (first)");
        free(buf1);
        free(buf2);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("成功注册第一个MR: %p, 大小: %zu MB\n", mr1, buf_size1 / (1024*1024));

    // 尝试注册第二个MR（35MB，应该失败，因为全局内存会超过60MB限制）
    mr2 = ibv_reg_mr(pd, buf2, buf_size2, IBV_ACCESS_LOCAL_WRITE);
    if (mr2) {
        printf("第二个MR注册成功（预期应该失败）\n");
        ibv_dereg_mr(mr2);
        free(buf2);
        ibv_dereg_mr(mr1);
        free(buf1);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    } else {
        printf("第二个MR注册失败（预期行为，达到全局内存限制）\n");
    }

    // 清理资源
    ibv_dereg_mr(mr1);
    free(buf1);
    free(buf2);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);

    printf("全局内存限制测试成功：成功限制为60MB\n");
    return 0;
}
EOF
    
    # 编译测试程序
    gcc -o test_global_memory test_global_memory.c -libverbs
    
    # 运行测试
    echo "运行全局内存限制测试..."
    export LD_PRELOAD="./build/librdma_intercept.so"
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_MR_CONTROL=true
    export RDMA_INTERCEPT_MAX_MR_PER_PROCESS=10
    export RDMA_INTERCEPT_MAX_MEMORY_PER_PROCESS=104857600  # 100MB
    
    ./test_global_memory
    
    echo "测试4通过：全局内存限制功能正常"
    
    # 清理测试文件
    rm -f test_global_memory.c test_global_memory
}

# 测试5：验证collector_server内存统计功能
test_collector_stats() {
    echo "\n测试5：验证collector_server内存统计功能"
    
    # 重启collector_server
    echo "重启collector_server..."
    pkill -f collector_server || true
    sleep 1
    export RDMA_INTERCEPT_MAX_GLOBAL_MEMORY=104857600  # 100MB
    ./build/collector_server &
    sleep 2
    
    # 创建测试程序
    cat > test_collector_stats.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>

int main() {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    void *buf;
    size_t buf_size = 20 * 1024 * 1024;  // 20MB

    // 获取RDMA设备上下文
    struct ibv_device **dev_list;
    int num_devices;
    
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
        fprintf(stderr, "无法获取RDMA设备列表\n");
        return 1;
    }
    
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "无法打开RDMA设备\n");
        ibv_free_device_list(dev_list);
        return 1;
    }
    
    ibv_free_device_list(dev_list);

    // 分配PD
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd");
        ibv_close_device(ctx);
        return 1;
    }

    // 分配内存
    buf = malloc(buf_size);
    if (!buf) {
        perror("malloc");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    // 注册MR
    mr = ibv_reg_mr(pd, buf, buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        perror("ibv_reg_mr");
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("成功注册MR: %p, 大小: %zu MB\n", mr, buf_size / (1024*1024));

    // 等待一段时间让collector_server更新统计
    sleep(1);

    // 注销MR
    if (ibv_dereg_mr(mr)) {
        perror("ibv_dereg_mr");
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("成功注销MR\n");

    // 清理资源
    free(buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);

    printf("collector_server统计测试完成\n");
    return 0;
}
EOF
    
    # 编译测试程序
    gcc -o test_collector_stats test_collector_stats.c -libverbs
    
    # 运行测试
    echo "运行collector_server统计测试..."
    export LD_PRELOAD="./build/librdma_intercept.so"
    export RDMA_INTERCEPT_ENABLE=1
    export RDMA_INTERCEPT_ENABLE_MR_CONTROL=true
    
    ./test_collector_stats
    
    # 检查collector_server统计
    echo "检查collector_server统计信息..."
    
    # 创建临时客户端获取统计
    cat > check_stats.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main() {
    int fd;
    struct sockaddr_un addr;
    char buffer[512];
    int n;

    // 创建socket
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    // 连接到collector_server
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/rdma_collector.sock", sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    // 发送GET_STATS请求
    n = write(fd, "GET_STATS", 9);
    if (n < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    // 读取响应
    n = read(fd, buffer, sizeof(buffer) - 1);
    if (n < 0) {
        perror("read");
        close(fd);
        return 1;
    }

    buffer[n] = '\0';
    printf("collector_server统计信息:\n%s", buffer);

    // 检查是否包含内存统计信息
    if (strstr(buffer, "Total MR:") && strstr(buffer, "Total Memory Used:") && strstr(buffer, "Max Memory:")) {
        printf("collector_server内存统计功能正常\n");
    } else {
        printf("collector_server内存统计功能缺失\n");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
EOF
    
    gcc -o check_stats check_stats.c
    ./check_stats
    
    echo "测试5通过：collector_server内存统计功能正常"
    
    # 清理测试文件
    rm -f test_collector_stats.c test_collector_stats check_stats.c check_stats
}

# 运行所有测试
run_all_tests() {
    initialize
    
    test_basic_mr
    test_mr_count_limit
    test_memory_size_limit
    test_global_memory_limit
    test_collector_stats
    
    cleanup
    
    echo "\n=== 所有内存监控测试通过！==="
}

# 运行测试
run_all_tests
