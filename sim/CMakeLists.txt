cmake_minimum_required(VERSION 3.13)
project(rpmsg_lite_sim C)
set(CMAKE_C_STANDARD 11)

# ---- Source files --------------------------------------------------------

set(RPMSG_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/../lib/rpmsg_lite/rpmsg_lite.c
    ${CMAKE_SOURCE_DIR}/../lib/rpmsg_lite/rpmsg_ns.c
    ${CMAKE_SOURCE_DIR}/../lib/rpmsg_lite/rpmsg_queue.c
    ${CMAKE_SOURCE_DIR}/../lib/virtio/virtqueue.c
    ${CMAKE_SOURCE_DIR}/../lib/common/llist.c
)

set(SIM_PLATFORM_SOURCES
    platform/rpmsg_env_sim.c
    platform/rpmsg_platform_sim.c
    platform/ipc_sim.c
)

set(TRACE_SOURCES
    instrumentation/rpmsg_trace.c
)

set(APP_SOURCES
    scenarios/sim_main.c
)

# ---- Include paths -------------------------------------------------------
# sim/core/include MUST come first to override rpmsg_platform.h,
# rpmsg_config.h, rpmsg_env_specific.h, and rpmsg_compiler.h

include_directories(
    core/include               # overrides: rpmsg_platform.h, rpmsg_config.h, rpmsg_compiler.h, rpmsg_env_specific.h
    ../lib/include             # canonical lib headers
    ../lib/include/environment/bm  # fallback for rpmsg_env_specific.h (overridden above)
    platform                   # ipc_sim.h, rpmsg_platform_sim.h
    instrumentation            # rpmsg_trace.h
)

# ---- Executable ----------------------------------------------------------

add_executable(rpmsg_sim
    ${RPMSG_CORE_SOURCES}
    ${SIM_PLATFORM_SOURCES}
    ${TRACE_SOURCES}
    ${APP_SOURCES}
)

# ---- Compile definitions -------------------------------------------------

target_compile_definitions(rpmsg_sim PRIVATE
    RPMSG_INSTRUMENTATION_ENABLED=1
    RL_USE_CUSTOM_CONFIG=1
)

# Windows: link no extra libs beyond what's implicit (kernel32 for Win32 API)
if(MSVC)
    target_compile_options(rpmsg_sim PRIVATE /W3)
else()
    target_compile_options(rpmsg_sim PRIVATE -Wall -Wextra)
endif()

message(STATUS "RPMsg-Lite Simulator configured")
message(STATUS "  Core sources  : ${RPMSG_CORE_SOURCES}")
message(STATUS "  Sim platform  : ${SIM_PLATFORM_SOURCES}")
