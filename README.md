# MTRDMA: 基于LD_PRELOAD的RDMA多租户资源隔离系统

基于LD_PRELOAD技术和共享内存的高性能RDMA多租户资源隔离系统，实现三级分层资源配额管理和基于pidfd+epoll的事件驱动资源回收。

## 项目概述

本项目解决多租户环境下RDMA资源竞争问题，核心技术：
- **LD_PRELOAD拦截**：动态库注入实现用户态RDMA API拦截，无内核依赖
- **三级分层配额**：进程级(L1)→全局级(L2)→租户级(L3)，`effective_quota = min(L1, L2_remaining, L3_remaining)`
- **pidfd+epoll资源回收**：事件驱动GC，进程退出后实时回收残留资源
- **共享内存通信**：零拷贝、低延迟的进程间数据交换

## 系统架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          MTRDMA 系统架构                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐  │
│  │   Application   │────▶│ librdma_intercept│────▶│  tenant_manager │  │
│  │   (用户应用)     │     │   (拦截库)        │     │   _daemon       │  │
│  │   LD_PRELOAD     │     │   src/rdma_       │     │   (租户管理)    │  │
│  └─────────────────┘     │   hooks_tenant.c   │     └────────┬────────┘  │
│                               │                    ↑         │           │
│                               ▼                    │         ▼           │
│                        ┌─────────────┐              ┌─────────────┐     │
│                        │  共享内存     │◀────────────▶│  共享内存     │     │
│                        │  shm/        │              │  租户级       │     │
│                        │  shared_     │              │  shm/        │     │
│                        │  memory.c    │              │  shared_     │     │
│                        └─────────────┘              │  memory_     │     │
│                               ↑                     │  tenant.c    │     │
│                               │                     └──────────────┘     │
│                        ┌──────────────────┐                             │
│                        │ collector_server │                             │
│                        │   _shm           │                             │
│                        │  ┌────────────┐  │                             │
│                        │  │ eBPF 同步   │  │                             │
│                        │  │ GC 线程     │  │                             │
│                        │  │ (pidfd+     │  │                             │
│                        │  │  epoll)     │  │                             │
│                        │  └────────────┘  │                             │
│                        └──────────────────┘                             │
└─────────────────────────────────────────────────────────────────────────┘
```

## 核心组件

### 1. 动态拦截库 (`librdma_intercept.so`)
| 源文件 | 作用 |
|--------|------|
| [rdma_hooks_tenant.c](file:///opt/mtrdma/src/rdma_hooks_tenant.c) | 租户级RDMA API拦截核心（QP/MR/CQ/PD创建销毁，三级配额检查） |
| [intercept_core.c](file:///opt/mtrdma/src/intercept_core.c) | 拦截框架初始化、函数指针解析、动态链接 |
| [performance_optimizer.c](file:///opt/mtrdma/src/performance_optimizer.c) | 进程级本地缓存、自适应TTL、批量操作优化 |
| [dynamic_policy_manager.c](file:///opt/mtrdma/src/dynamic_policy_manager.c) | 动态策略管理器，支持运行时配额热更新 |

### 2. 数据收集与GC服务 (`collector_server_shm`)
| 源文件 | 作用 |
|--------|------|
| [collector_server_shm.c](file:///opt/mtrdma/src/collector_server_shm.c) | 共享内存数据收集服务，eBPF同步，pidfd+epoll GC回收线程 |
| 关键线程 | 说明 |
| `eBPF同步线程` | 每100ms从eBPF maps同步数据到共享内存 |
| `GC线程` | pidfd+epoll事件驱动资源回收，1s超时兜底扫描 |

### 3. 租户管理
| 源文件 | 作用 |
|--------|------|
| [tenant_manager_daemon.c](file:///opt/mtrdma/src/tenant_manager_daemon.c) | 守护进程，统一管理租户配额和限制策略 |
| [tenant_manager_client.c](file:///opt/mtrdma/src/tenant_manager_client.c) | CLI客户端，创建/更新/删除/查询租户 |

### 4. 共享内存模块
| 源文件 | 作用 |
|--------|------|
| [shm/shared_memory.c](file:///opt/mtrdma/src/shm/shared_memory.c) | 基础共享内存：进程资源表、全局QP/MR计数、读写锁同步 |
| [shm/shared_memory.h](file:///opt/mtrdma/src/shm/shared_memory.h) | 共享内存数据结构定义 |
| [shm/shared_memory_tenant.c](file:///opt/mtrdma/src/shm/shared_memory_tenant.c) | 租户级共享内存：租户资源数组、PID-租户映射 |
| [shm/shared_memory_tenant.h](file:///opt/mtrdma/src/shm/shared_memory_tenant.h) | 租户共享内存数据结构定义 |

### 5. eBPF监控模块（辅助数据源）
| 源文件 | 作用 |
|--------|------|
| [ebpf/ebpf_monitor_shm.c](file:///opt/mtrdma/src/ebpf/ebpf_monitor_shm.c) | eBPF监控库（共享内存版本），从内核eBPF maps采集RDMA事件 |
| [ebpf/ebpf_monitor_shm.h](file:///opt/mtrdma/src/ebpf/ebpf_monitor_shm.h) | eBPF监控头文件 |
| [ebpf/ebpf_enhanced_monitor.c](file:///opt/mtrdma/src/ebpf/ebpf_enhanced_monitor.c) | 增强型eBPF监控，支持更细粒度的RDMA事件追踪 |
| [ebpf/ebpf_enhanced_monitor.h](file:///opt/mtrdma/src/ebpf/ebpf_enhanced_monitor.h) | 增强型eBPF监控头文件 |

### 6. 基础设施
| 源文件 | 作用 |
|--------|------|
| [config.c](file:///opt/mtrdma/src/config.c) | 环境变量配置解析、运行时参数管理 |
| [logger.c](file:///opt/mtrdma/src/logger.c) | 多级别日志系统，支持文件输出 |

## 目录结构

```
rdma_intercept_ldpreload/
├── src/                              # 核心源代码
│   ├── rdma_hooks_tenant.c           # 租户级RDMA API拦截（核心）
│   ├── intercept_core.c              # 拦截框架初始化
│   ├── collector_server_shm.c        # 共享内存数据收集 + GC回收
│   ├── tenant_manager_daemon.c       # 租户管理守护进程
│   ├── tenant_manager_client.c       # 租户管理CLI客户端
│   ├── dynamic_policy_manager.c      # 动态策略管理器
│   ├── performance_optimizer.c       # 性能优化器（本地缓存）
│   ├── config.c                      # 配置管理
│   ├── logger.c                      # 日志系统
│   ├── shm/                          # 共享内存模块
│   │   ├── shared_memory.c           #   基础共享内存
│   │   ├── shared_memory.h           #   头文件
│   │   ├── shared_memory_tenant.c    #   租户级共享内存
│   │   └── shared_memory_tenant.h    #   头文件
│   └── ebpf/                         # eBPF监控模块
│       ├── ebpf_monitor_shm.c        #   eBPF监控（共享内存版）
│       ├── ebpf_monitor_shm.h        #   头文件
│       ├── ebpf_enhanced_monitor.c   #   增强型eBPF监控
│       └── ebpf_enhanced_monitor.h   #   头文件
├── include/                          # 公共头文件
│   └── rdma_intercept.h              # 主头文件
├── build/                            # 构建输出目录
├── tests/                            # 测试程序
├── experiments/                      # 实验验证
│   ├── exp_isolation_verify/         #   隔离性验证实验（原始基准）
│   ├── exp1_microbenchmark/          #   微基准测试
│   ├── exp2_multi_tenant_isolation/  #   多租户隔离测试
│   ├── exp12_mtrdma_perf_combined/   #   综合性能对比
│   ├── exp13_hierarchical_quota/     #   三级分层配额实验
│   ├── exp14_gc_reclamation/         #   GC回收机制验证
│   └── exp15_cpu_overhead/           #   CPU消耗实验
├── CMakeLists.txt                    # CMake构建配置
└── README.md                         # 本文档
```

## 快速开始

### 1. 构建项目

```bash
cd /opt/mtrdma
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

