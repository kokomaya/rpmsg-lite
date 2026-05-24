#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
// Simulator config for RPMsg-Lite
#define RL_MS_PER_INTERVAL         (10U)
#define RL_BUFFER_PAYLOAD_SIZE     (496U)
#define RL_BUFFER_COUNT            (8U)
#define RL_API_HAS_ZEROCOPY        (1)
#define RL_USE_STATIC_API          (0)
#define RL_USE_ENVIRONMENT_CONTEXT (0)
#define RL_ALLOW_CUSTOM_SHMEM_CONFIG (0)
#define RL_PLATFORM_HIGHEST_LINK_ID  (0U)
#define VRING_ALIGN                (0x10U)
#define RL_USE_DCACHE              (0)

/* Override RL_ASSERT with a diagnostic version for the simulator */
#define RL_ASSERT(x) \
    do { \
        if (!((int32_t)(x) != 0)) { \
            printf("[ASSERT FAIL] %s:%d  expr=(" #x ")\n", __FILE__, __LINE__); \
            fflush(stdout); \
            ExitThread(99); \
        } \
    } while (0)
