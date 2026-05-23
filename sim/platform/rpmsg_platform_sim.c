// Minimal PC simulation platform for RPMsg-Lite
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include "rpmsg_platform_sim.h"

static int32_t platform_initialized = 0;

int32_t platform_init(void) {
    platform_initialized = 1;
    return 0;
}

int32_t platform_deinit(void) {
    platform_initialized = 0;
    return 0;
}

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data) { return 0; }
int32_t platform_deinit_interrupt(uint32_t vector_id) { return 0; }
int32_t platform_interrupt_enable(uint32_t vector_id) { return 0; }
int32_t platform_interrupt_disable(uint32_t vector_id) { return 0; }
int32_t platform_in_isr(void) { return 0; }
void platform_notify(uint32_t vector_id) { printf("[SIM] platform_notify(%u)\n", vector_id); }
void platform_time_delay(uint32_t num_msec) { Sleep(num_msec); }
void platform_map_mem_region(uint32_t vrt_addr, uint32_t phy_addr, uint32_t size, uint32_t flags) {}
void platform_cache_all_flush_invalidate(void) {}
void platform_cache_disable(void) {}
void platform_cache_invalidate(void *data, uint32_t len) {}
void platform_cache_flush(void *data, uint32_t len) {}
uintptr_t platform_vatopa(void *addr) { return (uintptr_t)addr; }
void *platform_patova(uintptr_t addr) { return (void *)addr; }