构建产物包括：
| 产物 | 路径 | 说明 |
|------|------|------|
| `librdma_intercept.so` | `build/librdma_intercept.so` | 主拦截库（LD_PRELOAD注入） |
| `collector_server_shm` | `build/collector_server_shm` | 数据收集+GC服务 |
| `tenant_manager_daemon` | `build/tenant_manager_daemon` | 租户管理守护进程 |
| `tenant_manager_client` | `build/tenant_manager_client` | 租户管理CLI |

### 2. 启动数据收集服务（必须先启动）

```bash
cd build
sudo rm -f /dev/shm/rdma_shm_*  # 清理旧共享内存
sudo ./collector_server_shm &
sleep 2
```

**注意**：必须先启动 `collector_server_shm`，它会创建共享内存区域。拦截库和租户管理服务在启动时连接到此共享内存。

### 3. 创建租户并设置配额

```bash
cd build
# 创建租户ID=10，QP上限=10，MR上限=10，内存上限=1GB
sudo ./tenant_manager_client create 10 10 10 1073741824 "Tenant_A"

# 创建租户ID=20，QP上限=20，MR上限=20，内存上限=2GB
sudo ./tenant_manager_client create 20 20 20 2147483648 "Tenant_B"

# 查看租户列表
sudo ./tenant_manager_client list
```

