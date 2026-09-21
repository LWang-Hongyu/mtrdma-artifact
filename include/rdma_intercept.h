#ifndef RDMA_INTERCEPT_H
#define RDMA_INTERCEPT_H

/* 禁用调试日志以提高性能 */
#ifdef NO_DEBUG
  #include <stdio.h>
  #define debug_fprintf(stream, fmt, ...) ((void)0)
  #define INTERCEPT_LOG(stream, ...) ((void)0)
#else
  #define debug_fprintf(stream, fmt, ...) fprintf(stream, fmt, ##__VA_ARGS__)
  #define INTERCEPT_LOG(stream, ...) fprintf(stream, ##__VA_ARGS__)
#endif

#include <stdint.h>
#include <stdbool.h>
#include <infiniband/verbs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 版本信息 */
#define RDMA_INTERCEPT_VERSION_MAJOR 1
#define RDMA_INTERCEPT_VERSION_MINOR 0
#define RDMA_INTERCEPT_VERSION_PATCH 0

/* 配置常量 */
#define MAX_LOG_BUFFER_SIZE 4096
#define MAX_CONFIG_FILE_PATH 256
#define MAX_INTERCEPT_FUNCTIONS 64

/* 日志级别 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3,
    LOG_LEVEL_FATAL = 4
} log_level_t;

/* 配置结构 */
typedef struct {
    bool enable_intercept;        /* 启用拦截 */
    bool log_qp_creation;         /* 记录QP创建信息 */
    bool log_all_operations;      /* 记录所有操作 */
    log_level_t log_level;        /* 日志级别 */
    char log_file_path[MAX_CONFIG_FILE_PATH]; /* 日志文件路径 */
    
    /* 资源管理和性能隔离配置 */
    bool enable_qp_control;       /* 启用QP控制 */
    uint32_t max_qp_per_process;  /* 每个进程的最大QP数量 */
    uint32_t max_send_wr_limit;   /* 最大发送工作请求数限制 */
    uint32_t max_recv_wr_limit;   /* 最大接收工作请求数限制 */
    bool allow_rc_qp;             /* 允许RC类型QP */
    bool allow_uc_qp;             /* 允许UC类型QP */
    bool allow_ud_qp;             /* 允许UD类型QP */
    bool allow_rq_qp;             /* 允许RQ类型QP */
    
    /* 全局资源管理配置 */
    uint32_t max_global_qp;       /* 全局最大QP数量 */
    
    /* 内存资源管理配置 */
    bool enable_mr_control;       /* 启用内存区域控制 */
    uint32_t max_mr_per_process;  /* 每个进程的最大MR数量 */
    uint64_t max_memory_per_process; /* 每个进程的最大内存使用量（字节） */
    
    /* MR操作速率限制配置 */
    bool enable_mr_rate_limit;    /* 启用MR操作速率限制 */
    uint32_t max_mr_ops_per_sec;  /* 每秒最大MR操作次数（注册+注销） */
    uint32_t mr_rate_limit_window_ms; /* 速率限制时间窗口（毫秒） */
} intercept_config_t;

/* QP创建信息 */
typedef struct {
    time_t timestamp;           /* 时间戳 */
    pid_t pid;                  /* 进程ID */
    pthread_t tid;              /* 线程ID */
    struct ibv_context *context;  /* RDMA上下文 */
    struct ibv_qp_init_attr *init_attr;  /* QP初始化属性 */
    struct ibv_pd *pd;          /* 保护域 */
    struct ibv_cq *send_cq;     /* 发送完成队列 */
    struct ibv_cq *recv_cq;     /* 接收完成队列 */
    struct ibv_srq *srq;        /* 共享接收队列 */
    int qp_type;                /* QP类型 */
    uint32_t max_send_wr;       /* 最大发送工作请求数 */
    uint32_t max_recv_wr;       /* 最大接收工作请求数 */
    uint32_t max_send_sge;      /* 最大发送SGE数 */
    uint32_t max_recv_sge;      /* 最大接收SGE数 */
    uint32_t max_inline_data;   /* 最大内联数据大小 */
} qp_creation_info_t;

/* 函数拦截状态 */
typedef struct {
    bool initialized;           /* 是否已初始化 */
    intercept_config_t config;  /* 配置信息 */
    FILE *log_file;            /* 日志文件句柄 */
    pthread_mutex_t log_mutex; /* 日志互斥锁 */
    pthread_mutex_t resource_mutex; /* 资源计数互斥锁 */
    void *original_functions[MAX_INTERCEPT_FUNCTIONS];  /* 原始函数指针 */
    
    /* 资源管理和性能隔离状态 */
    uint32_t qp_count;          /* 当前进程QP计数 */
    void *qp_list;              /* QP列表（预留） */
    
    /* 内存资源管理状态 */
    uint32_t mr_count;          /* 当前进程MR计数 */
    uint64_t memory_used;       /* 当前进程内存使用量（字节） */
    void *mr_list;              /* MR列表（预留） */
    
    /* MR操作速率限制状态 */
    pthread_mutex_t mr_rate_mutex; /* MR速率限制互斥锁 */
    uint64_t mr_op_count;       /* 当前时间窗口内的MR操作计数 */
    struct timespec mr_window_start; /* 速率限制窗口开始时间 */
} intercept_state_t;

/* 全局状态 */
extern intercept_state_t g_intercept_state;

/* 延迟初始化相关 */
extern pthread_once_t init_once;
extern void init_if_needed(void);

/* 初始化函数 */
int rdma_intercept_init(void);
void rdma_intercept_cleanup(void);

/* 配置函数 */
int rdma_intercept_load_config(const char *config_file);
int rdma_intercept_set_config(const intercept_config_t *config);
void rdma_intercept_get_config(intercept_config_t *config);

/* 日志函数 */
void rdma_intercept_log(log_level_t level, const char *format, ...);
void rdma_intercept_log_qp_creation(const qp_creation_info_t *info);

/* 工具函数 */
const char *rdma_intercept_version(void);
bool rdma_intercept_is_enabled(void);

/* 被拦截的RDMA函数声明 */
struct ibv_qp *ibv_create_qp_intercept(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr);
struct ibv_qp *ibv_create_qp_ex_intercept(struct ibv_context *context, struct ibv_qp_init_attr_ex *qp_init_attr_ex);
struct ibv_mr *ibv_reg_mr_intercept(struct ibv_pd *pd, void *addr, size_t length, int access);
int ibv_dereg_mr_intercept(struct ibv_mr *mr);

#ifdef __cplusplus
}
#endif

#endif /* RDMA_INTERCEPT_H */