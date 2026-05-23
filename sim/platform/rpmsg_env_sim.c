// Minimal PC simulation environment for RPMsg-Lite
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <windows.h>
#include "../core/include/rpmsg_env.h"
#include "rpmsg_platform_sim.h"

static int32_t env_init_counter = 0;

int32_t env_init(void) {
    if (env_init_counter < 0) return -1;
    env_init_counter++;
    if (env_init_counter > 1) return 0;
    return platform_init();
}

int32_t env_deinit(void) {
    if (env_init_counter <= 0) return -1;
    env_init_counter--;
    if (env_init_counter > 0) return 0;
    return platform_deinit();
}

void *env_allocate_memory(uint32_t size) { return malloc(size); }
void env_free_memory(void *ptr) { if (ptr) free(ptr); }
void env_memset(void *ptr, int32_t value, uint32_t size) { memset(ptr, value, size); }
void env_memcpy(void *dst, void const *src, uint32_t len) { memcpy(dst, src, len); }
int32_t env_strcmp(const char *dst, const char *src) { return strcmp(dst, src); }
void env_strncpy(char *dest, const char *src, uint32_t len) { strncpy(dest, src, len); }
int32_t env_strncmp(char *dest, const char *src, uint32_t len) { return strncmp(dest, src, len); }
void env_mb(void) {}
void env_rmb(void) {}
void env_wmb(void) {}
uint32_t env_map_vatopa(void *address) { return (uint32_t)(uintptr_t)address; }
void *env_map_patova(uint32_t address) { return (void *)(uintptr_t)address; }
void env_sleep_msec(uint32_t num_msec) { Sleep(num_msec); }
uint64_t env_get_timestamp(void) { return (uint64_t)clock() * 1000 / CLOCKS_PER_SEC; }
void env_print(const char *fmt, ...) { va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); }
// Mutex, queue, ISR, cache, etc. can be implemented as needed for simulation.
