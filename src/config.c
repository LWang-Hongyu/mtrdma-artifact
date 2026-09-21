#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "rdma_intercept.h"

/* 默认配置文件路径 */
#define DEFAULT_CONFIG_FILE "/etc/rdma_intercept.conf"
#define ENV_CONFIG_FILE "RDMA_INTERCEPT_CONFIG"

/* 配置解析状态 */
typedef struct {
    char *key;
    char *value;
    int (*parser)(const char *, intercept_config_t *);
} config_entry_t;

/* 前向声明 */
static int parse_bool(const char *value, bool *result);
static int parse_log_level_value(const char *value, log_level_t *level);
static int parse_string(const char *value, char *dest, size_t max_len);

/* 配置项解析函数 */
static int parse_enable_intercept(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->enable_intercept);
}

static int parse_log_qp_creation(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->log_qp_creation);
}

static int parse_log_all_operations(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->log_all_operations);
}

static int parse_log_level_config(const char *value, intercept_config_t *config) {
    log_level_t level;
    if (parse_log_level_value(value, &level) != 0) {
        return -1;
    }
    config->log_level = level;
    return 0;
}

static int parse_log_file_path(const char *value, intercept_config_t *config) {
    return parse_string(value, config->log_file_path, sizeof(config->log_file_path));
}

/* 资源管理和性能隔离配置解析 */
static int parse_enable_qp_control(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->enable_qp_control);
}

static int parse_max_qp_per_process(const char *value, intercept_config_t *config) {
    long val = strtol(value, NULL, 10);
    if (val <= 0 || val > UINT32_MAX) {
        return -1;
    }
    config->max_qp_per_process = (uint32_t)val;
    return 0;
}

static int parse_max_send_wr_limit(const char *value, intercept_config_t *config) {
    long val = strtol(value, NULL, 10);
    if (val <= 0 || val > UINT32_MAX) {
        return -1;
    }
    config->max_send_wr_limit = (uint32_t)val;
    return 0;
}

static int parse_max_recv_wr_limit(const char *value, intercept_config_t *config) {
    long val = strtol(value, NULL, 10);
    if (val <= 0 || val > UINT32_MAX) {
        return -1;
    }
    config->max_recv_wr_limit = (uint32_t)val;
    return 0;
}

static int parse_allow_rc_qp(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->allow_rc_qp);
}

static int parse_allow_uc_qp(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->allow_uc_qp);
}

static int parse_allow_ud_qp(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->allow_ud_qp);
}

static int parse_max_global_qp(const char *value, intercept_config_t *config) {
    long val = strtol(value, NULL, 10);
    if (val <= 0 || val > UINT32_MAX) {
        return -1;
    }
    config->max_global_qp = (uint32_t)val;
    return 0;
}

/* MR操作速率限制配置解析 */
static int parse_enable_mr_rate_limit(const char *value, intercept_config_t *config) {
    return parse_bool(value, &config->enable_mr_rate_limit);
}

static int parse_max_mr_ops_per_sec(const char *value, intercept_config_t *config) {
    long val = strtol(value, NULL, 10);
    if (val < 0 || val > UINT32_MAX) {
        return -1;
    }
    config->max_mr_ops_per_sec = (uint32_t)val;
    return 0;
}

static int parse_mr_rate_limit_window_ms(const char *value, intercept_config_t *config) {
    long val = strtol(value, NULL, 10);
    if (val < 0 || val > UINT32_MAX) {
        return -1;
    }
    config->mr_rate_limit_window_ms = (uint32_t)val;
    return 0;
}

