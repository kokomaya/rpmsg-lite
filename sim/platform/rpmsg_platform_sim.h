#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

int32_t platform_init(void);
int32_t platform_deinit(void);
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

#define RL_PLATFORM_MAX_ISR_COUNT (16U)
#define RL_PLATFORM_HIGHEST_LINK_ID (0U)

#ifdef __cplusplus
}
#endif
