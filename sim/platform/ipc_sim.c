/*
 * ipc_sim.c  —  Win32 implementation of the simulated IPC bus
 */
#include "ipc_sim.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_PENDING_VQ 32

typedef struct {
    HANDLE      event;              /* auto-reset Win32 event */
    CRITICAL_SECTION cs;
    uint32_t    queue[MAX_PENDING_VQ];
    uint32_t    head, tail, count;
    void       *rpmsg_instance;     /* struct rpmsg_lite_instance* */
} sim_side_t;

static sim_side_t g_sides[SIM_NUM_SIDES];

void ipc_sim_init(void)
{
    for (int i = 0; i < SIM_NUM_SIDES; i++) {
        memset(&g_sides[i], 0, sizeof(sim_side_t));
        g_sides[i].event = CreateEventW(NULL, FALSE, FALSE, NULL); /* auto-reset */
        InitializeCriticalSection(&g_sides[i].cs);
    }
}

void ipc_sim_deinit(void)
{
    for (int i = 0; i < SIM_NUM_SIDES; i++) {
        CloseHandle(g_sides[i].event);
        DeleteCriticalSection(&g_sides[i].cs);
    }
}

void ipc_sim_signal_from(uint32_t source_side, uint32_t vq_id)
{
    /* Always notify the OPPOSITE side */
    uint32_t target = (source_side == SIM_SIDE_MASTER) ? SIM_SIDE_REMOTE : SIM_SIDE_MASTER;
    sim_side_t *s = &g_sides[target];

    EnterCriticalSection(&s->cs);
    if (s->count < MAX_PENDING_VQ) {
        s->queue[s->tail] = vq_id;
        s->tail = (s->tail + 1) % MAX_PENDING_VQ;
        s->count++;
    }
    LeaveCriticalSection(&s->cs);
    SetEvent(s->event);
}

int32_t ipc_sim_wait(uint32_t side, uint32_t *vq_id_out, uint32_t timeout_ms)
{
    sim_side_t *s = &g_sides[side];
    DWORD wait_ms = (timeout_ms == 0) ? INFINITE : (DWORD)timeout_ms;

    DWORD result = WaitForSingleObject(s->event, wait_ms);
    if (result != WAIT_OBJECT_0) {
        return -1; /* timeout */
    }

    EnterCriticalSection(&s->cs);
    if (s->count == 0) {
        LeaveCriticalSection(&s->cs);
        return -1;
    }
    *vq_id_out = s->queue[s->head];
    s->head = (s->head + 1) % MAX_PENDING_VQ;
    s->count--;
    /* If more items remain, re-signal so next wait picks them up */
    if (s->count > 0) {
        SetEvent(s->event);
    }
    LeaveCriticalSection(&s->cs);
    return 0;
}

void ipc_sim_register_side(uint32_t side, void *rpmsg_instance)
{
    g_sides[side].rpmsg_instance = rpmsg_instance;
}

void *ipc_sim_get_instance(uint32_t side)
{
    return g_sides[side].rpmsg_instance;
}
