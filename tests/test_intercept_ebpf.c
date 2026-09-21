#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <assert.h>
#include <unistd.h>
#include <dlfcn.h>

// 定义函数指针类型
typedef struct ibv_qp* (*ibv_create_qp_func)(struct ibv_pd *pd, struct ibv_qp_init_attr *qp_init_attr);
typedef int (*ibv_destroy_qp_func)(struct ibv_qp *qp);

int main() {
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_context *context = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qp1 = NULL, *qp2 = NULL, *qp3 = NULL;
    struct ibv_mr *mr1 = NULL, *mr2 = NULL, *mr3 = NULL;
    void *buf1, *buf2, *buf3;
    int buf_size = 4096;

    // 获取设备列表
    device_list = ibv_get_device_list(NULL);
    if (!device_list) {
        fprintf(stderr, "无法获取IB设备列表\n");
        return 1;
    }

    // 选择第一个设备
    device = device_list[0];
    if (!device) {
        fprintf(stderr, "没有找到IB设备\n");
        ibv_free_device_list(device_list);
        return 1;
    }

    printf("使用设备: %s\n", device->name);

    // 打开设备上下文
    context = ibv_open_device(device);
    if (!context) {
        fprintf(stderr, "无法打开设备上下文\n");
        ibv_free_device_list(device_list);
        return 1;
    }

    // 分配保护域
    pd = ibv_alloc_pd(context);
    if (!pd) {
        fprintf(stderr, "无法分配保护域\n");
        ibv_close_device(context);
        ibv_free_device_list(device_list);
        return 1;
    }

    // 创建CQ
    cq = ibv_create_cq(context, 30, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "无法创建CQ\n");
        goto cleanup;
    }

    // 分配内存并注册MR
    buf1 = malloc(buf_size);
    buf2 = malloc(buf_size);
    buf3 = malloc(buf_size);
    if (!buf1 || !buf2 || !buf3) {
        fprintf(stderr, "无法分配内存\n");
        goto cleanup;
    }

    mr1 = ibv_reg_mr(pd, buf1, buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr1) {
        fprintf(stderr, "无法注册内存区域1\n");
        goto cleanup;
    }
    printf("MR1已注册\n");

    mr2 = ibv_reg_mr(pd, buf2, buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr2) {
        fprintf(stderr, "无法注册内存区域2\n");
        goto cleanup;
    }
    printf("MR2已注册\n");

    mr3 = ibv_reg_mr(pd, buf3, buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr3) {
        fprintf(stderr, "无法注册内存区域3\n");
        goto cleanup;
    }
    printf("MR3已注册\n");

    // 创建QP - 尝试创建3个QP，但全局限制为2个
    struct ibv_qp_init_attr init_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };

    qp1 = ibv_create_qp(pd, &init_attr);
    if (!qp1) {
        fprintf(stderr, "ERROR: 无法创建QP1 (这不应该发生)\n");
        goto cleanup;
    }
    printf("QP1已创建\n");

    qp2 = ibv_create_qp(pd, &init_attr);
    if (!qp2) {
        fprintf(stderr, "ERROR: 无法创建QP2 (这不应该发生)\n");
        goto cleanup;
    }
    printf("QP2已创建\n");

    qp3 = ibv_create_qp(pd, &init_attr);
    if (!qp3) {
        printf("QP3创建失败 - 这是预期的，因为全局QP限制为2\n");
    } else {
        printf("QP3已创建 - 这表明拦截没有生效\n");
        ibv_destroy_qp(qp3);
    }

    // 等待一段时间，让eBPF事件有机会被处理
    sleep(2);

    printf("测试完成\n");

cleanup:
    // 清理资源
    if (qp1) ibv_destroy_qp(qp1);
    if (qp2) ibv_destroy_qp(qp2);
    if (qp3) ibv_destroy_qp(qp3);
    if (mr1) ibv_dereg_mr(mr1);
    if (mr2) ibv_dereg_mr(mr2);
    if (mr3) ibv_dereg_mr(mr3);
    if (buf1) free(buf1);
    if (buf2) free(buf2);
    if (buf3) free(buf3);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (context) ibv_close_device(context);
    ibv_free_device_list(device_list);

    return 0;
}