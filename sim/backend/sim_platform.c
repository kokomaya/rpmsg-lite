/*
 * RPMsg-Lite Simulator - Platform Layer Implementation
 * Simulates the hardware platform (IPC interrupt, memory mapping, etc.)
 */

#include "rpmsg_platform.h"
#include "rpmsg_env.h"
#include "sim_platform.h"
#include "sim_events.h"
#include "sim_core.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Shared memory base for VA/PA translation */
static uint8_t *s_shmem_base = NULL;
static uint32_t s_shmem_size = 0;

void sim_platform_set_shmem(uint8_t *base, uint32_t size)
{
    s_shmem_base = base;
    s_shmem_size = size;
}

/* ========================================================================== */
/* Platform Interrupt                                                           */
/* ========================================================================== */

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data)
{
    env_register_isr(vector_id, isr_data);
    return 0;
}

int32_t platform_deinit_interrupt(uint32_t vector_id)
{
    env_unregister_isr(vector_id);
    return 0;
}

int32_t platform_interrupt_enable(uint32_t vector_id)
{
    env_enable_interrupt(vector_id);
    return 0;
}

int32_t platform_interrupt_disable(uint32_t vector_id)
{
    env_disable_interrupt(vector_id);
    return 0;
}

int32_t platform_in_isr(void)
{
    return 0; /* Never in ISR context in simulation */
}

/*
 * platform_notify - This is the core IPC simulation.
 * When one side calls virtqueue_kick(), it ultimately calls this function.
 * We simulate the "interrupt" by directly invoking env_isr on the OTHER side's vector.
 *
 * VQ ID encoding: bit0 = queue_id (0=RX, 1=TX), rest = link_id
 * Master TX vq has queue_index=0, Remote TX vq has queue_index=1
 *
 * When master kicks its TX queue (vq_queue_index=0):
 *   vector_id = RL_GET_VQ_ID(0, 0) = 0  -> should trigger remote's RX = vector 0
 *
 * When remote kicks its TX queue (vq_queue_index=1):
 *   vector_id = RL_GET_VQ_ID(0, 1) = 1  -> should trigger master's RX = vector 1
 */
void platform_notify(uint32_t vector_id)
{
    /* Record the notification event */
    sim_event_record_vring(SIM_EVT_PLATFORM_NOTIFY,
                           (vector_id & 1) ? "remote" : "master",
                           "notify", 0, 0, (uint16_t)vector_id);

    /* Apply slow-motion delay */
    sim_apply_delay();

    /* Directly invoke the ISR on the target vector.
     * In real hardware, this would be an inter-processor interrupt.
     * The vector_id maps directly to the registered ISR data. */
    env_isr(vector_id);
}

/* ========================================================================== */
/* Platform Time                                                                */
/* ========================================================================== */

void platform_time_delay(uint32_t num_msec)
{
#ifdef _WIN32
    Sleep(num_msec);
#else
    usleep(num_msec * 1000);
#endif
}

/* ========================================================================== */
/* Platform Memory (identity mapping for simulation)                           */
/* ========================================================================== */

void platform_map_mem_region(uint32_t vrt_addr, uint32_t phy_addr, uint32_t size, uint32_t flags)
{
    (void)vrt_addr;
    (void)phy_addr;
    (void)size;
    (void)flags;
}

void platform_cache_all_flush_invalidate(void)
{
}

void platform_cache_disable(void)
{
}

void platform_cache_invalidate(void *data, uint32_t len)
{
    (void)data;
    (void)len;
}

void platform_cache_flush(void *data, uint32_t len)
{
    (void)data;
    (void)len;
}

uintptr_t platform_vatopa(void *addr)
{
    return (uintptr_t)addr;
}

void *platform_patova(uintptr_t addr)
{
    return (void *)addr;
}

/* ========================================================================== */
/* Platform Init/Deinit                                                        */
/* ========================================================================== */

int32_t platform_init(void)
{
    return 0;
}

int32_t platform_deinit(void)
{
    return 0;
}
