/*
 * rpmsg_trace.h  —  Instrumentation event definitions for RPMsg-Lite simulator
 *
 * All state changes emit an rpmsg_event_t into a lock-free SPSC ring buffer.
 * The GUI thread polls the ring each frame.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Event Types -------------------------------------------------------- */

typedef enum {
    RPMSG_EVT_NONE               = 0,
    /* Lifecycle */
    RPMSG_EVT_MASTER_INIT_START,
    RPMSG_EVT_MASTER_INIT_DONE,
    RPMSG_EVT_REMOTE_INIT_START,
    RPMSG_EVT_REMOTE_INIT_DONE,
    RPMSG_EVT_DEINIT_START,
    RPMSG_EVT_DEINIT_DONE,
    /* Link state */
    RPMSG_EVT_LINK_UP,
    RPMSG_EVT_LINK_DOWN,
    /* Endpoint */
    RPMSG_EVT_EPT_CREATED,
    RPMSG_EVT_EPT_DESTROYED,
    /* TX path */
    RPMSG_EVT_TX_START,
    RPMSG_EVT_TX_DONE,
    /* RX path */
    RPMSG_EVT_RX_CALLBACK,
    RPMSG_EVT_RX_BUFFER_RELEASED,
    /* Name service */
    RPMSG_EVT_NS_ANNOUNCE,
    RPMSG_EVT_NS_DISCOVER,
    /* VRing */
    RPMSG_EVT_VRING_KICK,
    RPMSG_EVT_VRING_NOTIFY,
    /* Buffer management */
    RPMSG_EVT_BUFFER_ALLOC,
    RPMSG_EVT_BUFFER_FREE,
    /* Error */
    RPMSG_EVT_ERROR,
    RPMSG_EVT__COUNT
} rpmsg_event_type_t;

/* ---- Event payload ------------------------------------------------------ */

#define RPMSG_TRACE_PAYLOAD_PREVIEW 64

typedef struct {
    rpmsg_event_type_t type;
    uint64_t           timestamp_ms;  /* GetTickCount64() */
    uint32_t           side;          /* SIM_SIDE_MASTER or SIM_SIDE_REMOTE */
    uint32_t           extra;         /* generic: vector_id, addr, etc. */

    union {
        /* EPT_CREATED / EPT_DESTROYED */
        struct { uint32_t addr; } ept;

        /* TX_START / TX_DONE / RX_CALLBACK */
        struct {
            uint32_t src;
            uint32_t dst;
            uint16_t len;
            uint16_t flags;
            uint8_t  preview[RPMSG_TRACE_PAYLOAD_PREVIEW];
        } msg;

        /* VRING_KICK / VRING_NOTIFY */
        struct {
            uint32_t vq_id;
            uint16_t avail_idx;
            uint16_t used_idx;
        } vring;

        /* NS_ANNOUNCE / NS_DISCOVER */
        struct {
            char     name[32];
            uint32_t addr;
            uint32_t flags;
        } ns;

        /* ERROR */
        struct {
            int32_t code;
            char    desc[60];
        } error;
    } data;
} rpmsg_event_t;

/* ---- Ring buffer API ---------------------------------------------------- */

#define RPMSG_TRACE_RING_SIZE  (1024u)   /* must be power-of-two */

/* Call once at startup */
void rpmsg_trace_init(void);

/* Producer (sim threads) — non-blocking, drops if full */
void rpmsg_trace_emit(const rpmsg_event_t *evt);

/* Convenience helpers */
void rpmsg_trace_emit_simple(rpmsg_event_type_t type, uint32_t extra);

/* Consumer (GUI / main thread) — returns 1 if an event was dequeued */
int  rpmsg_trace_poll(rpmsg_event_t *out);

/* Returns current number of events waiting */
uint32_t rpmsg_trace_pending(void);

/* ---- Instrumentation macros --------------------------------------------- */
/* Define RPMSG_INSTRUMENTATION_ENABLED=1 to activate */

#if defined(RPMSG_INSTRUMENTATION_ENABLED) && (RPMSG_INSTRUMENTATION_ENABLED == 1)
  #define RPMSG_TRACE(evt_ptr)       rpmsg_trace_emit(evt_ptr)
  #define RPMSG_TRACE_SIMPLE(t, ex)  rpmsg_trace_emit_simple((t), (ex))
#else
  #define RPMSG_TRACE(evt_ptr)       ((void)0)
  #define RPMSG_TRACE_SIMPLE(t, ex)  ((void)0)
#endif

#ifdef __cplusplus
}
#endif
