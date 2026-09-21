#!/bin/bash
# <!-- created at: 2026-02-04 18:00:00 -->

echo "清理现有进程..."
sudo pkill -f rdma_monitor 2>/dev/null
sudo pkill -f collector_server 2>/dev/null

# 清空现有的eBPF映射数据
sudo rm -rf /sys/fs/bpf/process_resources /sys/fs/bpf/global_resources

echo "启动eBPF监控程序..."
sudo timeout 25s ./rdma_monitor &
MONITOR_PID=$!

sleep 3  # 等待监控程序启动

echo "启动collector_server (设置QP限制为2)..."
export RDMA_INTERCEPT_MAX_GLOBAL_QP=2
sudo -E timeout 25s ./collector_server &
COLLECTOR_PID=$!

sleep 3  # 等待collector_server启动

echo "初始状态检查:"
./test_collector_comm

echo ""
echo "使用拦截库运行资源限制测试..."

# 设置拦截库环境变量
export RDMA_INTERCEPT_ENABLE=1
export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
export RDMA_INTERCEPT_MAX_QP=1  # 进程级别的QP限制为1

# 使用拦截库运行测试
LD_LIBRARY_PATH=.:$LD_LIBRARY_PATH LD_PRELOAD=./librdma_intercept.so ./test_intercept_ebpf

echo ""
echo "测试后状态检查:"
./test_collector_comm

# 结束进程
sudo kill $MONITOR_PID $COLLECTOR_PID 2>/dev/null

echo ""
echo "测试完成"