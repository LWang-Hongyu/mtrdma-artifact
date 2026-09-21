#!/bin/bash
# <!-- created at: 2026-02-04 13:30:00 -->
# 编译eBPF程序的脚本

set -e

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== 编译eBPF程序 ===${NC}"

# 检查clang是否可用
if ! command -v clang &> /dev/null; then
    echo -e "${RED}错误: clang 未安装，无法编译eBPF程序${NC}"
    echo -e "${YELLOW}请运行: sudo apt-get install clang${NC}"
    exit 1
fi

# 检查llvm
if ! command -v llc &> /dev/null; then
    echo -e "${RED}错误: llvm 未安装，无法编译eBPF程序${NC}"
    echo -e "${YELLOW}请运行: sudo apt-get install llvm${NC}"
    exit 1
fi

# 检查libbpf-dev
if [ ! -f "/usr/include/bpf/bpf.h" ]; then
    echo -e "${RED}错误: libbpf-dev 未安装，无法编译eBPF程序${NC}"
    echo -e "${YELLOW}请运行: sudo apt-get install libbpf-dev${NC}"
    exit 1
fi

echo -e "${YELLOW}使用clang编译eBPF程序...${NC}"

# 编译.bpf.c文件到.bpf.o
# 使用-O1而非-O2以减少间接调用指令的生成，避免callx指令导致的验证器错误
clang -g -O1 -target bpf -D__KERNEL__ -D__TARGET_ARCH_x86 -I/usr/include -c /opt/mtrdma/src/ebpf/rdma_monitor.bpf.c -o /opt/mtrdma/build/rdma_monitor.bpf.o

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ eBPF对象文件编译成功: build/rdma_monitor.bpf.o${NC}"
else
    echo -e "${RED}✗ eBPF对象文件编译失败${NC}"
    exit 1
fi

echo -e "${GREEN}=== eBPF程序编译完成 ===${NC}"