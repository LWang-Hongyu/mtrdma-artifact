#!/bin/bash
# <!-- created at: 2026-01-27 18:00:00 -->
# 构建所有组件的脚本

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== 构建RDMA拦截系统所有组件 ===${NC}"

# 检查依赖
echo -e "${YELLOW}检查依赖...${NC}"
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}错误: cmake 未安装${NC}"
    exit 1
fi

if ! command -v make &> /dev/null; then
    echo -e "${RED}错误: make 未安装${NC}"
    exit 1
fi

# 构建主项目
echo -e "${YELLOW}构建主项目...${NC}"
cd "$(dirname "$0")"

# 创建构建目录
mkdir -p build
cd build

# 运行cmake
cmake ..

# 构建项目
make -j4

echo -e "${GREEN}主项目构建完成${NC}"

# 检查构建结果
echo -e "${YELLOW}检查构建结果...${NC}"

# 检查主拦截库
if [ -f "librdma_intercept.so" ]; then
    echo -e "${GREEN}✓ 主拦截库: librdma_intercept.so${NC}"
else
    echo -e "${RED}✗ 主拦截库未构建成功${NC}"
    exit 1
fi

# 检查测试客户端
if [ -f "test_rdma_client" ]; then
    echo -e "${GREEN}✓ 测试客户端: test_rdma_client${NC}"
else
    echo -e "${RED}✗ 测试客户端未构建成功${NC}"
    exit 1
fi

# 检查数据收集服务（共享内存版）
if [ -f "collector_server_shm" ]; then
    echo -e "${GREEN}✓ 数据收集服务: collector_server_shm${NC}"
else
    echo -e "${RED}✗ 数据收集服务未构建成功${NC}"
    exit 1
fi

echo -e "${GREEN}\n=== 所有组件构建完成 ===${NC}"
echo -e "${YELLOW}使用方法:${NC}"
echo -e "  1. 启动数据收集服务: cd build && sudo ./collector_server_shm"
echo -e "  2. 运行实验测试: cd experiments && ./run_experiment.sh"
