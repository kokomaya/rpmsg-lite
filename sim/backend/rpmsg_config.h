/*
 * RPMsg-Lite Simulator Configuration
 * Simulation-specific settings for the rpmsg protocol
 */

#ifndef RPMSG_CONFIG_H_
#define RPMSG_CONFIG_H_

/* Polling interval */
#define RL_MS_PER_INTERVAL (1)

/* No custom shmem config - use global settings */
#define RL_ALLOW_CUSTOM_SHMEM_CONFIG (0)

/* Buffer payload size: 496 bytes (standard) */
#define RL_BUFFER_PAYLOAD_SIZE (496U)

/* 4 buffers per direction for more interesting visualization */
#define RL_BUFFER_COUNT (4U)

/* Enable zero-copy API */
#define RL_API_HAS_ZEROCOPY (1)

/* Use static API (no dynamic allocation for core structures) */
#ifndef RL_USE_STATIC_API
#define RL_USE_STATIC_API (1)
#endif

/* Clear buffers when released (for visualization clarity) */
#define RL_CLEAR_USED_BUFFERS (1)

/* No data cache management needed in simulation */
#define RL_USE_DCACHE (0)

/* No MCMGR */
#define RL_USE_MCMGR_IPC_ISR_HANDLER (0)

/* No environment context needed */
#define RL_USE_ENVIRONMENT_CONTEXT (0)

/* Enable buffer debug checking */
#define RL_DEBUG_CHECK_BUFFERS (1)

/* Enable consumed buffers notification */
#define RL_ALLOW_CONSUMED_BUFFERS_NOTIFICATION (1)

/* Assert: log and continue in simulation */
#include <stdio.h>
#include <stdlib.h>
#define RL_ASSERT(x)                                                           \
    do                                                                         \
    {                                                                           \
        if (!(x))                                                              \
        {                                                                       \
            fprintf(stderr, "RL_ASSERT failed: %s, file %s, line %d\n",        \
                    #x, __FILE__, __LINE__);                                    \
        }                                                                       \
    } while (0)

#endif /* RPMSG_CONFIG_H_ */
