/*
 * RPMsg-Lite Simulator - Platform Header (internal)
 */

#ifndef SIM_PLATFORM_H_
#define SIM_PLATFORM_H_

#include <stdint.h>

/* Set the shared memory base for address translation */
void sim_platform_set_shmem(uint8_t *base, uint32_t size);

#endif /* SIM_PLATFORM_H_ */
