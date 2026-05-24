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

#endif /* SIM_ENV_H_ */
