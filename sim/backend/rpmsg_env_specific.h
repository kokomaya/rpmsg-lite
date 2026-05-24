/*
 * RPMsg-Lite Simulator - Environment Specific Header
 * Defines types required by the rpmsg_env.h interface
 */

#ifndef RPMSG_ENV_SPECIFIC_H_
#define RPMSG_ENV_SPECIFIC_H_

#include <stdint.h>
#include "rpmsg_default_config.h"

/* Queue receive callback data */
typedef struct
{
    uint32_t src;
    void *data;
    uint32_t len;
} rpmsg_queue_rx_cb_data_t;

#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
/* Static context for lock (just a placeholder byte for simulation) */
typedef uint8_t LOCK_STATIC_CONTEXT;
/* Static context for queue */
typedef uint8_t rpmsg_static_queue_ctxt;
#endif

#endif /* RPMSG_ENV_SPECIFIC_H_ */