/* 配置表 */
static config_entry_t config_table[] = {
    {"enable_intercept", NULL, (int (*)(const char *, intercept_config_t *))parse_enable_intercept},
    {"log_qp_creation", NULL, (int (*)(const char *, intercept_config_t *))parse_log_qp_creation},
    {"log_all_operations", NULL, (int (*)(const char *, intercept_config_t *))parse_log_all_operations},
    {"log_level", NULL, parse_log_level_config},
    {"log_file_path", NULL, (int (*)(const char *, intercept_config_t *))parse_log_file_path},
    
    /* 资源管理和性能隔离配置项 */
    {"enable_qp_control", NULL, (int (*)(const char *, intercept_config_t *))parse_enable_qp_control},
    {"max_qp_per_process", NULL, (int (*)(const char *, intercept_config_t *))parse_max_qp_per_process},
    {"max_send_wr_limit", NULL, (int (*)(const char *, intercept_config_t *))parse_max_send_wr_limit},
    {"max_recv_wr_limit", NULL, (int (*)(const char *, intercept_config_t *))parse_max_recv_wr_limit},
    {"allow_rc_qp", NULL, (int (*)(const char *, intercept_config_t *))parse_allow_rc_qp},
    {"allow_uc_qp", NULL, (int (*)(const char *, intercept_config_t *))parse_allow_uc_qp},
    {"allow_ud_qp", NULL, (int (*)(const char *, intercept_config_t *))parse_allow_ud_qp},
    {"max_global_qp", NULL, (int (*)(const char *, intercept_config_t *))parse_max_global_qp},
    
    /* MR操作速率限制配置项 */
    {"enable_mr_rate_limit", NULL, (int (*)(const char *, intercept_config_t *))parse_enable_mr_rate_limit},
    {"max_mr_ops_per_sec", NULL, (int (*)(const char *, intercept_config_t *))parse_max_mr_ops_per_sec},
    {"mr_rate_limit_window_ms", NULL, (int (*)(const char *, intercept_config_t *))parse_mr_rate_limit_window_ms},
    
    {NULL, NULL, NULL}
};

/* 解析布尔值 */
static int parse_bool(const char *value, bool *result) {
    if (!value || !result) {
        return -1;
    }
    
    /* 去除前后空格 */
    while (isspace(*value)) value++;
    
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || 
        strcasecmp(value, "on") == 0 || strcasecmp(value, "1") == 0) {
        *result = true;
        return 0;
    } else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || 
               strcasecmp(value, "off") == 0 || strcasecmp(value, "0") == 0) {
        *result = false;
        return 0;
    }
    
    return -1;
}

/* 解析日志级别 */
static int parse_log_level_value(const char *value, log_level_t *level) {
    if (!value || !level) {
        return -1;
    }
    
    /* 去除前后空格 */
    while (isspace(*value)) value++;
    
    if (strcasecmp(value, "debug") == 0) {
        *level = LOG_LEVEL_DEBUG;
    } else if (strcasecmp(value, "info") == 0) {
        *level = LOG_LEVEL_INFO;
    } else if (strcasecmp(value, "warn") == 0 || strcasecmp(value, "warning") == 0) {
        *level = LOG_LEVEL_WARN;
    } else if (strcasecmp(value, "error") == 0) {
        *level = LOG_LEVEL_ERROR;
    } else if (strcasecmp(value, "fatal") == 0) {
        *level = LOG_LEVEL_FATAL;
    } else {
        return -1;
    }
    
    return 0;
}

/* 解析字符串 */
static int parse_string(const char *value, char *dest, size_t max_len) {
    if (!value || !dest || max_len == 0) {
        return -1;
    }
    
    /* 去除前后空格和引号 */
    while (isspace(*value) || *value == '"' || *value == '\'') value++;
    
    size_t len = strlen(value);
    while (len > 0 && (isspace(value[len-1]) || value[len-1] == '"' || value[len-1] == '\'')) {
        len--;
    }
    
    if (len >= max_len) {
        return -1;
    }
    
    strncpy(dest, value, len);
    dest[len] = '\0';
    
    return 0;
}

/* 去除字符串前后空格 */
static char *trim_whitespace(char *str) {
    char *end;
    
    /* 去除前导空格 */
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) {
        return str;
    }
    
    /* 去除尾随空格 */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    /* 写入新的null字符 */
    end[1] = '\0';
    
    return str;
}

/* 解析配置文件行 */
static int parse_config_line(char *line, intercept_config_t *config) {
    char *key, *value, *equals;
    int i;
    
    /* 去除前后空格 */
    line = trim_whitespace(line);
    
    /* 跳过空行和注释 */
    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
        return 0;
    }
    
    /* 查找等号 */
    equals = strchr(line, '=');
    if (!equals) {
        return -1;
    }
    
    /* 分割键和值 */
    *equals = '\0';
    key = trim_whitespace(line);
    value = trim_whitespace(equals + 1);
    
    /* 查找并调用对应的解析函数 */
    for (i = 0; config_table[i].key != NULL; i++) {
        if (strcasecmp(key, config_table[i].key) == 0) {
            if (config_table[i].parser) {
                return config_table[i].parser(value, config);
            }
            break;
        }
    }
    
    return -1;  /* 未找到对应的配置项 */
}

