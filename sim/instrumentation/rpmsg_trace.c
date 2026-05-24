/*
 * rpmsg_trace.c  —  Lock-free SPSC ring buffer implementation
 *
 * Designed for a single producer thread and a single consumer thread.
 * Uses volatile 32-bit indices and a memory fence via _ReadWriteBarrier /
 * __sync_synchronize so no mutex is needed in the hot path.
 */
#include "rpmsg_trace.h"
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  define TRACE_FENCE() _ReadWriteBarrier()
#  define TRACE_TIME()  ((uint64_t)GetTickCount64())
#else
#  include <time.h>
#  define TRACE_FENCE() __sync_synchronize()
static uint64_t TRACE_TIME(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

/* ---- Internal ring ------------------------------------------------------- */

#define RING_MASK (RPMSG_TRACE_RING_SIZE - 1u)

typedef struct {
    rpmsg_event_t      slots[RPMSG_TRACE_RING_SIZE];
    volatile uint32_t  write_idx;   /* only written by producer */
    volatile uint32_t  read_idx;    /* only written by consumer */
} trace_ring_t;

static trace_ring_t g_ring;

/* ---- API ----------------------------------------------------------------- */

void rpmsg_trace_init(void)
{
    memset(&g_ring, 0, sizeof(g_ring));
}

void rpmsg_trace_emit(const rpmsg_event_t *evt)
{
    uint32_t w = g_ring.write_idx;
    uint32_t r = g_ring.read_idx;

    /* Ring full — drop silently (non-blocking) */
    if ((w - r) >= RPMSG_TRACE_RING_SIZE)
        return;

    rpmsg_event_t *slot = &g_ring.slots[w & RING_MASK];
    memcpy(slot, evt, sizeof(*slot));
    slot->timestamp_ms = TRACE_TIME();

    TRACE_FENCE();
    g_ring.write_idx = w + 1u;
}

void rpmsg_trace_emit_simple(rpmsg_event_type_t type, uint32_t extra)
{
    rpmsg_event_t e;
    memset(&e, 0, sizeof(e));
    e.type  = type;
    e.extra = extra;
    rpmsg_trace_emit(&e);
}

int rpmsg_trace_poll(rpmsg_event_t *out)
{
    uint32_t r = g_ring.read_idx;
    uint32_t w = g_ring.write_idx;

    if (r == w) return 0; /* empty */

    TRACE_FENCE();
    memcpy(out, &g_ring.slots[r & RING_MASK], sizeof(*out));
    TRACE_FENCE();
    g_ring.read_idx = r + 1u;
    return 1;
}

uint32_t rpmsg_trace_pending(void)
{
    return g_ring.write_idx - g_ring.read_idx;
}
