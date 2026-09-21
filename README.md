# MTRDMA: RDMA Multi-Tenant Control-Plane Resource Isolation

A lightweight user-space interception mechanism that enforces hierarchical
per-process, per-tenant, and per-type (QP/MR) resource quotas on commodity
RDMA NICs, preserving the zero-copy kernel-bypass data path.

## Architecture

The system operates at three layers:

1. **Interception Layer** — `LD_PRELOAD`-based library that intercepts
   control-plane verbs (`ibv_create_qp`, `ibv_reg_mr`, etc.) and enforces
   admission control before the kernel commits hardware state.
2. **Communication Subsystem** — POSIX shared memory + spinlocks for
   O(1) hierarchical quota coordination across processes.
3. **Resource Management Daemon** — tracks process lifetimes via
   `pidfd` + `epoll`, with a hybrid GC that reclaims orphaned resources
   within 3 seconds of abnormal termination.

## Building

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Dependencies

- Linux kernel >= 5.9 (for `SECCOMP_USER_NOTIF` and `pidfd`)
- libibverbs (RDMA core)
- pthread
- clang (optional, for eBPF monitoring)

## Quick Start

```bash
# 1. Start the daemon
./build/collector_server_shm &

# 2. Set tenant quota
./build/tenant_manager --create <tenant_id> --quota <max_qp>,<max_mr>,<mem_mb>

# 3. Run applications under MTRDMA
LD_PRELOAD=./build/librdma_intercept.so RDMA_TENANT_ID=<tenant_id> ./your_app
```

## Layout

```
src/
  rdma_hooks_tenant.c    — LD_PRELOAD interception (QP/MR create/destroy)
  intercept_core.c       — function pointer resolution, admission control
  collector_server_shm.c — resource daemon (pidfd + epoll GC)
  performance_optimizer.c— per-process TTL cache, adaptive batching
  dynamic_policy_manager.c — runtime quota hot-update
include/                 — shared headers (shm layout, quota structs)
tests/                   — functional and lifecycle tests
scripts/                 — build helpers
```

## Key Properties

- **Exact enforcement**: created QPs/MRs exactly match configured quotas
- **O(1) admission**: per-process TTL cache amortises quota checks to ~103 ns
- **Transparent**: no application or kernel modification required
- **Fail-closed**: unenrolled processes denied in mandatory mode
- **Fast reclamation**: hybrid pidfd+epoll GC recovers within 3 s of crash

## License

This project is released under a permissive license for artifact evaluation.
