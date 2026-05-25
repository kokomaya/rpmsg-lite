/*
 * RPMsg-Lite Simulator - Environment Layer Header
 */

#ifndef SIM_ENV_H_
#define SIM_ENV_H_

#include <stdint.h>

/* Shared memory base address (set during sim_init) */
extern uint8_t *g_sim_shmem_base;
extern uint32_t g_sim_shmem_size;

/* Link state signal */
extern volatile int g_sim_link_up_signal;

/* Core context for ISR table separation.
 * In real hardware each core has its own interrupt controller.
 * We simulate this by maintaining per-core ISR tables. */
#define SIM_CORE_MASTER 0
#define SIM_CORE_REMOTE 1

void sim_env_set_current_core(int core_id);
int sim_env_get_current_core(void);

/* Push/pop current-core context for nested ISR dispatch */
void sim_env_push_core(int core_id);
void sim_env_pop_core(void);

/* Query whether the target core has an ISR registered & enabled for vector_id.
 * Used by sim_core to decide whether a kick can actually be delivered. */
int sim_env_isr_ready(int target_core, uint32_t vector_id);

#endif /* SIM_ENV_H_ */
