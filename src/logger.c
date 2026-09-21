#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include "rdma_intercept.h"


static pthread_mutex_t log_file_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 获取当前时间字符串 */
static void get_current_time_string(char *buffer, size_t size) {
    time_t now;
    struct tm *tm_info;
    struct timespec ts;
    
    time(&now);
    tm_info = localtime(&now);
    clock_gettime(CLOCK_REALTIME, &ts);
    
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             ts.tv_nsec / 1000000);
}

/* 获取日志级别字符串 */
static const char *get_log_level_string(log_level_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_FATAL: return "FATAL";
        default:              return "UNKNOWN";
    }
}

/* 确保日志目录存在 */
static int ensure_log_directory(const char *log_path) {
    char dir_path[MAX_CONFIG_FILE_PATH];
    char *last_slash;
    
    /* 复制路径 */
    strncpy(dir_path, log_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    
    /* 找到最后一个'/' */
    last_slash = strrchr(dir_path, '/');
    if (last_slash && last_slash != dir_path) {
        *last_slash = '\0';
        
        /* 检查目录是否存在 */
        struct stat st;
        if (stat(dir_path, &st) != 0) {
            /* 创建目录 */
            if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
        }
    }
    
    return 0;
}

/* 内部日志函数 */
void rdma_intercept_log_internal(log_level_t level, const char *file, int line, 
                                const char *func, const char *format, ...) {
    if (!g_intercept_state.initialized || level < g_intercept_state.config.log_level) {
        return;
    }

    va_list args;
    char time_str[64];
    const char *level_str;
    char local_buffer[MAX_LOG_BUFFER_SIZE];
    
    /* 预格式化消息，减少锁持有时间 */
    va_start(args, format);
    vsnprintf(local_buffer, sizeof(local_buffer), format, args);
    va_end(args);
    
    /* 获取时间 */
    get_current_time_string(time_str, sizeof(time_str));
    
    /* 获取日志级别 */
    level_str = get_log_level_string(level);
    
    /* 写入日志文件（只在必要时加锁） */
    if (g_intercept_state.log_file) {
        pthread_mutex_lock(&log_file_mutex);
        if (g_intercept_state.log_file) {
            if (g_intercept_state.config.log_level <= LOG_LEVEL_DEBUG) {
                /* 调试模式包含文件位置信息 */
                fprintf(g_intercept_state.log_file, "[%s] [%s] [PID:%d] [%s:%d] %s: %s\n",
                        time_str, level_str, getpid(), file, line, func, local_buffer);
            } else {
                /* 普通模式 */
                fprintf(g_intercept_state.log_file, "[%s] [%s] [PID:%d] %s\n",
                        time_str, level_str, getpid(), local_buffer);
            }
            /* 减少fflush调用，只在关键级别或配置要求时刷新 */
            if (level >= LOG_LEVEL_ERROR || g_intercept_state.config.log_level <= LOG_LEVEL_DEBUG) {
                fflush(g_intercept_state.log_file);
            }
        }
        pthread_mutex_unlock(&log_file_mutex);
    }
    
    /* 致命错误时同时输出到stderr */
    if (level == LOG_LEVEL_FATAL) {
        fprintf(stderr, "[RDMA_INTERCEPT] [%s] %s\n", level_str, local_buffer);
    }
}

/* 设置日志级别 */
int rdma_intercept_set_log_level(log_level_t level) {
    if (level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_FATAL) {
        return -1;
    }
    
    g_intercept_state.config.log_level = level;
    rdma_intercept_log(LOG_LEVEL_INFO, "Log level changed to %s", get_log_level_string(level));
    return 0;
}

/* 获取日志级别 */
log_level_t rdma_intercept_get_log_level(void) {
    return g_intercept_state.config.log_level;
}

/* 设置日志文件 */
int rdma_intercept_set_log_file(const char *log_file_path) {
    if (!log_file_path || strlen(log_file_path) >= MAX_CONFIG_FILE_PATH) {
        return -1;
    }
    
    /* 确保日志目录存在 */
    if (ensure_log_directory(log_file_path) != 0) {
        return -1;
    }
    
    pthread_mutex_lock(&log_file_mutex);
    
    /* 关闭旧的日志文件 */
    if (g_intercept_state.log_file && g_intercept_state.log_file != stdout) {
        fclose(g_intercept_state.log_file);
    }
    
    /* 打开新的日志文件 */
    FILE *new_file = fopen(log_file_path, "a");
    if (!new_file) {
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }
    
    /* 更新配置 */
    strncpy(g_intercept_state.config.log_file_path, log_file_path, MAX_CONFIG_FILE_PATH - 1);
    g_intercept_state.config.log_file_path[MAX_CONFIG_FILE_PATH - 1] = '\0';
    g_intercept_state.log_file = new_file;
    
    pthread_mutex_unlock(&log_file_mutex);
    
    rdma_intercept_log(LOG_LEVEL_INFO, "Log file changed to %s", log_file_path);
    return 0;
}

/* 日志轮转 */
int rdma_intercept_rotate_log(void) {
    if (!g_intercept_state.log_file || !g_intercept_state.config.log_file_path[0]) {
        return -1;
    }
    
    pthread_mutex_lock(&log_file_mutex);
    
    /* 关闭当前文件 */
    fclose(g_intercept_state.log_file);
    
    /* 生成备份文件名 */
    char backup_path[MAX_CONFIG_FILE_PATH + 32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    
    snprintf(backup_path, sizeof(backup_path), "%s.%04d%02d%02d-%02d%02d%02d",
             g_intercept_state.config.log_file_path,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    /* 重命名当前日志文件 */
    rename(g_intercept_state.config.log_file_path, backup_path);
    
    /* 创建新的日志文件 */
    FILE *new_file = fopen(g_intercept_state.config.log_file_path, "a");
    if (!new_file) {
        /* 恢复原来的文件 */
        rename(backup_path, g_intercept_state.config.log_file_path);
        pthread_mutex_unlock(&log_file_mutex);
        return -1;
    }
    
    g_intercept_state.log_file = new_file;
    
    pthread_mutex_unlock(&log_file_mutex);
    
    rdma_intercept_log(LOG_LEVEL_INFO, "Log rotated to %s", backup_path);
    return 0;
}

/* 获取日志统计信息 */
void rdma_intercept_get_log_stats(size_t *total_messages, size_t *error_count, 
                                 size_t *warn_count, time_t *start_time) {
    static size_t message_count = 0;
    static size_t err_count = 0;
    static size_t wrn_count = 0;
    static time_t log_start_time = 0;
    static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
    
    pthread_mutex_lock(&stats_mutex);
    
    if (log_start_time == 0) {
        log_start_time = time(NULL);
    }
    
    if (total_messages) *total_messages = message_count;
    if (error_count) *error_count = err_count;
    if (warn_count) *warn_count = wrn_count;
    if (start_time) *start_time = log_start_time;
    
    pthread_mutex_unlock(&stats_mutex);
}

/* QP创建信息日志函数 */
void rdma_intercept_log_qp_creation(const qp_creation_info_t *info) {
    if (!g_intercept_state.initialized || 
        !g_intercept_state.config.log_qp_creation ||
        !info) {
        return;
    }

    char time_str[64];
    struct tm *tm_info;
    
    /* 转换时间戳为人类可读格式（预计算，减少锁持有时间） */
    tm_info = localtime(&info->timestamp);
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    /* 写入QP创建详细信息到日志文件（只在必要时加锁） */
    if (g_intercept_state.log_file) {
        pthread_mutex_lock(&log_file_mutex);
        if (g_intercept_state.log_file) {
            fprintf(g_intercept_state.log_file, "===== QP Creation Event =====\n");
            fprintf(g_intercept_state.log_file, "[Timestamp] %s\n", time_str);
            fprintf(g_intercept_state.log_file, "[PID] %d\n", info->pid);
            fprintf(g_intercept_state.log_file, "[TID] %lu\n", info->tid);
            fprintf(g_intercept_state.log_file, "[QP Address] %p\n", info->pd ? info->pd : NULL);
            fprintf(g_intercept_state.log_file, "[Context] %p\n", info->context);
            fprintf(g_intercept_state.log_file, "[QP Type] %d\n", info->qp_type);
            fprintf(g_intercept_state.log_file, "[Max Send WR] %u\n", info->max_send_wr);
            fprintf(g_intercept_state.log_file, "[Max Recv WR] %u\n", info->max_recv_wr);
            fprintf(g_intercept_state.log_file, "[Max Send SGE] %u\n", info->max_send_sge);
            fprintf(g_intercept_state.log_file, "[Max Recv SGE] %u\n", info->max_recv_sge);
            fprintf(g_intercept_state.log_file, "[Max Inline Data] %u bytes\n", info->max_inline_data);
            fprintf(g_intercept_state.log_file, "[Send CQ] %p\n", info->send_cq);
            fprintf(g_intercept_state.log_file, "[Recv CQ] %p\n", info->recv_cq);
            fprintf(g_intercept_state.log_file, "[SRQ] %p\n", info->srq);
            fprintf(g_intercept_state.log_file, "=============================\n");
            /* 减少fflush调用，只在必要时刷新 */
            if (g_intercept_state.config.log_level <= LOG_LEVEL_DEBUG) {
                fflush(g_intercept_state.log_file);
            }
        }
        pthread_mutex_unlock(&log_file_mutex);
    }
}

/* 简化的日志函数（供其他模块调用） */
void rdma_intercept_log(log_level_t level, const char *format, ...) {
    if (!g_intercept_state.initialized || level < g_intercept_state.config.log_level) {
        return;
    }

    va_list args;
    char time_str[64];
    const char *level_str;
    char local_buffer[MAX_LOG_BUFFER_SIZE];
    
    /* 预格式化消息，减少锁持有时间 */
    va_start(args, format);
    vsnprintf(local_buffer, sizeof(local_buffer), format, args);
    va_end(args);
    
    /* 获取时间 */
    get_current_time_string(time_str, sizeof(time_str));
    
    /* 获取日志级别 */
    level_str = get_log_level_string(level);
    
    /* 写入日志文件（只在必要时加锁） */
    if (g_intercept_state.log_file) {
        pthread_mutex_lock(&log_file_mutex);
        if (g_intercept_state.log_file) {
            fprintf(g_intercept_state.log_file, "[%s] [%s] [PID:%d] %s\n",
                    time_str, level_str, getpid(), local_buffer);
            /* 减少fflush调用，只在关键级别或配置要求时刷新 */
            if (level >= LOG_LEVEL_ERROR || g_intercept_state.config.log_level <= LOG_LEVEL_DEBUG) {
                fflush(g_intercept_state.log_file);
            }
        }
        pthread_mutex_unlock(&log_file_mutex);
    }
}