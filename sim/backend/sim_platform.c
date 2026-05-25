/*
 * RPMsg-Lite Simulator - Platform Layer Implementation
 * Simulates the hardware platform (IPC interrupt, memory mapping, etc.)
 */

#include "rpmsg_platform.h"
#include "rpmsg_env.h"
#include "sim_platform.h"
#include "sim_env.h"
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
 * platform_notify - core of the IPC simulation.
 *
 * When one side calls virtqueue_kick(), the library calls us via
 * vq->notify_fc(). In real hardware this would trigger an IPI on the other
 * core. Here we directly invoke that core's ISR.
 *
 * Vector layout (single link, RL_PLATFORM_SIM_LINK_ID = 0):
 *   vector 0 = vqs[0]: master's RVQ / remote's TVQ -> direction: remote->master
 *   vector 1 = vqs[1]: master's TVQ / remote's RVQ -> direction: master->remote
 *
 * Source-of-kick rules (sender always "kicks its own producer view"):
 *   master sends -> kicks tvq (vector_id=1) -> wakes remote's rvq (vector 1)
 *   remote sends -> kicks tvq (vector_id=0) -> wakes master's rvq (vector 0)
 *   master init  -> kicks rvq (vector_id=0) -> wakes remote's tvq (vector 0,
 *                   rpmsg_lite_tx_callback) -> sets remote->link_state=1
 *   remote rx-freed (consumed_buf_notif) -> kicks rvq -> wakes the other side
 *
 * sim_env::env_isr() reads sim_env_get_current_core() to decide who the
 * "other" core is, so the caller must have set its core via
 * sim_env_set_current_core() before invoking the library API. Nested kicks
 * (e.g. from inside an ISR) use the push/pop stack so routing stays correct.
 */
void platform_notify(uint32_t vector_id)
{
    int src_core = sim_env_get_current_core();
    const char *src_name = (src_core == SIM_CORE_MASTER) ? "master" : "remote";

    sim_event_record_vring(SIM_EVT_VRING_KICK, src_name, "kick",
                           0, 0, (uint16_t)vector_id);

    /* Slow-mode delay between sender-side work and ISR delivery so the user
     * actually sees the kick happen, then the rx callback happen. Step mode
     * pauses the worker thread but the main poll loop keeps pushing events. */
    sim_apply_delay();

    sim_event_record_vring(SIM_EVT_PLATFORM_NOTIFY, src_name, "notify",
                           0, 0, (uint16_t)vector_id);

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
