#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include "shared_memory.h"

// 全局共享内存文件描述符和指针
static int shm_fd = -1;
static shared_memory_data_t* shm_data_ptr = NULL;

// 获取当前时间戳（纳秒）
static uint64_t get_current_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int shm_init(void) {
    // 创建或打开共享内存对象
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("[SHM] shm_open failed");
        return -1;
    }

    // 设置共享内存大小
    if (ftruncate(shm_fd, sizeof(shared_memory_data_t)) == -1) {
        perror("[SHM] ftruncate failed");
        close(shm_fd);
        shm_fd = -1;
        return -1;
    }

    // 设置共享内存权限，确保普通用户进程也能写入
    fchmod(shm_fd, 0666);

    // 映射共享内存到进程地址空间
    shm_data_ptr = (shared_memory_data_t*)mmap(NULL, sizeof(shared_memory_data_t),
                                              PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_data_ptr == MAP_FAILED) {
        perror("[SHM] mmap failed");
        close(shm_fd);
        shm_fd = -1;
        shm_data_ptr = NULL;
        return -1;
    }

    // 初始化共享内存数据（仅首次创建时）
    if (shm_data_ptr->global_stats.qp_count == 0 &&
        shm_data_ptr->global_stats.mr_count == 0 &&
        shm_data_ptr->global_stats.memory_used == 0) {
        // 初始化全局统计
        shm_data_ptr->global_stats.qp_count = 0;
        shm_data_ptr->global_stats.mr_count = 0;
        shm_data_ptr->global_stats.memory_used = 0;

        // 初始化进程统计数组
        for (int i = 0; i < MAX_PROCESSES; i++) {
            shm_data_ptr->process_stats[i].qp_count = 0;
            shm_data_ptr->process_stats[i].mr_count = 0;
            shm_data_ptr->process_stats[i].memory_used = 0;
            shm_data_ptr->process_pids[i] = 0;
            shm_data_ptr->process_start_times[i] = 0;
        }

        // 初始化全局配置
        shm_data_ptr->max_global_qp = 1000;  // 默认值
        shm_data_ptr->max_global_mr = 1000;  // 默认值
        shm_data_ptr->max_global_memory = 1024UL * 1024UL * 1024UL;  // 1GB 默认值

        // 初始化同步机制
        shm_data_ptr->shm_lock = 0;

        // 初始化版本号和时间戳
        shm_data_ptr->version = 1;
        shm_data_ptr->last_update_time = get_current_time_ns();
    }

    fprintf(stderr, "[SHM] Shared memory initialized successfully\n");
    return 0;
}

int shm_destroy(void) {
    if (shm_data_ptr != NULL) {
        munmap(shm_data_ptr, sizeof(shared_memory_data_t));
        shm_data_ptr = NULL;
    }

    if (shm_fd != -1) {
        close(shm_fd);
        shm_fd = -1;
    }

    // 删除共享内存对象
    if (shm_unlink(SHM_NAME) == -1) {
        perror("[SHM] shm_unlink failed");
        return -1;
    }

    fprintf(stderr, "[SHM] Shared memory destroyed successfully\n");
    return 0;
}

shared_memory_data_t* shm_get_ptr(void) {
    return shm_data_ptr;
}

