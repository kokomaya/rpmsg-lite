/*
 * RPMsg-Lite Simulator - Core Header
 * Manages Master and Remote rpmsg instances in a single process
 */

#ifndef SIM_CORE_H_
#define SIM_CORE_H_

#include <stdint.h>
#include "rpmsg_lite.h"

/* Buffer size as defined in rpmsg_lite.c (payload + 16 byte header, word-aligned) */
#ifndef RL_BUFFER_SIZE
#define RL_BUFFER_SIZE_RAW (RL_BUFFER_PAYLOAD_SIZE + 16UL)
#define RL_BUFFER_SIZE RL_WORD_ALIGN_UP(RL_BUFFER_SIZE_RAW)
#endif

/* Simulation run modes */
typedef enum
{
    SIM_MODE_NORMAL = 0, /* Full speed */
    SIM_MODE_SLOW,       /* Configurable delay between operations */
    SIM_MODE_STEP,       /* Pause before each operation, wait for user */
} sim_mode_t;

/* Shared memory size for simulation */
#define SIM_SHMEM_SIZE (2U * RL_VRING_OVERHEAD + 2U * RL_BUFFER_COUNT * RL_BUFFER_SIZE)

/* Maximum endpoints per core */
#define SIM_MAX_ENDPOINTS 8

/* Endpoint info for serialization */
typedef struct
{
    uint32_t addr;
    struct rpmsg_lite_endpoint *ept;
    struct rpmsg_lite_ept_static_context ept_ctx;
    int active;
} sim_ept_info_t;

/* Core state */
typedef struct
{
    struct rpmsg_lite_instance *instance;
    struct rpmsg_lite_instance instance_static;
    int initialized;
    sim_ept_info_t endpoints[SIM_MAX_ENDPOINTS];
    int ept_count;
} sim_core_state_t;

/* Global simulation state */
typedef struct
{
    sim_core_state_t master;
    sim_core_state_t remote;
    uint8_t *shmem;
    uint32_t shmem_size;
    sim_mode_t mode;
    uint32_t delay_ms;
    volatile int step_pending; /* 1 = waiting for step command */
    volatile int running;
} sim_state_t;

/* Get global sim state */
sim_state_t *sim_get_state(void);

/* Initialize simulation */
int sim_init(void);

/* Reset simulation */
void sim_reset(void);

/* Initialize master side */
int sim_init_master(void);

/* Initialize remote side */
int sim_init_remote(void);

/* Deinitialize a core */
int sim_deinit_core(const char *core);

/* Create endpoint */
int sim_create_ept(const char *core, uint32_t addr);

/* Destroy endpoint */
int sim_destroy_ept(const char *core, uint32_t addr);

/* Send message */
int sim_send(const char *core, uint32_t src, uint32_t dst,
             const uint8_t *data, uint32_t len);

/* Set simulation mode */
void sim_set_mode(sim_mode_t mode, uint32_t delay_ms);

/* Step (advance one operation in step mode) */
void sim_step(void);

/* Get full state as JSON */
int sim_state_to_json(char *buf, int buf_size);

/* Check if link is up */
int sim_is_link_up(const char *core);

/* Apply slow-motion delay if needed */
void sim_apply_delay(void);

#endif /* SIM_CORE_H_ */