### 4. 运行受保护的应用

```bash
export LD_PRELOAD=/path/to/build/librdma_intercept.so
export RDMA_INTERCEPT_ENABLE=1
export RDMA_INTERCEPT_ENABLE_QP_CONTROL=1
export RDMA_INTERCEPT_ENABLE_MR_CONTROL=1
export RDMA_TENANT_ID=10

# 运行RDMA应用
ib_write_bw -d mlx5_0 -x 2
```

## 三级分层配额机制

### 配额层次

```
进程级(L1) ←────────────────── 每进程最大QP/MR数
    │
全局级(L2) ←────────────────── 系统全局QP/MR总量上限
    │
租户级(L3) ←────────────────── 租户维度的QP/MR/内存上限
```

### 检查逻辑

```c
effective_quota = min(L1_limit, L2_remaining, L3_remaining)
```

### 共享内存数据结构

```
共享内存布局 (shm/shared_memory.h: resource_usage_t):
┌───────────────────────────────────────────────┐
│  全局统计 (global)                              │
│  ├─ total_qp / total_mr / total_memory         │
│  ├─ max_qp / max_mr / max_memory               │
│  └─ process_count                              │
├───────────────────────────────────────────────┤
│  进程资源表 (process_pids[], process_stats[])   │
│  ├─ PID + 启动时间(starttime)  ← PID重用防护  │
│  └─ qp_count / mr_count / memory_used          │
├───────────────────────────────────────────────┤
│  写锁 (rwlock)                                 │
│  版本号 (version)                              │
└───────────────────────────────────────────────┘

租户共享内存布局 (shm/shared_memory_tenant.h):
┌───────────────────────────────────────────────┐
│  头部信息 (魔数、版本、租户计数)                │
├───────────────────────────────────────────────┤
│  租户数组 tenant_info_t[MAX_TENANTS]           │
│  ├─ tenant_id / name                           │
│  ├─ max_qp / max_mr / max_memory (配额)        │
│  ├─ cur_qp / cur_mr / cur_memory (当前)        │
│  └─ status / created_at                        │
├───────────────────────────────────────────────┤
│  PID→租户映射 pid_tenant_map[MAX_PROCESSES]    │
└───────────────────────────────────────────────┘
```

## GC资源回收机制

### 问题背景

当进程被 `SIGKILL` 强制终止时，`ibv_destroy_qp` 等清理API不会被调用，导致共享内存中的资源计数泄漏（QP/MR计数未减）。

### 解决方案：pidfd + epoll 事件驱动回收

```
collector_server_shm GC线程架构:
┌─────────────────────────────────────────────────────────────────┐
│  ┌──────────────────────┐     ┌──────────────────────────────┐  │
│  │ eBPF sync thread     │     │ pidfd + epoll GC thread      │  │
│  │ (sync_thread_func)   │     │ (gc_thread_func)             │  │
│  │                      │     │                              │  │
│  │ 每100ms从eBPF maps   │     │ epoll_wait(1s timeout)       │  │
│  │ 同步数据到共享内存     │     │ ─ EPOLLIN → 立即回收         │  │
│  └──────────────────────┘     │ ─ timeout → 扫描新进程+兜底   │  │
│                               │                              │  │
│                               │ gc_pidfd_table[MAX_PROC]     │  │
│                               │ ┌──┬──┬──┬──┬──┬──┬──┬──┐   │  │
│                               │ │pidfd,pid,boottime...│   │  │  │
│                               │ └──┴──┴──┴──┴──┴──┴──┴──┘   │  │
│                               └──────────────────────────────┘  │
│                                       │                          │
│                                       ▼                          │
│                               ┌───────────────┐                 │
│                               │  共享内存       │                 │
│                               │  process_pids  │                 │
│                               │  process_stats │                 │
│                               │  global_stats  │                 │
│                               └───────┬───────┘                 │
│                                       │                          │
│                                       ▼                          │
│                               ┌───────────────┐                 │
│                               │ 租户共享内存    │                 │
│                               │ pid_tenant_map │                 │
│                               │ tenant usage   │                 │
│                               └───────────────┘                 │
└─────────────────────────────────────────────────────────────────┘
```