// 自旋锁实现
void shm_lock(shared_memory_data_t* data) {
    int expected;
    do {
        expected = 0;
    } while (!__atomic_compare_exchange_n(&data->shm_lock, &expected, 1, 
                                         false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
}

void shm_unlock(shared_memory_data_t* data) {
    __atomic_store_n(&data->shm_lock, 0, __ATOMIC_RELEASE);
}

int shm_get_global_resources(resource_usage_t* usage) {
    if (!usage || !shm_data_ptr) {
        return -1;
    }

    // 简单复制，不加锁以提高性能，可能存在轻微数据不一致（可接受）
    usage->qp_count = shm_data_ptr->global_stats.qp_count;
    usage->mr_count = shm_data_ptr->global_stats.mr_count;
    usage->memory_used = shm_data_ptr->global_stats.memory_used;

    return 0;
}

int shm_get_process_resources(pid_t pid, resource_usage_t* usage) {
    if (!usage || !shm_data_ptr || pid <= 0) {
        return -1;
    }

    // 查找对应进程的资源统计
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (shm_data_ptr->process_pids[i] == pid) {
            usage->qp_count = shm_data_ptr->process_stats[i].qp_count;
            usage->mr_count = shm_data_ptr->process_stats[i].mr_count;
            usage->memory_used = shm_data_ptr->process_stats[i].memory_used;
            return 0;
        }
    }

    // 如果没找到，返回0值
    usage->qp_count = 0;
    usage->mr_count = 0;
    usage->memory_used = 0;
    return 0; // 成功但未找到特定进程信息
}

int shm_update_global_resources(const resource_usage_t* usage) {
    if (!usage || !shm_data_ptr) {
        return -1;
    }

    shm_lock(shm_data_ptr);

    shm_data_ptr->global_stats.qp_count = usage->qp_count;
    shm_data_ptr->global_stats.mr_count = usage->mr_count;
    shm_data_ptr->global_stats.memory_used = usage->memory_used;

    // 更新版本号和时间戳
    shm_data_ptr->version++;
    shm_data_ptr->last_update_time = get_current_time_ns();

    shm_unlock(shm_data_ptr);

    return 0;
}

int shm_update_process_resources(pid_t pid, const resource_usage_t* usage) {
    if (!usage || !shm_data_ptr || pid <= 0) {
        return -1;
    }

    shm_lock(shm_data_ptr);

    // 查找现有条目或寻找空槽
    int slot = -1;
    int empty_slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (shm_data_ptr->process_pids[i] == pid) {
            slot = i;
            break;
        } else if (shm_data_ptr->process_pids[i] == 0 && empty_slot == -1) {
            empty_slot = i;
        }
    }

    if (slot >= 0) {
        // 找到现有条目，检查 PID 是否被重用（启动时间不一致）
        uint64_t current_start = shm_read_proc_start_time(pid);
        if (current_start != 0 && shm_data_ptr->process_start_times[slot] != 0 &&
            current_start != shm_data_ptr->process_start_times[slot]) {
            // PID 被重用！原进程已死，先清除旧资源
            shm_data_ptr->global_stats.qp_count -= shm_data_ptr->process_stats[slot].qp_count;
            shm_data_ptr->global_stats.mr_count -= shm_data_ptr->process_stats[slot].mr_count;
            shm_data_ptr->global_stats.memory_used -= shm_data_ptr->process_stats[slot].memory_used;
            // 清零进程统计，确保后续 (usage->qp_count - old_qp) 计算正确
            memset(&shm_data_ptr->process_stats[slot], 0, sizeof(resource_usage_t));
            shm_data_ptr->process_start_times[slot] = current_start;
        }
    } else if (empty_slot >= 0) {
        // 新增进程条目
        slot = empty_slot;
        shm_data_ptr->process_pids[slot] = pid;
        shm_data_ptr->process_start_times[slot] = shm_read_proc_start_time(pid);
    } else {
        // 没有空槽
        shm_unlock(shm_data_ptr);
        return -1;
    }

    int old_qp = shm_data_ptr->process_stats[slot].qp_count;
    int old_mr = shm_data_ptr->process_stats[slot].mr_count;
    uint64_t old_mem = shm_data_ptr->process_stats[slot].memory_used;

    shm_data_ptr->process_stats[slot].qp_count = usage->qp_count;
    shm_data_ptr->process_stats[slot].mr_count = usage->mr_count;
    shm_data_ptr->process_stats[slot].memory_used = usage->memory_used;

    shm_data_ptr->global_stats.qp_count += (usage->qp_count - old_qp);
    shm_data_ptr->global_stats.mr_count += (usage->mr_count - old_mr);
    shm_data_ptr->global_stats.memory_used += (usage->memory_used - old_mem);

    // 更新版本号和时间戳
    shm_data_ptr->version++;
    shm_data_ptr->last_update_time = get_current_time_ns();

    shm_unlock(shm_data_ptr);

    return 0;
}

/* 读取 /proc/[pid]/stat 获取进程启动时间（jiffies since boot） */
uint64_t shm_read_proc_start_time(pid_t pid) {
    char path[64];
    char buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);

    // 找到最后一个 ')'，跳过 ") " 到达 state 字段
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    p += 2;
    if (*p == '\0') return 0;

    // state 之后跳过的字段: state, ppid, pgid, sid, tty_nr, tty_pgrp, flags,
    // min_flt, cmin_flt, maj_flt, cmaj_flt, utime, stime, cutime, cstime,
    // priority, nice, num_threads, itrealvalue = 19 fields total
    // 然后第 20 个字段是 starttime
    char *saveptr = NULL;
    char *tok = strtok_r(p, " ", &saveptr);
    if (!tok) return 0;
    for (int i = 0; i < 19; i++) {
        tok = strtok_r(NULL, " ", &saveptr);
        if (!tok) return 0;
    }
    tok = strtok_r(NULL, " ", &saveptr);
    if (!tok) return 0;
    return strtoull(tok, NULL, 10);
}

/* 清空指定进程条目并在全局计数中减去其占用的资源 */
int shm_clear_process_entry(int slot, int *out_qp, int *out_mr, uint64_t *out_mem) {
    if (!shm_data_ptr || slot < 0 || slot >= MAX_PROCESSES) {
        return -1;
    }
    if (shm_data_ptr->process_pids[slot] == 0) {
        return -1;
    }

    if (out_qp) *out_qp = shm_data_ptr->process_stats[slot].qp_count;
    if (out_mr) *out_mr = shm_data_ptr->process_stats[slot].mr_count;
    if (out_mem) *out_mem = shm_data_ptr->process_stats[slot].memory_used;

    shm_data_ptr->global_stats.qp_count  -= shm_data_ptr->process_stats[slot].qp_count;
    shm_data_ptr->global_stats.mr_count  -= shm_data_ptr->process_stats[slot].mr_count;
    shm_data_ptr->global_stats.memory_used -= shm_data_ptr->process_stats[slot].memory_used;

    memset(&shm_data_ptr->process_stats[slot], 0, sizeof(resource_usage_t));
    shm_data_ptr->process_pids[slot] = 0;
    shm_data_ptr->process_start_times[slot] = 0;

    shm_data_ptr->version++;

    return 0;
}

int shm_set_global_limits(uint32_t max_qp, uint32_t max_mr, uint64_t max_memory) {
    if (!shm_data_ptr) {
        return -1;
    }

    shm_lock(shm_data_ptr);

    shm_data_ptr->max_global_qp = max_qp;
    shm_data_ptr->max_global_mr = max_mr;
    shm_data_ptr->max_global_memory = max_memory;

    shm_unlock(shm_data_ptr);

    return 0;
}