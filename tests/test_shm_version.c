/*
 * test_shm_version.c
 * 测试共享内存版本的RDMA拦截库功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

// 简单的RDMA资源创建测试函数
int test_rdma_resources() {
    // 获取可用的RDMA设备
    struct ibv_device **device_list;
    struct ibv_context *context = NULL;
    
    device_list = ibv_get_device_list(NULL);
    if (!device_list) {
        printf("No RDMA devices found\n");
        return -1;
    }
    
    if (!device_list[0]) {
        printf("No RDMA devices available\n");
        ibv_free_device_list(device_list);
        return -1;
    }
    
    // 打开第一个设备
    context = ibv_open_device(device_list[0]);
    if (!context) {
        printf("Cannot open RDMA device\n");
        ibv_free_device_list(device_list);
        return -1;
    }
    
    printf("Successfully opened RDMA device: %s\n", ibv_get_device_name(device_list[0]));
    
    // 获取设备属性
    struct ibv_device_attr device_attr;
    if (ibv_query_device(context, &device_attr)) {
        printf("Cannot query device attributes\n");
        ibv_close_device(context);
        ibv_free_device_list(device_list);
        return -1;
    }
    
    printf("Device max QPs: %d\n", device_attr.max_qp);
    
    // 创建保护域
    struct ibv_pd *pd = ibv_alloc_pd(context);
    if (!pd) {
        printf("Cannot allocate protection domain\n");
        ibv_close_device(context);
        ibv_free_device_list(device_list);
        return -1;
    }
    
    printf("Successfully allocated protection domain\n");
    
    // 尝试创建CQ
    struct ibv_cq *cq = ibv_create_cq(context, 10, NULL, NULL, 0);
    if (!cq) {
        printf("Cannot create completion queue\n");
    } else {
        printf("Successfully created completion queue\n");
        
        // 尝试创建QP
        struct ibv_qp_init_attr qp_init_attr;
        memset(&qp_init_attr, 0, sizeof(qp_init_attr));
        
        qp_init_attr.qp_type = IBV_QPT_RC;
        qp_init_attr.sq_sig_all = 1;
        qp_init_attr.send_cq = cq;
        qp_init_attr.recv_cq = cq;
        qp_init_attr.cap.max_send_wr = 10;
        qp_init_attr.cap.max_recv_wr = 10;
        qp_init_attr.cap.max_send_sge = 1;
        qp_init_attr.cap.max_recv_sge = 1;
        
        struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
        if (qp) {
            printf("Successfully created QP\n");
            
            // 销毁QP
            if (ibv_destroy_qp(qp) == 0) {
                printf("Successfully destroyed QP\n");
            }
        } else {
            printf("Failed to create QP (possibly intercepted)\n");
        }
        
        // 销毁CQ
        if (ibv_destroy_cq(cq) == 0) {
            printf("Successfully destroyed CQ\n");
        }
    }
    
    // 注册内存区域
    const size_t mr_size = 4096;
    void *buf = malloc(mr_size);
    if (buf) {
        struct ibv_mr *mr = ibv_reg_mr(pd, buf, mr_size, IBV_ACCESS_LOCAL_WRITE);
        if (mr) {
            printf("Successfully registered memory region\n");
            
            // 注销内存区域
            if (ibv_dereg_mr(mr) == 0) {
                printf("Successfully deregistered memory region\n");
            }
        } else {
            printf("Failed to register memory region (possibly intercepted)\n");
        }
        
        free(buf);
    }
    
    // 释放保护域
    if (ibv_dealloc_pd(pd) == 0) {
        printf("Successfully deallocated protection domain\n");
    }
    
    // 关闭设备
    if (ibv_close_device(context) == 0) {
        printf("Successfully closed RDMA device\n");
    }
    
    ibv_free_device_list(device_list);
    
    return 0;
}

// 测试共享内存是否存在
int test_shared_memory() {
    // 尝试打开共享内存对象
    int shm_fd = shm_open("/rdma_intercept_shm", O_RDONLY, 0666);
    if (shm_fd == -1) {
        printf("Shared memory object does not exist or is not accessible: %s\n", strerror(errno));
        return -1;
    }
    
    printf("Successfully accessed shared memory object\n");
    
    // 映射共享内存
    struct stat shm_stat;
    if (fstat(shm_fd, &shm_stat) == -1) {
        printf("Cannot get shared memory stats\n");
        close(shm_fd);
        return -1;
    }
    
    printf("Shared memory size: %ld bytes\n", shm_stat.st_size);
    
    void *shm_ptr = mmap(NULL, shm_stat.st_size, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        printf("Cannot map shared memory\n");
        close(shm_fd);
        return -1;
    }
    
    printf("Successfully mapped shared memory\n");
    
    // 简单检查结构
    printf("First 8 bytes of shared memory: ");
    unsigned char *bytes = (unsigned char *)shm_ptr;
    for (int i = 0; i < 8; i++) {
        printf("%02x ", bytes[i]);
    }
    printf("\n");
    
    // 解除映射
    munmap(shm_ptr, shm_stat.st_size);
    close(shm_fd);
    
    return 0;
}

int main() {
    printf("Testing Shared Memory Version of RDMA Intercept Library\n");
    printf("=====================================================\n\n");
    
    // 测试共享内存
    printf("1. Testing shared memory accessibility:\n");
    if (test_shared_memory() == 0) {
        printf("   Shared memory test PASSED\n\n");
    } else {
        printf("   Shared memory test FAILED\n\n");
    }
    
    // 测试RDMA资源
    printf("2. Testing RDMA resource creation:\n");
    if (test_rdma_resources() == 0) {
        printf("   RDMA resource test COMPLETED\n\n");
    } else {
        printf("   RDMA resource test FAILED\n\n");
    }
    
    printf("Test completed.\n");
    
    return 0;
}