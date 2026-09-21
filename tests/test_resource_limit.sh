#!/bin/bash
# <!-- created at: 2026-02-04 17:30:00 -->

echo "清理现有进程..."
sudo pkill -f rdma_monitor 2>/dev/null
sudo pkill -f collector_server 2>/dev/null

# 清空现有的eBPF映射数据
sudo rm -rf /sys/fs/bpf/process_resources /sys/fs/bpf/global_resources

echo "启动eBPF监控程序..."
sudo timeout 20s ./rdma_monitor &
MONITOR_PID=$!

sleep 2  # 等待监控程序启动

echo "启动collector_server (设置QP限制为2)..."
export RDMA_INTERCEPT_MAX_GLOBAL_QP=2
sudo -E timeout 20s ./collector_server &
COLLECTOR_PID=$!

sleep 2  # 等待collector_server启动

echo "初始状态检查:"
./test_collector_comm

echo ""
echo "使用拦截库运行资源密集型测试..."
LD_LIBRARY_PATH=.:/usr/local/lib64 ./test_ebpf_comprehensive

echo ""
echo "测试后状态检查:"
./test_collector_comm

# 结束进程
sudo kill $MONITOR_PID $COLLECTOR_PID 2>/dev/null

echo ""
echo "测试完成"