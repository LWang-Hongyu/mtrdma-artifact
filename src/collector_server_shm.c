/*
 * collector_server_shm.c
 * 基于共享内存的资源收集服务器
 * 从eBPF maps读取数据并同步到共享内存
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "shm/shared_memory.h"
#include "shm/shared_memory_tenant.h"

// 条件编译：如果定义了NO_EBPF，则不包含eBPF相关头文件
#ifndef NO_EBPF
#include <linux/bpf.h>
#include <sys/syscall.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ebpf/ebpf_monitor_shm.h"
#endif

// 全局变量
static volatile int running = 1;
static uint32_t max_global_qp = 10; // 默认全局QP上限
static pthread_mutex_t qp_count_mutex = PTHREAD_MUTEX_INITIALIZER;

// 内存资源全局变量
static uint64_t max_global_memory = 1024ULL * 1024ULL * 1024ULL * 10; // 默认全局内存上限10GB
static pthread_mutex_t memory_count_mutex = PTHREAD_MUTEX_INITIALIZER;

// eBPF相关变量
static int process_resources_map_fd = -1;
static int global_resources_map_fd = -1;
static pthread_t sync_thread;
static pthread_mutex_t ebpf_sync_mutex = PTHREAD_MUTEX_INITIALIZER;

// 获取全局QP上限
static void update_max_global_qp(void) {
    // 从环境变量读取全局QP上限
    const char *max_qp_str = getenv("RDMA_INTERCEPT_MAX_GLOBAL_QP");
    if (max_qp_str) {
        uint32_t new_max = atoi(max_qp_str);
        if (new_max > 0) {
            pthread_mutex_lock(&qp_count_mutex);
            max_global_qp = new_max;
            pthread_mutex_unlock(&qp_count_mutex);
            printf("更新全局QP上限为: %u\n", new_max);
        }
    }
}

// 获取全局内存上限
static void update_max_global_memory(void) {
    // 从环境变量读取全局内存上限
    const char *max_memory_str = getenv("RDMA_INTERCEPT_MAX_GLOBAL_MEMORY");
    if (max_memory_str) {
        uint64_t new_max = atoll(max_memory_str);
        if (new_max > 0) {
            pthread_mutex_lock(&memory_count_mutex);
            max_global_memory = new_max;
            pthread_mutex_unlock(&memory_count_mutex);
            printf("更新全局内存上限为: %llu bytes\n", (unsigned long long)new_max);
        }
    }
}

// 信号处理函数
static void signal_handler(int sig __attribute__((unused)))
{
    running = 0;
    printf("\n收到信号，正在退出...\n");
}

// 从eBPF映射中读取数据并同步到共享内存
static void sync_ebpf_data_to_shared_memory(void)
{
    pthread_mutex_lock(&ebpf_sync_mutex);
    
    shared_memory_data_t* shm_data = shm_get_ptr();
    if (!shm_data) {
        pthread_mutex_unlock(&ebpf_sync_mutex);
        return;
    }

    // 首先同步全局资源数据
    if (global_resources_map_fd >= 0) {
        uint32_t key = 0;
        resource_usage_t global_usage = {};
        
        int lookup_err = bpf_map_lookup_elem(global_resources_map_fd, &key, &global_usage);
        if (lookup_err == 0) {
            printf("[COLLECTOR] 从eBPF map读取全局资源: QP=%d, MR=%d, Memory=%llu\n", 
                   global_usage.qp_count, global_usage.mr_count, (unsigned long long)global_usage.memory_used);
            // 更新共享内存中的全局计数
            shm_lock(shm_data);
            shm_data->global_stats.qp_count = global_usage.qp_count;
            shm_data->global_stats.mr_count = global_usage.mr_count;
            shm_data->global_stats.memory_used = global_usage.memory_used;
            shm_data->version++;
            shm_unlock(shm_data);
            printf("[COLLECTOR] 已更新共享内存中的全局计数: QP=%d, MR=%d, Memory=%llu\n", 
                   shm_data->global_stats.qp_count, shm_data->global_stats.mr_count, (unsigned long long)shm_data->global_stats.memory_used);
        } else {
            printf("[COLLECTOR] 从eBPF map读取全局资源失败: %d\n", lookup_err);
        }
    }
    
    // 同步进程资源数据
    if (process_resources_map_fd >= 0) {
        // 读取eBPF map中的所有进程资源数据
        uint32_t current_pid = 0;
        uint32_t next_pid = 0;
        resource_usage_t usage = {};
        
        // 遍历进程资源映射
        while (bpf_map_get_next_key(process_resources_map_fd, &current_pid, &next_pid) == 0) {
            if (bpf_map_lookup_elem(process_resources_map_fd, &next_pid, &usage) == 0) {
                // 更新共享内存中的进程资源数据
                shm_update_process_resources(next_pid, &usage);
            }
            current_pid = next_pid;
        }
    }
    
    pthread_mutex_unlock(&ebpf_sync_mutex);
}

// eBPF数据同步线程函数
static void *sync_thread_func(void *arg __attribute__((unused)))
{
    // 定期从eBPF maps同步数据到共享内存
    struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000000}; // 每100ms同步一次
    
    while (running) {
        sync_ebpf_data_to_shared_memory();
        nanosleep(&interval, NULL);
    }
    
    return NULL;
}

// ===== GC 线程：基于 pidfd + epoll 的事件驱动资源回收 =====

#define GC_EPOLL_MAX_EVENTS 64

static struct {
    int      pidfd;       // pidfd 文件描述符，-1 表示未注册
    pid_t    pid;         // 注册时的 PID
    uint64_t start_time;  // 注册时的启动时间，用于防 PID 重用
} gc_pidfd_table[MAX_PROCESSES];

static int         gc_epoll_fd = -1;
static pthread_t   gc_thread;
static volatile int gc_running = 0;

// 为指定共享内存槽位注册 pidfd
static void gc_register_pidfd(int slot, pid_t pid, uint64_t start_time)
{
    if (slot < 0 || slot >= MAX_PROCESSES) return;

    if (gc_pidfd_table[slot].pidfd >= 0) {
        epoll_ctl(gc_epoll_fd, EPOLL_CTL_DEL, gc_pidfd_table[slot].pidfd, NULL);
        close(gc_pidfd_table[slot].pidfd);
    }

    int pidfd = syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        gc_pidfd_table[slot].pidfd = -1;
        gc_pidfd_table[slot].pid = 0;
        gc_pidfd_table[slot].start_time = 0;
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = (uint64_t)(uint32_t)slot;
    if (epoll_ctl(gc_epoll_fd, EPOLL_CTL_ADD, pidfd, &ev) < 0) {
        close(pidfd);
        gc_pidfd_table[slot].pidfd = -1;
        gc_pidfd_table[slot].pid = 0;
        gc_pidfd_table[slot].start_time = 0;
        return;
    }

    gc_pidfd_table[slot].pidfd      = pidfd;
    gc_pidfd_table[slot].pid        = pid;
    gc_pidfd_table[slot].start_time = start_time;
}

// 检查进程是否真正存活（通过 /proc/[pid]/status 判断是否僵尸）
static int gc_is_process_really_alive(pid_t pid)
{
    char path[64];
    char buf[256];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0; // 进程彻底不存在（包括僵尸已被回收）
    int alive = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "State:", 6) == 0) {
            for (char *p = buf + 6; *p; p++) {
                if (*p == 'Z' || *p == 'X') { // Zombie 或 Dead
                    alive = 0;
                    break;
                } else if (*p == 'S' || *p == 'R' || *p == 'D' || *p == 'T') {
                    alive = 1;
                    break;
                }
            }
            break;
        }
    }
    fclose(f);
    return alive;
}

// EPOLLIN 事件处理：进程已退出，回收其资源
static void gc_handle_process_exit(int slot)
{
    shared_memory_data_t *shm = shm_get_ptr();
    if (!shm) return;

    int dead_qp = 0, dead_mr = 0;
    uint64_t dead_mem = 0;
    pid_t target_pid = 0;

    // 检查启动时间是否一致，防止 PID 被重用后误回收
    shm_lock(shm);
    int slot_changed = (shm->process_pids[slot] != gc_pidfd_table[slot].pid) ||
                       (shm->process_start_times[slot] != gc_pidfd_table[slot].start_time);

    if (!slot_changed && shm->process_pids[slot] != 0) {
        target_pid = shm->process_pids[slot];
        dead_qp   = shm->process_stats[slot].qp_count;
        dead_mr   = shm->process_stats[slot].mr_count;
        dead_mem  = shm->process_stats[slot].memory_used;

        memset(&shm->process_stats[slot], 0, sizeof(resource_usage_t));
        shm->process_pids[slot] = 0;
        shm->process_start_times[slot] = 0;
        shm->global_stats.qp_count  -= dead_qp;
        shm->global_stats.mr_count  -= dead_mr;
        shm->global_stats.memory_used -= dead_mem;
        shm->version++;

        fprintf(stderr, "[GC] 回收进程 %d: QP=%d, MR=%d, Memory=%lu\n",
                target_pid, dead_qp, dead_mr, dead_mem);
    }
    shm_unlock(shm);

    // 调整租户计数（已释放 shm 锁，避免锁顺序死锁）
    if (target_pid > 0 && dead_qp > 0) {
        uint32_t tenant_id = 0;
        int ret = tenant_get_process_tenant(target_pid, &tenant_id);
        if (ret == 0 && tenant_id > 0) {
            tenant_resource_usage_t tu = {0};
            if (tenant_get_resource_usage(tenant_id, &tu) == 0) {
                tu.qp_count  = (tu.qp_count > dead_qp) ? tu.qp_count - dead_qp : 0;
                tu.mr_count  = (tu.mr_count > dead_mr) ? tu.mr_count - dead_mr : 0;
                tu.memory_used = (tu.memory_used > dead_mem) ? tu.memory_used - dead_mem : 0;
                tenant_update_resource_usage(tenant_id, &tu);
                fprintf(stderr, "[GC] 调整租户 %u 资源: QP-= %d, MR-= %d, Mem-= %lu\n",
                        tenant_id, dead_qp, dead_mr, dead_mem);
            }
        }
    }

    // 清理 pidfd
    if (gc_pidfd_table[slot].pidfd >= 0) {
        epoll_ctl(gc_epoll_fd, EPOLL_CTL_DEL, gc_pidfd_table[slot].pidfd, NULL);
        close(gc_pidfd_table[slot].pidfd);
    }
    gc_pidfd_table[slot].pidfd = -1;
    gc_pidfd_table[slot].pid   = 0;
    gc_pidfd_table[slot].start_time = 0;
}

// 扫描共享内存，为新出现的进程注册 pidfd；同时通过 /proc kill(0) 兜底
static void gc_scan_and_register(void)
{
    shared_memory_data_t *shm = shm_get_ptr();
    if (!shm) return;

    shm_lock(shm);
    for (int slot = 0; slot < MAX_PROCESSES; slot++) {
        pid_t pid = shm->process_pids[slot];
        uint64_t st = shm->process_start_times[slot];

        // 情况 00：共享内存中槽位为空
        if (pid == 0) {
            if (gc_pidfd_table[slot].pidfd >= 0) {
                epoll_ctl(gc_epoll_fd, EPOLL_CTL_DEL, gc_pidfd_table[slot].pidfd, NULL);
                close(gc_pidfd_table[slot].pidfd);
                gc_pidfd_table[slot].pidfd = -1;
                gc_pidfd_table[slot].pid   = 0;
                gc_pidfd_table[slot].start_time = 0;
            }
            continue;
        }

        // 情况 1：尚未注册 pidfd
        if (gc_pidfd_table[slot].pidfd < 0) {
            gc_register_pidfd(slot, pid, st);
            if (gc_pidfd_table[slot].pidfd < 0 && !gc_is_process_really_alive(pid)) {
                // pidfd_open 失败且进程已死（含僵尸态），通过 /proc 兜底回收
                int dead_qp   = shm->process_stats[slot].qp_count;
                int dead_mr   = shm->process_stats[slot].mr_count;
                uint64_t dead_mem = shm->process_stats[slot].memory_used;

                memset(&shm->process_stats[slot], 0, sizeof(resource_usage_t));
                shm->process_pids[slot] = 0;
                shm->process_start_times[slot] = 0;
                shm->global_stats.qp_count  -= dead_qp;
                shm->global_stats.mr_count  -= dead_mr;
                shm->global_stats.memory_used -= dead_mem;
                shm->version++;

                fprintf(stderr, "[GC] (/proc 兜底) 回收进程 %d: QP=%d, MR=%d, Memory=%lu\n",
                        pid, dead_qp, dead_mr, dead_mem);
                (void)dead_qp; (void)dead_mr; (void)dead_mem;
            }
            continue;
        }

        // 情况 2：已注册但 PID / 启动时间变了 ⇒ 旧进程死，新进程占用了槽位
        if (gc_pidfd_table[slot].pid != pid ||
            gc_pidfd_table[slot].start_time != st) {
            if (gc_pidfd_table[slot].pidfd >= 0) {
                close(gc_pidfd_table[slot].pidfd);
            }
            gc_pidfd_table[slot].pidfd = -1;
            gc_pidfd_table[slot].pid   = 0;
            gc_pidfd_table[slot].start_time = 0;
            gc_register_pidfd(slot, pid, st);
            continue;
        }

        // 情况 3：已注册 pidfd，但 epoll 事件可能丢失
        // 通过 /proc 健康检查确认进程是否存
        if (!gc_is_process_really_alive(pid)) {
            int dead_qp   = shm->process_stats[slot].qp_count;
            int dead_mr   = shm->process_stats[slot].mr_count;
            uint64_t dead_mem = shm->process_stats[slot].memory_used;

            // 清理共享内存条目
            memset(&shm->process_stats[slot], 0, sizeof(resource_usage_t));
            shm->process_pids[slot] = 0;
            shm->process_start_times[slot] = 0;
            shm->global_stats.qp_count  -= dead_qp;
            shm->global_stats.mr_count  -= dead_mr;
            shm->global_stats.memory_used -= dead_mem;
            shm->version++;

            fprintf(stderr, "[GC] (健康检查) 回收进程 %d: QP=%d, MR=%d, Memory=%lu\n",
                    pid, dead_qp, dead_mr, dead_mem);

            // 清理 pidfd
            if (gc_pidfd_table[slot].pidfd >= 0) {
                epoll_ctl(gc_epoll_fd, EPOLL_CTL_DEL, gc_pidfd_table[slot].pidfd, NULL);
                close(gc_pidfd_table[slot].pidfd);
            }
            gc_pidfd_table[slot].pidfd = -1;
            gc_pidfd_table[slot].pid   = 0;
            gc_pidfd_table[slot].start_time = 0;

            (void)dead_qp; (void)dead_mr; (void)dead_mem;
        }
    }
    shm_unlock(shm);
}

// GC 线程主循环
static void *gc_thread_func(void *arg __attribute__((unused)))
{
    gc_epoll_fd = epoll_create1(0);
    if (gc_epoll_fd < 0) {
        fprintf(stderr, "[GC] epoll_create1 失败: %s\n", strerror(errno));
        return NULL;
    }

    memset(gc_pidfd_table, 0, sizeof(gc_pidfd_table));

    // 启动时扫描已有进程并注册 pidfd
    gc_scan_and_register();

    gc_running = 1;

    struct epoll_event events[GC_EPOLL_MAX_EVENTS];

    while (running) {
        int nfds = epoll_wait(gc_epoll_fd, events, GC_EPOLL_MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) break;
            fprintf(stderr, "[GC] epoll_wait 错误: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].events & EPOLLIN) {
                int slot = (int)(uint32_t)events[i].data.u64;
                gc_handle_process_exit(slot);
            }
        }

        gc_scan_and_register();
    }

    // 清理所有 pidfd
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (gc_pidfd_table[i].pidfd >= 0) {
            close(gc_pidfd_table[i].pidfd);
        }
    }

    if (gc_epoll_fd >= 0) {
        close(gc_epoll_fd);
        gc_epoll_fd = -1;
    }

    gc_running = 0;
    fprintf(stderr, "[GC] pidfd+epoll GC 线程已退出\n");
    return NULL;
}

// 初始化数据收集服务
static int initialize_service(void)
{
    // 初始化共享内存
    if (shm_init() != 0) {
        fprintf(stderr, "初始化共享内存失败\n");
        return -1;
    }
    
    // 从环境变量读取全局QP上限
    update_max_global_qp();
    
    // 从环境变量读取全局内存上限
    update_max_global_memory();
    
    // 设置全局资源限制
    shm_set_global_limits(max_global_qp, 1000, max_global_memory);
    
    // 获取eBPF映射文件描述符
    process_resources_map_fd = bpf_obj_get("/sys/fs/bpf/process_resources");
    if (process_resources_map_fd < 0) {
        fprintf(stderr, "获取进程资源eBPF映射文件描述符失败: %d (errno: %s)\n", errno, strerror(errno));
        fprintf(stderr, "提示: 请确保eBPF程序已加载并将maps挂载到bpffs\n");
    } else {
        printf("成功获取进程资源eBPF映射文件描述符: %d\n", process_resources_map_fd);
    }
    
    global_resources_map_fd = bpf_obj_get("/sys/fs/bpf/global_resources");
    if (global_resources_map_fd < 0) {
        fprintf(stderr, "获取全局资源eBPF映射文件描述符失败: %d (errno: %s)\n", errno, strerror(errno));
        fprintf(stderr, "提示: 请确保eBPF程序已加载并将maps挂载到bpffs\n");
    } else {
        printf("成功获取全局资源eBPF映射文件描述符: %d\n", global_resources_map_fd);
    }
    
    // 启动数据同步线程
    int ret = pthread_create(&sync_thread, NULL, sync_thread_func, NULL);
    if (ret != 0) {
        fprintf(stderr, "创建数据同步线程失败: %d\n", ret);
        return -1;
    }
    
    // 启动 GC 线程
    ret = pthread_create(&gc_thread, NULL, gc_thread_func, NULL);
    if (ret != 0) {
        fprintf(stderr, "创建 GC 线程失败: %d\n", ret);
        return -1;
    }
    
    printf("数据收集服务初始化成功，共享内存已准备就绪\n");
    return 0;
}

// 清理资源
static void cleanup_service(void)
{
    running = 0;
    
    if (sync_thread) {
        pthread_join(sync_thread, NULL);
    }
    
    if (gc_running) {
        pthread_join(gc_thread, NULL);
    }
    
    if (process_resources_map_fd >= 0) {
        close(process_resources_map_fd);
    }
    
    if (global_resources_map_fd >= 0) {
        close(global_resources_map_fd);
    }
    
    printf("数据收集服务已停止\n");
}

// 主函数
int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    printf("基于共享内存的数据收集服务启动中...\n");
    printf("从环境变量读取全局QP上限: %s -> %d\n", 
           getenv("RDMA_INTERCEPT_MAX_GLOBAL_QP") ? : "NULL", 
           max_global_qp);
    
    if (getenv("RDMA_INTERCEPT_MAX_GLOBAL_MEMORY")) {
        printf("未找到RDMA_INTERCEPT_MAX_GLOBAL_MEMORY环境变量\n");
    }
    
    // 注册信号处理器
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // 初始化服务
    if (initialize_service() != 0) {
        fprintf(stderr, "服务初始化失败\n");
        return 1;
    }
    
    printf("基于共享内存的数据收集服务已启动，正在同步eBPF数据到共享内存...\n");
    printf("按Ctrl+C退出\n");
    
    // 主循环 - 持续运行直到收到信号
    while (running) {
        sleep(1);
    }
    
    // 清理资源
    cleanup_service();
    
    return 0;
}