cmake_minimum_required(VERSION 3.10)
project(rdma_intercept_ldpreload VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -fPIC -DNO_DEBUG")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O2 -DNDEBUG")

# 链接选项 - 防止未定义符号导致的问题
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")

# 版本脚本路径
set(RDMA_INTERCEPT_VERSION_SCRIPT ${CMAKE_SOURCE_DIR}/src/rdma_intercept.map)

# 查找依赖
find_package(Threads REQUIRED)

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    /usr/include/infiniband
    /usr/include/bpf
)

# 链接目录
link_directories(/usr/lib/x86_64-linux-gnu)

# 源文件
set(INTERCEPT_SOURCES
    src/intercept_core.c
    src/rdma_hooks_tenant.c
    src/logger.c
    src/config.c
    src/performance_optimizer.c
    src/shm/shared_memory.c
    src/shm/shared_memory_tenant.c
    src/ebpf/ebpf_monitor_shm.c
    src/ebpf/ebpf_enhanced_monitor.c
    src/dynamic_policy_manager.c
)





# collector_server通信测试程序
add_executable(test_collector_comm tests/test_collector_comm.c)
target_link_libraries(test_collector_comm
    ${CMAKE_THREAD_LIBS_INIT}
)

# eBPF拦截功能测试程序
add_executable(test_intercept_ebpf tests/test_intercept_ebpf.c)
target_link_libraries(test_intercept_ebpf
    ibverbs
    rdmacm
    ${CMAKE_THREAD_LIBS_INIT}
    dl
)

# 全局限制测试程序
add_executable(test_global_limit tests/test_global_limit.c)
target_link_libraries(test_global_limit
    ibverbs
    rdmacm
    ${CMAKE_THREAD_LIBS_INIT}
    dl
)

# 共享内存版本测试程序
add_executable(test_shm_version tests/test_shm_version.c)
target_link_libraries(test_shm_version
    ibverbs
    ${CMAKE_THREAD_LIBS_INIT}
    rt
)

# 基于共享内存的数据收集服务
add_executable(collector_server_shm src/collector_server_shm.c)
target_link_libraries(collector_server_shm 
    shared_memory
    tenant_shared_memory
    ebpf_monitor_shm
    ${CMAKE_THREAD_LIBS_INIT}
    bpf
    elf
    z
    rt
)

# 主拦截库
add_library(rdma_intercept SHARED ${INTERCEPT_SOURCES})
target_link_libraries(rdma_intercept 
    ibverbs
    rdmacm
    ${CMAKE_THREAD_LIBS_INIT}
    dl
    ebpf_monitor_shm
    ebpf_enhanced_monitor
    shared_memory
    tenant_shared_memory
    dynamic_policy_manager
    performance_optimizer
    bpf
    elf
    z
)
set_target_properties(rdma_intercept PROPERTIES LINK_FLAGS "-Wl,--version-script=${RDMA_INTERCEPT_VERSION_SCRIPT}")

# 动态策略管理库
add_library(dynamic_policy_manager STATIC src/dynamic_policy_manager.c)
target_link_libraries(dynamic_policy_manager 
    shared_memory
    Threads::Threads
    rt
)

# 性能优化器库
add_library(performance_optimizer STATIC src/performance_optimizer.c)
target_link_libraries(performance_optimizer 
    shared_memory
    Threads::Threads
    rt
)

# ★ 动态策略管理守护进程（方案A）
add_executable(tenant_manager_daemon src/tenant_manager_daemon.c)
target_link_libraries(tenant_manager_daemon 
    tenant_shared_memory
    json-c
    Threads::Threads
    rt
)

# ★ 动态策略管理客户端（方案A）
add_executable(tenant_manager_client src/tenant_manager_client.c)
target_link_libraries(tenant_manager_client 
    json-c
    Threads::Threads
    rt
)

# 共享内存库
add_library(shared_memory STATIC src/shm/shared_memory.c)
target_link_libraries(shared_memory 
    Threads::Threads
    rt
)

# 租户共享内存库
add_library(tenant_shared_memory STATIC src/shm/shared_memory_tenant.c)
target_link_libraries(tenant_shared_memory 
    shared_memory
    Threads::Threads
    rt
)

# eBPF监控库（共享内存版本）
add_library(ebpf_monitor_shm STATIC src/ebpf/ebpf_monitor_shm.c)
target_link_libraries(ebpf_monitor_shm 
    shared_memory
    bpf
    elf
    z
)

# 增强型eBPF监控库
add_library(ebpf_enhanced_monitor STATIC src/ebpf/ebpf_enhanced_monitor.c)
target_link_libraries(ebpf_enhanced_monitor 
    shared_memory
    bpf
    elf
    z
    rt
)

# eBPF监控程序（共享内存版本）- 使用独立的main文件
# add_executable(ebpf_monitor_shm_exe src/ebpf/ebpf_monitor_shm_main.c)
# target_link_libraries(ebpf_monitor_shm_exe 
#     ebpf_monitor_shm
#     shared_memory
#     bpf
#     elf
#     z
#     rt
# )

# 测试程序（已移除，使用tests目录下的脚本）

# 单元测试（已移除，使用tests目录下的脚本）

# 安装规则
install(TARGETS rdma_intercept
    LIBRARY DESTINATION lib
)

install(FILES 
    include/rdma_intercept.h
    DESTINATION include
)

install(PROGRAMS
    scripts/run_with_intercept.sh
    DESTINATION bin
)