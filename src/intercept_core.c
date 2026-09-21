#define _GNU_SOURCE
/* NO_DEBUG: Disable debug output for performance testing */
#ifdef NO_DEBUG
  #define DEBUG_FPRINTF(...) ((void)0)
#else
  #define DEBUG_FPRINTF(...) fprintf(__VA_ARGS__)
#endif

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include "rdma_intercept.h"

/* 全局状态 */
intercept_state_t g_intercept_state = {
    .initialized = false,
    .config = {
        .enable_intercept = true,
        .log_qp_creation = true,
        .log_all_operations = false,
        .log_level = LOG_LEVEL_INFO,
        .log_file_path = "/tmp/rdma_intercept.log",
        
        /* 资源管理和性能隔离默认配置 */
        .enable_qp_control = false,  /* 默认关闭QP控制 */
        .max_qp_per_process = 100,   /* 默认每个进程最多100个QP */
        .max_send_wr_limit = 1024,   /* 默认发送WR限制1024 */
        .max_recv_wr_limit = 1024,   /* 默认接收WR限制1024 */
        .allow_rc_qp = true,         /* 默认允许RC类型 */
        .allow_uc_qp = true,         /* 默认允许UC类型 */
        .allow_ud_qp = true,         /* 默认允许UD类型 */
        .allow_rq_qp = true,         /* 默认允许RQ类型 */
        
        /* 全局资源管理默认配置 */
        .max_global_qp = 1000,       /* 默认全局最多1000个QP */
        
        /* 内存资源管理默认配置 */
        .enable_mr_control = false,  /* 默认关闭内存控制 */
        .max_mr_per_process = 1000,  /* 默认每个进程最多1000个MR */
        .max_memory_per_process = 1024ULL * 1024ULL * 1024ULL * 10ULL  /* 默认每个进程最多10GB内存 */
    },
    .log_file = NULL,
    .log_mutex = PTHREAD_MUTEX_INITIALIZER,
    .resource_mutex = PTHREAD_MUTEX_INITIALIZER,
    .qp_count = 0,                  /* 当前进程QP计数 */
    .qp_list = NULL,                /* QP列表 */
    .mr_count = 0,                  /* 当前进程MR计数 */
    .memory_used = 0,               /* 当前进程内存使用量（字节） */
    .mr_list = NULL                 /* MR列表 */
};

/* 函数指针类型定义 */
typedef struct ibv_qp *(*ibv_create_qp_fn)(struct ibv_pd *, struct ibv_qp_init_attr *);
typedef struct ibv_qp *(*ibv_create_qp_ex_fn)(struct ibv_context *, struct ibv_qp_init_attr_ex *);
typedef struct ibv_mr *(*ibv_reg_mr_fn)(struct ibv_pd *, void *, size_t, int);
typedef int (*ibv_dereg_mr_fn)(struct ibv_mr *);

/* 原始函数指针存储 */
static ibv_create_qp_fn real_ibv_create_qp = NULL;
static ibv_create_qp_ex_fn real_ibv_create_qp_ex = NULL;
static ibv_reg_mr_fn real_ibv_reg_mr = NULL;
static ibv_dereg_mr_fn real_ibv_dereg_mr = NULL;

/* 延迟初始化相关变量 */
pthread_once_t init_once = PTHREAD_ONCE_INIT;

/* 按需初始化函数 */
void init_if_needed(void) {
    /* 检查是否启用拦截：RDMA_INTERCEPT_ENABLE=1 或 RDMA_INTERCEPT_ENABLE_QP_CONTROL=1 均可触发初始化 */
    const char *enable_var = getenv("RDMA_INTERCEPT_ENABLE");
    const char *qp_ctrl_var = getenv("RDMA_INTERCEPT_ENABLE_QP_CONTROL");
    int should_init = 0;
    if (enable_var && strcmp(enable_var, "1") == 0) {
        should_init = 1;
    }
    if (qp_ctrl_var && strcmp(qp_ctrl_var, "1") == 0) {
        should_init = 1;
    }
    if (!should_init) {
        return; /* 未启用拦截，直接返回 */
    }

    /* 初始化拦截功能 */
    if (rdma_intercept_init() != 0) {
        /* 初始化失败，记录错误但不中断进程 */
        fprintf(stderr, "[RDMA_INTERCEPT] 初始化失败，拦截功能将不可用\n");
    }
}