/* 从环境变量加载配置 */
static void load_config_from_env(intercept_config_t *config) {
    const char *env_val;
    
    /* 启用QP控制 */
    env_val = getenv("RDMA_INTERCEPT_ENABLE_QP_CONTROL");
    if (env_val) {
        parse_bool(env_val, &config->enable_qp_control);
    }
    
    /* 最大QP数量 */
    env_val = getenv("RDMA_INTERCEPT_MAX_QP_PER_PROCESS");
    if (env_val) {
        long val = strtol(env_val, NULL, 10);
        if (val > 0 && val <= UINT32_MAX) {
            config->max_qp_per_process = (uint32_t)val;
        }
    }
    
    /* 全局最大QP数量 */
    env_val = getenv("RDMA_INTERCEPT_MAX_GLOBAL_QP");
    if (env_val) {
        long val = strtol(env_val, NULL, 10);
        if (val > 0 && val <= UINT32_MAX) {
            config->max_global_qp = (uint32_t)val;
        }
    }
    
    /* 发送WR限制 */
    env_val = getenv("RDMA_INTERCEPT_MAX_SEND_WR_LIMIT");
    if (env_val) {
        long val = strtol(env_val, NULL, 10);
        if (val > 0 && val <= UINT32_MAX) {
            config->max_send_wr_limit = (uint32_t)val;
        }
    }
    
    /* 接收WR限制 */
    env_val = getenv("RDMA_INTERCEPT_MAX_RECV_WR_LIMIT");
    if (env_val) {
        long val = strtol(env_val, NULL, 10);
        if (val > 0 && val <= UINT32_MAX) {
            config->max_recv_wr_limit = (uint32_t)val;
        }
    }
    
    /* QP类型限制 */
    env_val = getenv("RDMA_INTERCEPT_ALLOW_RC_QP");
    if (env_val) {
        parse_bool(env_val, &config->allow_rc_qp);
    }
    
    env_val = getenv("RDMA_INTERCEPT_ALLOW_UC_QP");
    if (env_val) {
        parse_bool(env_val, &config->allow_uc_qp);
    }
    
    env_val = getenv("RDMA_INTERCEPT_ALLOW_UD_QP");
    if (env_val) {
        parse_bool(env_val, &config->allow_ud_qp);
    }
    
    /* 内存控制相关配置 */
    env_val = getenv("RDMA_INTERCEPT_ENABLE_MR_CONTROL");
    if (env_val) {
        parse_bool(env_val, &config->enable_mr_control);
    }
    
    /* 最大MR数量 */
    env_val = getenv("RDMA_INTERCEPT_MAX_MR_PER_PROCESS");
    if (env_val) {
        long val = strtol(env_val, NULL, 10);
        if (val > 0 && val <= UINT32_MAX) {
            config->max_mr_per_process = (uint32_t)val;
        }
    }
    
    /* 最大内存使用量 */
    env_val = getenv("RDMA_INTERCEPT_MAX_MEMORY_PER_PROCESS");
    if (env_val) {
        unsigned long long val = strtoull(env_val, NULL, 10);
        if (val > 0) {
            config->max_memory_per_process = val;
        }
    }
    
    /* MR操作速率限制配置 */
    env_val = getenv("RDMA_INTERCEPT_ENABLE_MR_RATE_LIMIT");
    if (env_val) {
        parse_bool(env_val, &config->enable_mr_rate_limit);
    }
    
    /* 最大MR操作速率（每秒） */
    env_val = getenv("RDMA_INTERCEPT_MAX_MR_OPS_PER_SEC");
    if (env_val) {
        long val = strtol(env_val, NULL, 10);
        if (val >= 0 && val <= UINT32_MAX) {
            config->max_mr_ops_per_sec = (uint32_t)val;
        }
    }
    
    /* MR速率限制窗口（毫秒） */
    env_val = getenv("RDMA_INTERCEPT_MR_RATE_LIMIT_WINDOW_MS");
    if (env_val) {
        long val = strtol(env_val, NULL, 10);
        if (val >= 0 && val <= UINT32_MAX) {
            config->mr_rate_limit_window_ms = (uint32_t)val;
        }
    }
    
    /* 日志文件路径 */
    env_val = getenv("RDMA_INTERCEPT_LOG_FILE_PATH");
    if (env_val) {
        parse_string(env_val, config->log_file_path, sizeof(config->log_file_path));
    }
}

