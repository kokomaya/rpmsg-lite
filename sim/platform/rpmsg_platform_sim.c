/*
 * rpmsg_platform_sim.c  — Platform layer for PC simulation
 *
 * Interrupt notifications are routed through the IPC bus (ipc_sim).
 * Each side runs a dedicated interrupt-dispatcher thread that calls
 * env_isr() whenever the other side kicks a virtqueue.
 *
 * TLS (Thread Local Storage) tracks which side each thread belongs to.
 */
#include "rpmsg_platform_sim.h"
#include "ipc_sim.h"
#include "rpmsg_trace.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* ---- TLS: side of the calling thread ------------------------------------ */

static DWORD g_tls_side = TLS_OUT_OF_INDEXES;

void platform_set_current_side(uint32_t side)
{
    if (g_tls_side != TLS_OUT_OF_INDEXES)
        TlsSetValue(g_tls_side, (LPVOID)(uintptr_t)side);
}

uint32_t platform_get_current_side(void)
{
    if (g_tls_side == TLS_OUT_OF_INDEXES) return SIM_SIDE_MASTER;
    return (uint32_t)(uintptr_t)TlsGetValue(g_tls_side);
}

/* ---- Forward declares --------------------------------------------------- */

void env_register_isr(uint32_t vector_id, void *data);
void env_unregister_isr(uint32_t vector_id);
void env_isr(uint32_t vector);

/* ---- Per-side ISR thread ------------------------------------------------ */

typedef struct {
    uint32_t side;
    HANDLE   thread;
    volatile int running;
} isr_thread_ctx_t;

static isr_thread_ctx_t g_isr_ctx[SIM_NUM_SIDES];

static DWORD WINAPI isr_dispatcher(LPVOID arg)
{
    isr_thread_ctx_t *ctx = (isr_thread_ctx_t *)arg;
    platform_set_current_side(ctx->side);  /* TLS: this thread belongs to 'side' */
    uint32_t vq_id = 0;
    while (ctx->running) {
        if (ipc_sim_wait(ctx->side, &vq_id, 200) == 0) {
            env_isr(vq_id);
        }
    }
    return 0;
}

/* ---- Platform API ------------------------------------------------------- */

int32_t platform_init(void)
{
    g_tls_side = TlsAlloc();
    ipc_sim_init();
    for (int i = 0; i < SIM_NUM_SIDES; i++) {
        g_isr_ctx[i].side    = (uint32_t)i;
        g_isr_ctx[i].running = 1;
        g_isr_ctx[i].thread  = CreateThread(NULL, 0, isr_dispatcher, &g_isr_ctx[i], 0, NULL);
    }
    return 0;
}

int32_t platform_deinit(void)
{
    for (int i = 0; i < SIM_NUM_SIDES; i++) {
        g_isr_ctx[i].running = 0;
        /* wake the blocked wait so the thread exits gracefully */
        ipc_sim_signal_from(1u - (uint32_t)i, 0u);
        WaitForSingleObject(g_isr_ctx[i].thread, 1000);
        CloseHandle(g_isr_ctx[i].thread);
    }
    if (g_tls_side != TLS_OUT_OF_INDEXES) {
        TlsFree(g_tls_side);
        g_tls_side = TLS_OUT_OF_INDEXES;
    }
    ipc_sim_deinit();
    return 0;
}

/* platform_init_interrupt: register the virtqueue pointer in the env ISR table */
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

int32_t platform_interrupt_enable(uint32_t vector_id)  { (void)vector_id; return 0; }
int32_t platform_interrupt_disable(uint32_t vector_id) { (void)vector_id; return 0; }
int32_t platform_in_isr(void)                          { return 0; }

/* platform_notify: kick the OPPOSITE side's ISR dispatcher */
void platform_notify(uint32_t vector_id)
{
    uint32_t my_side = platform_get_current_side();
    rpmsg_trace_emit_simple(RPMSG_EVT_VRING_KICK, vector_id);
    ipc_sim_signal_from(my_side, vector_id);
}

void platform_time_delay(uint32_t num_msec)              { Sleep(num_msec); }
void platform_map_mem_region(uint32_t va, uint32_t pa, uint32_t sz, uint32_t fl) { (void)va;(void)pa;(void)sz;(void)fl; }
void platform_cache_all_flush_invalidate(void)           {}
void platform_cache_disable(void)                        {}
void platform_cache_invalidate(void *d, uint32_t l)      { (void)d;(void)l; }
void platform_cache_flush(void *d, uint32_t l)           { (void)d;(void)l; }
uintptr_t platform_vatopa(void *addr)                    { return (uintptr_t)addr; }
void     *platform_patova(uintptr_t addr)                { return (void *)addr; }
