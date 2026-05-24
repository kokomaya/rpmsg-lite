cmake_minimum_required(VERSION 3.14)
project(rpmsg_sim C)

set(CMAKE_C_STANDARD 11)

# RPMsg-Lite core sources (unmodified)
set(RPMSG_LIB_DIR ${CMAKE_SOURCE_DIR}/../lib)

set(RPMSG_SOURCES
    ${RPMSG_LIB_DIR}/rpmsg_lite/rpmsg_lite.c
    ${RPMSG_LIB_DIR}/rpmsg_lite/rpmsg_ns.c
    ${RPMSG_LIB_DIR}/rpmsg_lite/rpmsg_queue.c
    ${RPMSG_LIB_DIR}/virtio/virtqueue.c
    ${RPMSG_LIB_DIR}/common/llist.c
)

# Simulation backend sources
set(SIM_SOURCES
    backend/main.c
    backend/sim_env.c
    backend/sim_platform.c
    backend/sim_core.c
    backend/sim_events.c
    third_party/mongoose/mongoose.c
)

add_executable(rpmsg_sim ${RPMSG_SOURCES} ${SIM_SOURCES})

target_include_directories(rpmsg_sim PRIVATE
    ${RPMSG_LIB_DIR}/include
    ${CMAKE_SOURCE_DIR}/backend        # rpmsg_config.h, rpmsg_platform.h, rpmsg_env_specific.h
    ${CMAKE_SOURCE_DIR}/third_party/mongoose
)

# Platform-specific settings
if(WIN32)
    target_link_libraries(rpmsg_sim ws2_32)
    target_compile_definitions(rpmsg_sim PRIVATE _CRT_SECURE_NO_WARNINGS MG_ENABLE_WINSOCK=1)
else()
    target_link_libraries(rpmsg_sim pthread m)
endif()

target_compile_definitions(rpmsg_sim PRIVATE
    RL_USE_STATIC_API=1
    SIM_BUILD=1
)

# Copy web files to build output
file(COPY ${CMAKE_SOURCE_DIR}/web DESTINATION ${CMAKE_BINARY_DIR})