/* 从文件加载配置 */
int rdma_intercept_load_config(const char *config_file) {
    FILE *fp;
    char line[1024];
    intercept_config_t temp_config;
    int line_num = 0;
    int errors = 0;
    
    if (!config_file) {
        config_file = DEFAULT_CONFIG_FILE;
    }
    
    /* 复制当前配置作为基础 */
    memcpy(&temp_config, &g_intercept_state.config, sizeof(temp_config));
    
    /* 加载环境变量配置（优先） */
    load_config_from_env(&temp_config);
    
    /* 尝试打开配置文件 */
    fp = fopen(config_file, "r");
    if (!fp) {
        /* 如果文件不存在，尝试环境变量指定的文件 */
        const char *env_config = getenv(ENV_CONFIG_FILE);
        if (env_config) {
            fp = fopen(env_config, "r");
            if (fp) {
                config_file = env_config;
            }
        }
    }
    
    if (!fp) {
        /* 使用默认配置和环境变量配置 */
        rdma_intercept_log(LOG_LEVEL_INFO, "Config file not found, using defaults + env");
        
        /* 应用新配置 */
        memcpy(&g_intercept_state.config, &temp_config, sizeof(g_intercept_state.config));
        return 0;
    }
    
    rdma_intercept_log(LOG_LEVEL_INFO, "Loading config from: %s", config_file);
    
    /* 解析配置文件 */
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        
        /* 去除行尾换行符 */
        line[strcspn(line, "\r\n")] = '\0';
        
        /* 解析配置行 */
        if (parse_config_line(line, &temp_config) != 0) {
            rdma_intercept_log(LOG_LEVEL_WARN, "Invalid config line %d: %s", line_num, line);
            errors++;
        }
    }
    
    fclose(fp);
    
    if (errors > 0) {
        rdma_intercept_log(LOG_LEVEL_WARN, "Config parsing completed with %d errors", errors);
    }
    
    /* 应用新配置 */
    memcpy(&g_intercept_state.config, &temp_config, sizeof(g_intercept_state.config));
    
    rdma_intercept_log(LOG_LEVEL_INFO, "Config loaded successfully");
    return 0;
}

/* 设置配置 */
int rdma_intercept_set_config(const intercept_config_t *config) {
    if (!config) {
        return -1;
    }
    
    /* 验证配置 */
    if (config->log_level < LOG_LEVEL_DEBUG || config->log_level > LOG_LEVEL_FATAL) {
        return -1;
    }
    
    if (strlen(config->log_file_path) >= MAX_CONFIG_FILE_PATH) {
        return -1;
    }
    
    /* 应用配置 */
    pthread_mutex_lock(&g_intercept_state.log_mutex);
    memcpy(&g_intercept_state.config, config, sizeof(g_intercept_state.config));
    pthread_mutex_unlock(&g_intercept_state.log_mutex);
    
    /* 更新日志文件 - 使用外部声明的函数 */
    extern int rdma_intercept_set_log_file(const char *log_file_path);
    if (strlen(config->log_file_path) > 0) {
        rdma_intercept_set_log_file(config->log_file_path);
    }
    
    rdma_intercept_log(LOG_LEVEL_INFO, "Configuration updated");
    return 0;
}

/* 获取配置 */
void rdma_intercept_get_config(intercept_config_t *config) {
    if (config) {
        pthread_mutex_lock(&g_intercept_state.log_mutex);
        memcpy(config, &g_intercept_state.config, sizeof(*config));
        pthread_mutex_unlock(&g_intercept_state.log_mutex);
    }
}

/* 打印当前配置 */
void rdma_intercept_print_config(void) {
    intercept_config_t config;
    
    rdma_intercept_get_config(&config);
    
    rdma_intercept_log(LOG_LEVEL_INFO, "Current Configuration:");
    rdma_intercept_log(LOG_LEVEL_INFO, "  enable_intercept: %s", config.enable_intercept ? "true" : "false");
    rdma_intercept_log(LOG_LEVEL_INFO, "  log_qp_creation: %s", config.log_qp_creation ? "true" : "false");
    rdma_intercept_log(LOG_LEVEL_INFO, "  log_all_operations: %s", config.log_all_operations ? "true" : "false");
    rdma_intercept_log(LOG_LEVEL_INFO, "  log_level: %d", config.log_level);
    rdma_intercept_log(LOG_LEVEL_INFO, "  log_file_path: %s", config.log_file_path);
}