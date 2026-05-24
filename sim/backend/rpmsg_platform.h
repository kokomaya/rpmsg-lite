/*
 * RPMsg-Lite Simulator Platform Header
 * Defines platform-specific constants for the simulation environment
 */

#ifndef RPMSG_PLATFORM_H_
#define RPMSG_PLATFORM_H_

#include <stdint.h>
#include "virtio_ring.h"

/* VRing alignment - 16 bytes */
#ifndef VRING_ALIGN
#define VRING_ALIGN (0x10U)
#endif

/* Calculate VRING_SIZE based on buffer count */
#ifndef VRING_SIZE
#define VRING_DESC_SIZE  (((RL_BUFFER_COUNT * sizeof(struct vring_desc)) + VRING_ALIGN - 1UL) & ~(VRING_ALIGN - 1UL))
#define VRING_AVAIL_SIZE (((sizeof(struct vring_avail) + (RL_BUFFER_COUNT * sizeof(uint16_t)) + sizeof(uint16_t)) + VRING_ALIGN - 1UL) & ~(VRING_ALIGN - 1UL))
#define VRING_USED_SIZE  (((sizeof(struct vring_used) + (RL_BUFFER_COUNT * sizeof(struct vring_used_elem)) + sizeof(uint16_t)) + VRING_ALIGN - 1UL) & ~(VRING_ALIGN - 1UL))
#define VRING_SIZE       (VRING_DESC_SIZE + VRING_AVAIL_SIZE + VRING_USED_SIZE)
#endif

/* Shared memory overhead for VRings (2 directions) */
#define RL_VRING_OVERHEAD (2UL * VRING_SIZE)

/* Maximum ISR count */
#ifndef RL_PLATFORM_MAX_ISR_COUNT
#define RL_PLATFORM_MAX_ISR_COUNT (4U)
#endif

/* VQ ID macros */
#define RL_GET_VQ_ID(link_id, queue_id) (((queue_id) & 0x1U) | (((link_id) << 1U) & 0xFFFFFFFEU))
#define RL_GET_LINK_ID(id)              (((id) & 0xFFFFFFFEU) >> 1U)
#define RL_GET_Q_ID(id)                 ((id) & 0x1U)

/* Simulation uses a single link */
#define RL_PLATFORM_SIM_LINK_ID     (0U)
#define RL_PLATFORM_HIGHEST_LINK_ID (0U)

/* Platform function declarations */
int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data);
int32_t platform_deinit_interrupt(uint32_t vector_id);
int32_t platform_interrupt_enable(uint32_t vector_id);
int32_t platform_interrupt_disable(uint32_t vector_id);
int32_t platform_in_isr(void);
void platform_notify(uint32_t vector_id);

void platform_time_delay(uint32_t num_msec);

void platform_map_mem_region(uint32_t vrt_addr, uint32_t phy_addr, uint32_t size, uint32_t flags);
void platform_cache_all_flush_invalidate(void);
void platform_cache_disable(void);
void platform_cache_invalidate(void *data, uint32_t len);
void platform_cache_flush(void *data, uint32_t len);
uintptr_t platform_vatopa(void *addr);
void *platform_patova(uintptr_t addr);

int32_t platform_init(void);
int32_t platform_deinit(void);

#endif /* RPMSG_PLATFORM_H_ */