### 核心流程

| 步骤 | 说明 |
|------|------|
| **1. 注册** | 扫描共享内存进程表，对有效进程调用 `pidfd_open()` 获取pidfd，注册到epoll |
| **2. 事件驱动** | `epoll_wait()` 等待进程退出事件（EPOLLIN），触发后立即回收资源 |
| **3. 兜底扫描** | 每次超时（1s）后 `gc_scan_and_register()`：对新进程注册pidfd，对已注册但已死进程通过 `kill(pid,0)` 回收 |
| **4. PID重用防护** | 共享内存中记录进程启动时间（`/proc/[pid]/stat` 的starttime字段），回收前比对，防止误回收 |
| **5. 租户联动** | 回收进程资源后同步减去对应租户的计数 |

### 关键代码位置

| 函数 | 文件:行号 | 作用 |
|------|-----------|------|
| `gc_thread_func()` | [collector_server_shm.c](file:///opt/mtrdma/src/collector_server_shm.c) | GC线程主循环 |
| `gc_scan_and_register()` | [collector_server_shm.c](file:///opt/mtrdma/src/collector_server_shm.c) | 扫描进程表+注册pidfd+兜底回收 |
| `gc_collect_process()` | [collector_server_shm.c](file:///opt/mtrdma/src/collector_server_shm.c) | 单个进程资源回收（全局+租户联动） |
| `gc_is_process_really_alive()` | [collector_server_shm.c](file:///opt/mtrdma/src/collector_server_shm.c) | PID重用检测（`kill(pid,0)` + boottime比对） |

## 配置参数

### 环境变量

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `RDMA_INTERCEPT_ENABLE` | bool | 1 | 启用拦截功能 |
| `RDMA_INTERCEPT_ENABLE_QP_CONTROL` | bool | 0 | 启用QP配额控制 |
| `RDMA_INTERCEPT_ENABLE_MR_CONTROL` | bool | 0 | 启用MR配额控制 |
| `RDMA_INTERCEPT_ENABLE_MR_RATE_LIMIT` | bool | 0 | 启用MR操作速率限制 |
| `RDMA_INTERCEPT_MAX_QP_PER_PROCESS` | int | 100 | 每进程最大QP数（L1限额） |
| `RDMA_INTERCEPT_MAX_MR_PER_PROCESS` | int | 100 | 每进程最大MR数（L1限额） |
| `RDMA_TENANT_ID` | int | 0 | 指定进程所属租户ID |
| `RDMA_INTERCEPT_MAX_MR_OPS_PER_SEC` | int | 1000 | MR操作速率上限（次/秒） |
| `RDMA_INTERCEPT_MR_RATE_LIMIT_WINDOW_MS` | int | 1000 | 速率限制时间窗口（毫秒） |
| `RDMA_INTERCEPT_LOG_LEVEL` | enum | INFO | 日志级别：DEBUG/INFO/WARN/ERROR |
| `RDMA_INTERCEPT_LOG_FILE_PATH` | string | `/tmp/rdma_intercept.log` | 日志文件路径 |

### 租户管理CLI命令

```
# 创建租户
sudo ./tenant_manager_client create <tenant_id> <max_qp> <max_mr> <max_memory> <name>

# 更新配额
sudo ./tenant_manager_client update <tenant_id> <max_qp> <max_mr> <max_memory>

# 删除租户（同时清理关联进程资源）
sudo ./tenant_manager_client delete <tenant_id>

# 查询单个租户
sudo ./tenant_manager_client query <tenant_id>

# 列出所有租户
sudo ./tenant_manager_client list
```

### 资源限制层级关系

| 层级 | 配置方式 | 作用范围 |
|------|----------|----------|
| L1 进程级 | `RDMA_INTERCEPT_MAX_QP_PER_PROCESS` 环境变量 | 单进程 |
| L2 全局级 | 共享内存 `global.max_qp` | 系统全局 |
| L3 租户级 | `tenant_manager_client create ...` | 租户维度 |

## 部署注意事项

### 启动顺序（重要）

正确的启动顺序是：

1. **清理旧共享内存**（如有）：`sudo rm -f /dev/shm/rdma_shm_*`
2. **启动 `collector_server_shm`**：创建共享内存，启动eBPF同步和GC线程
3. **启动 `tenant_manager_daemon`**：连接共享内存，管理租户
4. **创建租户配额**：通过 `tenant_manager_client` 配置
5. **运行应用**：设置 `LD_PRELOAD` + 环境变量后执行RDMA程序

### 权限要求

- `collector_server_shm` → 需要 `sudo`（创建共享内存、eBPF操作）
- `tenant_manager_daemon` → 需要 `sudo`
- `tenant_manager_client` → 需要 `sudo`
- 运行应用 → 不需要 `sudo`，依赖LD_PRELOAD注入

### 共享内存清理

如果 `collector_server_shm` 异常退出，共享内存可能残留：
```bash
sudo rm -f /dev/shm/rdma_shm_*
sudo rm -f /dev/shm/rdma_shm_tenants_*
ipcrm -a  # 清理所有System V共享内存（可选）
```

### 交叉编译/无RDMA硬件环境

系统支持模拟模式编译，条件编译自动检测 `infiniband/verbs.h`：
```
cmake ..   # 如有RDMA头文件，编译完整版
           # 如无RDMA头文件，编译模拟模式（用于代码检查）
```

## 实验验证

| 实验 | 目录 | 说明 |
|------|------|------|
| Isolation Verify | [exp_isolation_verify/](file:///opt/mtrdma/experiments/exp_isolation_verify/) | 隔离性验证原始基准实验 |
| EXP-1 Microbenchmark | [exp1_microbenchmark/](file:///opt/mtrdma/experiments/exp1_microbenchmark/) | 微基准测试，测量拦截开销 |
| EXP-2 Multi-Tenant Isolation | [exp2_multi_tenant_isolation/](file:///opt/mtrdma/experiments/exp2_multi_tenant_isolation/) | 多租户隔离功能验证 |
| EXP-12 Perf Combined | [exp12_mtrdma_perf_combined/](file:///opt/mtrdma/experiments/exp12_mtrdma_perf_combined/) | ATC风格综合性能对比 |
| EXP-13 Hierarchical Quota | [exp13_hierarchical_quota/](file:///opt/mtrdma/experiments/exp13_hierarchical_quota/) | 三级分层配额有效性验证 |
| EXP-14 GC Reclamation | [exp14_gc_reclamation/](file:///opt/mtrdma/experiments/exp14_gc_reclamation/) | GC回收机制验证（pidfd+epoll） |
| EXP-15 CPU Overhead | [exp15_cpu_overhead/](file:///opt/mtrdma/experiments/exp15_cpu_overhead/) | MTRDMA CPU消耗测量 |

## 构建依赖

| 依赖 | 用途 | 安装命令 |
|------|------|----------|
| `cmake` (>= 3.10) | 构建系统 | `apt install cmake` |
| `gcc` | 编译器 | `apt install gcc` |
| `libibverbs-dev` | RDMA头文件 | `apt install libibverbs-dev` |
| `librdmacm-dev` | RDMA通信管理 | `apt install librdmacm-dev` |
| `libbpf-dev` | eBPF支持 | `apt install libbpf-dev` |
| `libjson-c-dev` | JSON处理(租户管理) | `apt install libjson-c-dev` |

## 技术原理

### 拦截流程

```
应用 RDMA API 调用
    │
    ▼
LD_PRELOAD 拦截 (librdma_intercept.so)
    │
    ├──→ ibv_create_qp / ibv_destroy_qp
    ├──→ ibv_reg_mr / ibv_dereg_mr
    ├──→ ibv_create_cq / ibv_destroy_cq
    └──→ ibv_alloc_pd / ibv_dealloc_pd
    │
    ▼
配额检查 (三级分层)
    │
    ├── L1: 进程级 (环境变量限额)
    ├── L2: 全局级 (共享内存 global.max_*)
    └── L3: 租户级 (共享内存 tenant.max_*)
    │
    ├──→ 通过 → 调用原始API → 更新共享内存计数
    └──→ 拒绝 → 返回错误 (拒绝计数+1)
```

### 共享内存同步机制

- **读写锁**：`pthread_rwlock_t` 保护共享内存并发访问
- **原子操作**：资源计数使用 `__sync_fetch_and_add` 原子更新
- **进程启动时间**：记录 `/proc/[pid]/stat` 的 starttime 字段，用于 PID 重用检测
- **版本号**：每次更新后递增，用于检测数据陈旧性

### 性能优化

| 优化手段 | 说明 |
|----------|------|
| 本地缓存 | 每个进程缓存租户配额，减少共享内存查询 |
| 自适应TTL | 根据竞争程度动态调整缓存有效期 |
| 批量同步 | 累积多次操作后一次性同步到共享内存 |
| 自旋锁 | 使用自旋锁代替互斥锁，减少上下文切换 |