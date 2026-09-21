#include <stdio.h>
#include <stdlib.h>
#include <infiniband/verbs.h>
#include <assert.h>
#include <unistd.h>

int main() {
    struct ibv_device **device_list;
    struct ibv_device *device;
    struct ibv_context *context = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qp = NULL;
    struct ibv_mr *mr = NULL;
    void *buf;
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
    cq = ibv_create_cq(context, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "无法创建CQ\n");
        goto cleanup;
    }

    // 分配内存并注册MR
    buf = malloc(buf_size);
    if (!buf) {
        fprintf(stderr, "无法分配内存\n");
        goto cleanup;
    }

    mr = ibv_reg_mr(pd, buf, buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) {
        fprintf(stderr, "无法注册内存区域\n");
        free(buf);
        goto cleanup;
    }
    printf("MR已注册\n");

    // 创建QP - 测试全局限制
    struct ibv_qp_init_attr init_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 1,
            .max_recv_wr = 1,
            .max_send_sge = 1,
            .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,
    };

    printf("尝试创建第1个QP...\n");
    qp = ibv_create_qp(pd, &init_attr);
    if (!qp) {
        fprintf(stderr, "ERROR: 无法创建第1个QP\n");
        goto cleanup;
    }
    printf("第1个QP已创建\n");

    printf("尝试创建第2个QP...\n");
    struct ibv_qp *qp2 = ibv_create_qp(pd, &init_attr);
    if (!qp2) {
        fprintf(stderr, "第2个QP创建失败 - 这是正常的，因为我们设置了全局限制为2\n");
    } else {
        printf("第2个QP已创建\n");
    }

    printf("尝试创建第3个QP（应该被拦截）...\n");
    struct ibv_qp *qp3 = ibv_create_qp(pd, &init_attr);
    if (!qp3) {
        printf("第3个QP创建失败 - 这是正常的，因为我们设置了全局限制为2\n");
    } else {
        printf("第3个QP已创建 - 这表明拦截没有生效！\n");
        ibv_destroy_qp(qp3);
    }

    // 等待一段时间，让eBPF事件有机会被处理
    sleep(2);

    printf("测试完成\n");

cleanup:
    // 清理资源
    if (qp) ibv_destroy_qp(qp);
    if (mr) ibv_dereg_mr(mr);
    if (buf) free(buf);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    if (context) ibv_close_device(context);
    ibv_free_device_list(device_list);

    return 0;
}