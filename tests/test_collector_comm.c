#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <errno.h>

// 资源使用情况结构（与collector_server保持一致）
typedef struct {
    int qp_count;
    int mr_count;
    unsigned long long memory_used;
} resource_usage_t;

int main() {
    int sockfd;
    struct sockaddr_un addr;
    char buffer[1024];
    ssize_t n;
    
    printf("测试与collector_server的通信...\n");
    
    // 创建Unix域套接字
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return 1;
    }
    
    // 设置地址
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/rdma_collector.sock", sizeof(addr.sun_path) - 1);
    
    // 连接到collector_server
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("connect");
        close(sockfd);
        printf("无法连接到collector_server，请确保它正在运行\n");
        return 1;
    }
    
    printf("成功连接到collector_server\n");
    
    // 请求全局资源使用情况
    const char *request_global = "GET_STATS\n";
    if (write(sockfd, request_global, strlen(request_global)) == -1) {
        perror("write global request");
        close(sockfd);
        return 1;
    }
    
    // 读取响应
    n = read(sockfd, buffer, sizeof(buffer)-1);
    if (n == -1) {
        perror("read global response");
        close(sockfd);
        return 1;
    }
    buffer[n] = '\0';
    
    printf("全局资源使用情况: %s", buffer);
    
    // 请求当前进程资源使用情况
    char request_proc[256];
    snprintf(request_proc, sizeof(request_proc), "GET_PROC_STATS:%d\n", getpid());
    
    if (write(sockfd, request_proc, strlen(request_proc)) == -1) {
        perror("write process request");
        close(sockfd);
        return 1;
    }
    
    // 读取响应
    n = read(sockfd, buffer, sizeof(buffer)-1);
    if (n == -1) {
        perror("read process response");
        close(sockfd);
        return 1;
    }
    buffer[n] = '\0';
    
    printf("当前进程资源使用情况: %s", buffer);
    
    close(sockfd);
    printf("测试完成\n");
    return 0;
}