/* 初始化函数 */
int rdma_intercept_init(void) {
    if (g_intercept_state.initialized) {
        return 0;
    }

    errno = 0;

    /* 首先加载配置（包括环境变量），以确保日志文件路径等配置正确 */
    rdma_intercept_load_config(NULL);
    
    /* 打开日志文件 */
    g_intercept_state.log_file = fopen(g_intercept_state.config.log_file_path, "a");
    if (!g_intercept_state.log_file) {
fprintf(stderr, "[RDMA_INTERCEPT] Failed to open log file: %s\n", 
                g_intercept_state.config.log_file_path);
        errno = EIO;
        return -1;
    }

    /* 直接打开libibverbs.so获取原始函数地址 */
    void *libibverbs = dlopen("libibverbs.so", RTLD_LAZY);
    if (!libibverbs) {
fprintf(stderr, "[RDMA_INTERCEPT] Failed to open libibverbs.so: %s\n", dlerror());
        fclose(g_intercept_state.log_file);
        errno = ENOSYS;
        return -1;
    }
    
    /* 获取原始ibv_create_qp函数地址 */
    dlerror(); // 清除之前的错误
    real_ibv_create_qp = (ibv_create_qp_fn)dlsym(libibverbs, "ibv_create_qp");
    
    if (!real_ibv_create_qp) {
fprintf(stderr, "[RDMA_INTERCEPT] Failed to find ibv_create_qp in libibverbs: %s\n", dlerror());
        dlclose(libibverbs);
        fclose(g_intercept_state.log_file);
        errno = ENOSYS;
        return -1;
    }
    
fprintf(stderr, "[RDMA_INTERCEPT] Found ibv_create_qp in libibverbs at: %p\n", real_ibv_create_qp);
    
    // ibv_create_qp_ex是内联函数，可能不存在于库中，所以不检查错误
    dlerror(); // 清除之前的错误
    real_ibv_create_qp_ex = (ibv_create_qp_ex_fn)dlsym(libibverbs, "ibv_create_qp_ex");
    dlerror(); // 忽略错误信息
    
    // 获取原始ibv_reg_mr函数地址
    dlerror(); // 清除之前的错误
    real_ibv_reg_mr = (ibv_reg_mr_fn)dlsym(libibverbs, "ibv_reg_mr");
    
    if (!real_ibv_reg_mr) {
fprintf(stderr, "[RDMA_INTERCEPT] Failed to find ibv_reg_mr in libibverbs: %s\n", dlerror());
        dlclose(libibverbs);
        fclose(g_intercept_state.log_file);
        errno = ENOSYS;
        return -1;
    }
    
    // 获取原始ibv_dereg_mr函数地址
    dlerror(); // 清除之前的错误
    real_ibv_dereg_mr = (ibv_dereg_mr_fn)dlsym(libibverbs, "ibv_dereg_mr");
    
    if (!real_ibv_dereg_mr) {
fprintf(stderr, "[RDMA_INTERCEPT] Failed to find ibv_dereg_mr in libibverbs: %s\n", dlerror());
        dlclose(libibverbs);
        fclose(g_intercept_state.log_file);
        errno = ENOSYS;
        return -1;
    }
    
fprintf(stderr, "[RDMA_INTERCEPT] Found ibv_reg_mr in libibverbs at: %p\n", real_ibv_reg_mr);
fprintf(stderr, "[RDMA_INTERCEPT] Found ibv_dereg_mr in libibverbs at: %p\n", real_ibv_dereg_mr);
    
    // 关闭libibverbs句柄
    dlclose(libibverbs);

    fprintf(g_intercept_state.log_file, "[INFO] RDMA Intercept initialized successfully\n");
    fprintf(g_intercept_state.log_file, "[INFO] ibv_create_qp: %p\n", real_ibv_create_qp);
    fprintf(g_intercept_state.log_file, "[INFO] ibv_create_qp_ex: %p\n", real_ibv_create_qp_ex);
    fprintf(g_intercept_state.log_file, "[INFO] ibv_reg_mr: %p\n", real_ibv_reg_mr);
    fprintf(g_intercept_state.log_file, "[INFO] ibv_dereg_mr: %p\n", real_ibv_dereg_mr);

    g_intercept_state.initialized = true;
    return 0;
}

/* 清理函数 */
void rdma_intercept_cleanup(void) {
    if (!g_intercept_state.initialized) {
        return;
    }

    if (g_intercept_state.log_file) {
        fprintf(g_intercept_state.log_file, "[INFO] RDMA Intercept cleanup\n");
        fclose(g_intercept_state.log_file);
        g_intercept_state.log_file = NULL;
    }

    g_intercept_state.initialized = false;
}

/* 配置函数 - 这些函数现在只在intercept_core.c中定义 */

/* 版本信息 */
const char *rdma_intercept_version(void) {
    static const char *version = "1.0.0";
    return version;
}

/* 检查是否启用 */
bool rdma_intercept_is_enabled(void) {
    return g_intercept_state.initialized && g_intercept_state.config.enable_intercept;
}

/* 构造函数 - 自动初始化 */
__attribute__((constructor))
static void intercept_constructor(void) {
    /* 空函数，不再执行主动初始化 */
}