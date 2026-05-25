/*
 * RPMsg-Lite Simulator - Core Logic Implementation
 *
 * This file glues the *unmodified* rpmsg-lite library to:
 *   - a fake shared-memory region (g_sim_shmem_*),
 *   - a fake IPC (platform_notify -> env_isr in sim_platform.c / sim_env.c),
 *   - a Master/Remote pair of rpmsg_lite_instances living in the same process,
 *   - an event stream that mirrors every meaningful internal state change
 *     to the web UI so the user can *see* the protocol working.
 *
 * Original rpmsg_lite.c / virtqueue.c are untouched. All observability comes
 * from sampling vq state *around* each library call here.
 */

#include "sim_core.h"
#include "sim_env.h"
#include "sim_platform.h"
#include "sim_events.h"
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "rpmsg_queue.h"
#include "virtqueue.h"
#include "virtio_ring.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif

/* Global simulation state */
static sim_state_t g_sim_state;

/* Step-mode signaling: sim_apply_delay() waits on this when paused; sim_step()
 * signals it from any thread. Auto-reset semantics on Windows, manual
 * count-based on pthread. */
#ifdef _WIN32
static HANDLE g_step_event = NULL;
#else
static pthread_mutex_t g_step_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_step_cond = PTHREAD_COND_INITIALIZER;
static volatile int    g_step_signal = 0;
#endif

sim_state_t *sim_get_state(void)
{
    return &g_sim_state;
}

/* ========================================================================== */
/* VQ snapshot helpers (used to diff state around rpmsg-lite calls)            */
/* ========================================================================== */

typedef struct
{
    uint16_t desc_head_idx;
    uint16_t avail_idx;       /* internal cursor */
    uint16_t avail_ring_idx;  /* shared avail->idx */
    uint16_t used_cons_idx;
    uint16_t used_ring_idx;   /* shared used->idx */
} vq_snap_t;

static void vq_snap(struct virtqueue *vq, vq_snap_t *s)
{
    if (!vq) { memset(s, 0, sizeof(*s)); return; }
    s->desc_head_idx  = vq->vq_desc_head_idx;
    s->avail_idx      = vq->vq_available_idx;
    s->avail_ring_idx = vq->vq_ring.avail ? vq->vq_ring.avail->idx : 0;
    s->used_cons_idx  = vq->vq_used_cons_idx;
    s->used_ring_idx  = vq->vq_ring.used  ? vq->vq_ring.used->idx  : 0;
}

/* ========================================================================== */
/* RX callback - records the receive event for the web UI                     */
/* ========================================================================== */

static int32_t sim_rx_callback(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    sim_ept_info_t *slot = (sim_ept_info_t *)priv;
    const char *core = "unknown";
    uint32_t dst_addr = 0;

    if (slot)
    {
        dst_addr = slot->addr;
        /* Find which core this slot belongs to */
        for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
        {
            if (&g_sim_state.master.endpoints[i] == slot) { core = "master"; break; }
            if (&g_sim_state.remote.endpoints[i] == slot) { core = "remote"; break; }
        }
    }

    sim_event_record_msg(SIM_EVT_RX_CALLBACK, core, "rx",
                         src, dst_addr, payload_len,
                         (const uint8_t *)payload, payload_len);

    return RL_RELEASE;
}

/* ========================================================================== */
/* Init / Reset                                                               */
/* ========================================================================== */

int sim_init(void)
{
    memset(&g_sim_state, 0, sizeof(g_sim_state));
    g_sim_state.mode = SIM_MODE_NORMAL;
    g_sim_state.delay_ms = 500;
    g_sim_state.running = 1;

#ifdef _WIN32
    if (g_step_event == NULL)
        g_step_event = CreateEvent(NULL, FALSE /*auto-reset*/, FALSE, NULL);
#endif

    g_sim_state.shmem_size = SIM_SHMEM_SIZE;
    g_sim_state.shmem = (uint8_t *)malloc(g_sim_state.shmem_size);
    if (!g_sim_state.shmem)
    {
        fprintf(stderr, "Failed to allocate shared memory (%u bytes)\n", g_sim_state.shmem_size);
        return -1;
    }
    memset(g_sim_state.shmem, 0, g_sim_state.shmem_size);

    g_sim_shmem_base = g_sim_state.shmem;
    g_sim_shmem_size = g_sim_state.shmem_size;
    sim_platform_set_shmem(g_sim_state.shmem, g_sim_state.shmem_size);

    sim_events_init();

    printf("[SIM] Initialized. Shared memory: %u bytes at %p\n",
           g_sim_state.shmem_size, (void *)g_sim_state.shmem);
    return 0;
}

void sim_reset(void)
{
    /* Wake any blocked step-wait so deinit can proceed */
    g_sim_state.step_pending = 0;
#ifdef _WIN32
    if (g_step_event) SetEvent(g_step_event);
#else
    pthread_mutex_lock(&g_step_lock);
    g_step_signal = 1;
    pthread_cond_broadcast(&g_step_cond);
    pthread_mutex_unlock(&g_step_lock);
#endif

    if (g_sim_state.master.initialized) sim_deinit_core("master");
    if (g_sim_state.remote.initialized) sim_deinit_core("remote");

    if (g_sim_state.shmem)
        memset(g_sim_state.shmem, 0, g_sim_state.shmem_size);

    g_sim_link_up_signal = 0;
    sim_events_init();
    printf("[SIM] Reset complete\n");
}

/* ========================================================================== */
/* Master / Remote Init                                                       */
/* ========================================================================== */

int sim_init_master(void)
{
    if (g_sim_state.master.initialized) return -1;

    sim_apply_delay();
    sim_event_record(SIM_EVT_INIT_MASTER, "master", NULL);

    sim_env_set_current_core(SIM_CORE_MASTER);
    env_init();

    g_sim_state.master.instance = rpmsg_lite_master_init(
        g_sim_state.shmem,
        g_sim_state.shmem_size,
        RL_PLATFORM_SIM_LINK_ID,
        RL_NO_FLAGS,
        &g_sim_state.master.instance_static);

    if (g_sim_state.master.instance == RL_NULL) return -1;

    g_sim_state.master.initialized = 1;
    sim_event_record(SIM_EVT_LINK_UP, "master", NULL);
    printf("[SIM] Master initialized, link_state=%u\n",
           g_sim_state.master.instance->link_state);
    return 0;
}

int sim_init_remote(void)
{
    if (g_sim_state.remote.initialized) return -1;

    sim_apply_delay();
    sim_event_record(SIM_EVT_INIT_REMOTE, "remote", NULL);

    sim_env_set_current_core(SIM_CORE_REMOTE);
    g_sim_state.remote.instance = rpmsg_lite_remote_init(
        g_sim_state.shmem,
        RL_PLATFORM_SIM_LINK_ID,
        RL_NO_FLAGS,
        &g_sim_state.remote.instance_static);

    if (g_sim_state.remote.instance == RL_NULL) return -1;

    g_sim_state.remote.initialized = 1;

    /* Real-hardware handshake:
     *   master_init() ends with virtqueue_kick(master->rvq) → IPI → remote's
     *   tx_callback → remote->link_state = 1.
     * If master was initialized FIRST, that kick was lost (remote had no ISR
     * yet). Replay it now so the user sees the handshake event. */
    if (g_sim_state.master.initialized)
    {
        sim_env_set_current_core(SIM_CORE_MASTER);
        virtqueue_kick(g_sim_state.master.instance->rvq);
        sim_env_set_current_core(SIM_CORE_REMOTE);
    }

    printf("[SIM] Remote initialized, link_state=%u\n",
           g_sim_state.remote.instance->link_state);
    return 0;
}

/* ========================================================================== */
/* Deinit                                                                     */
/* ========================================================================== */

int sim_deinit_core(const char *core)
{
    sim_core_state_t *cs = NULL;
    int core_id = 0;

    if (strcmp(core, "master") == 0) { cs = &g_sim_state.master; core_id = SIM_CORE_MASTER; }
    else if (strcmp(core, "remote") == 0) { cs = &g_sim_state.remote; core_id = SIM_CORE_REMOTE; }
    else return -1;

    if (!cs->initialized) return -1;

    sim_apply_delay();
    sim_event_record(SIM_EVT_DEINIT, core, NULL);

    sim_env_set_current_core(core_id);

    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        if (cs->endpoints[i].active && cs->endpoints[i].ept)
        {
            rpmsg_lite_destroy_ept(cs->instance, cs->endpoints[i].ept);
            cs->endpoints[i].active = 0;
            cs->endpoints[i].ept = NULL;
        }
    }
    cs->ept_count = 0;

    rpmsg_lite_deinit(cs->instance);
    cs->initialized = 0;
    cs->instance = NULL;
    printf("[SIM] %s deinitialized\n", core);
    return 0;
}

/* ========================================================================== */
/* Endpoints                                                                  */
/* ========================================================================== */

int sim_create_ept(const char *core, uint32_t addr)
{
    sim_core_state_t *cs = NULL;
    int core_id = 0;

    if (strcmp(core, "master") == 0) { cs = &g_sim_state.master; core_id = SIM_CORE_MASTER; }
    else if (strcmp(core, "remote") == 0) { cs = &g_sim_state.remote; core_id = SIM_CORE_REMOTE; }
    else return -1;

    if (!cs->initialized || cs->ept_count >= SIM_MAX_ENDPOINTS) return -1;

    int slot = -1;
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
        if (!cs->endpoints[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    sim_apply_delay();
    sim_env_set_current_core(core_id);

    cs->endpoints[slot].ept = rpmsg_lite_create_ept(
        cs->instance, addr, sim_rx_callback,
        &cs->endpoints[slot], &cs->endpoints[slot].ept_ctx);

    if (cs->endpoints[slot].ept == RL_NULL)
    {
        fprintf(stderr, "[SIM] Failed to create endpoint %u on %s\n", addr, core);
        return -1;
    }

    cs->endpoints[slot].addr = cs->endpoints[slot].ept->addr;
    cs->endpoints[slot].active = 1;
    cs->ept_count++;

    sim_event_record_ept(SIM_EVT_EPT_CREATE, core, cs->endpoints[slot].addr);
    printf("[SIM] Created endpoint %u on %s\n", cs->endpoints[slot].addr, core);
    return 0;
}

int sim_destroy_ept(const char *core, uint32_t addr)
{
    sim_core_state_t *cs = NULL;
    int core_id = 0;

    if (strcmp(core, "master") == 0) { cs = &g_sim_state.master; core_id = SIM_CORE_MASTER; }
    else if (strcmp(core, "remote") == 0) { cs = &g_sim_state.remote; core_id = SIM_CORE_REMOTE; }
    else return -1;

    if (!cs->initialized) return -1;

    sim_env_set_current_core(core_id);

    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        if (cs->endpoints[i].active && cs->endpoints[i].addr == addr)
        {
            sim_apply_delay();
            rpmsg_lite_destroy_ept(cs->instance, cs->endpoints[i].ept);
            cs->endpoints[i].active = 0;
            cs->endpoints[i].ept = NULL;
            cs->ept_count--;
            sim_event_record_ept(SIM_EVT_EPT_DESTROY, core, addr);
            printf("[SIM] Destroyed endpoint %u on %s\n", addr, core);
            return 0;
        }
    }
    return -1;
}

/* ========================================================================== */
/* Send (with synthetic events around the rpmsg call to expose internals)     */
/* ========================================================================== */

int sim_send(const char *core, uint32_t src, uint32_t dst,
             const uint8_t *data, uint32_t len)
{
    sim_core_state_t *cs = NULL;
    int core_id = 0;

    if (strcmp(core, "master") == 0) { cs = &g_sim_state.master; core_id = SIM_CORE_MASTER; }
    else if (strcmp(core, "remote") == 0) { cs = &g_sim_state.remote; core_id = SIM_CORE_REMOTE; }
    else return -1;

    if (!cs->initialized) return -1;

    struct rpmsg_lite_endpoint *ept = NULL;
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
        if (cs->endpoints[i].active && cs->endpoints[i].addr == src)
            { ept = cs->endpoints[i].ept; break; }

    if (!ept)
    {
        fprintf(stderr, "[SIM] Source endpoint %u not found on %s\n", src, core);
        return -1;
    }

    sim_apply_delay();

    /* Snapshot tvq before the call so we can describe what changed */
    vq_snap_t before, after;
    vq_snap(cs->instance->tvq, &before);

    sim_event_record_msg(SIM_EVT_TX_SEND, core, "tx", src, dst, len, data, len);

    sim_env_set_current_core(core_id);
    int32_t ret = rpmsg_lite_send(cs->instance, ept, dst, (char *)data, len, RL_DONT_BLOCK);

    if (ret != RL_SUCCESS)
    {
        fprintf(stderr, "[SIM] Send failed on %s: %d\n", core, (int)ret);
        return (int)ret;
    }

    /* Describe what rpmsg_lite_send did internally:
     *   master TX path: vq_tx_alloc -> get a free desc (head idx changes),
     *                   vq_tx -> add_buffer (avail->idx++),
     *                   virtqueue_kick -> notify.
     *   remote TX path: vq_tx_alloc -> get from avail ring (avail_idx++),
     *                   vq_tx -> add_consumed_buffer (used->idx++),
     *                   virtqueue_kick -> notify.
     * platform_notify itself emits its own event from sim_platform.c. */
    vq_snap(cs->instance->tvq, &after);
    if (after.desc_head_idx != before.desc_head_idx ||
        after.avail_idx != before.avail_idx)
    {
        /* TX buffer was allocated. For master, head_idx changed; for remote,
         * avail_idx changed. Record both. */
        sim_event_record_vring(SIM_EVT_TX_ALLOC, core, "tx",
                               after.desc_head_idx,
                               after.avail_idx,
                               after.used_cons_idx);
    }
    if (after.avail_ring_idx != before.avail_ring_idx ||
        after.used_ring_idx != before.used_ring_idx)
    {
        sim_event_record_vring(SIM_EVT_VRING_ADD_BUFFER, core, "tx",
                               after.desc_head_idx,
                               after.avail_ring_idx,
                               after.used_ring_idx);
    }

    printf("[SIM] Sent %u bytes from %s:%u to %u\n", len, core, src, dst);
    return 0;
}

/* ========================================================================== */
/* Mode Control                                                               */
/* ========================================================================== */

void sim_set_mode(sim_mode_t mode, uint32_t delay_ms)
{
    /* If leaving step mode, release any waiter */
    sim_mode_t old = g_sim_state.mode;
    g_sim_state.mode = mode;
    if (delay_ms > 0) g_sim_state.delay_ms = delay_ms;

    if (old == SIM_MODE_STEP && mode != SIM_MODE_STEP)
    {
        g_sim_state.step_pending = 0;
#ifdef _WIN32
        if (g_step_event) SetEvent(g_step_event);
#else
        pthread_mutex_lock(&g_step_lock);
        g_step_signal = 1;
        pthread_cond_broadcast(&g_step_cond);
        pthread_mutex_unlock(&g_step_lock);
#endif
    }
    printf("[SIM] Mode=%d delay=%ums\n", (int)mode, g_sim_state.delay_ms);
}

void sim_step(void)
{
    g_sim_state.step_pending = 0;
#ifdef _WIN32
    if (g_step_event) SetEvent(g_step_event);
#else
    pthread_mutex_lock(&g_step_lock);
    g_step_signal = 1;
    pthread_cond_broadcast(&g_step_cond);
    pthread_mutex_unlock(&g_step_lock);
#endif
}

void sim_apply_delay(void)
{
    if (!g_sim_state.running) return;

    if (g_sim_state.mode == SIM_MODE_SLOW)
    {
#ifdef _WIN32
        Sleep(g_sim_state.delay_ms);
#else
        usleep(g_sim_state.delay_ms * 1000);
#endif
    }
    else if (g_sim_state.mode == SIM_MODE_STEP)
    {
        g_sim_state.step_pending = 1;
#ifdef _WIN32
        /* Wait up to ~24 days; broken out periodically so reset can interrupt */
        while (g_sim_state.step_pending && g_sim_state.running &&
               g_sim_state.mode == SIM_MODE_STEP)
        {
            WaitForSingleObject(g_step_event, 200);
        }
#else
        pthread_mutex_lock(&g_step_lock);
        while (g_sim_state.step_pending && g_sim_state.running &&
               g_sim_state.mode == SIM_MODE_STEP && !g_step_signal)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_step_cond, &g_step_lock, &ts);
        }
        g_step_signal = 0;
        pthread_mutex_unlock(&g_step_lock);
#endif
    }
}

/* ========================================================================== */
/* Link Status                                                                */
/* ========================================================================== */

int sim_is_link_up(const char *core)
{
    if (strcmp(core, "master") == 0 && g_sim_state.master.initialized)
        return (int)rpmsg_lite_is_link_up(g_sim_state.master.instance);
    if (strcmp(core, "remote") == 0 && g_sim_state.remote.initialized)
        return (int)rpmsg_lite_is_link_up(g_sim_state.remote.instance);
    return 0;
}

/* ========================================================================== */
/* Buffer / VQ classification helpers (for state serialization)               */
/* ========================================================================== */

/* For a given vq+slot, classify what state the descriptor is in.
 * Returns one of: "free", "avail", "used".
 *
 * Definitions:
 *   "avail"  - this desc index appears in avail->ring[0 .. avail->idx-1] but
 *              not yet consumed (avail->idx > vq_available_idx for that pos),
 *              i.e. the OTHER side will see it as pending input.
 *   "used"   - this desc index appears in used->ring[0 .. used->idx-1] but
 *              not yet consumed locally (used->idx > vq_used_cons_idx).
 *   "free"   - everything else (including the producer-owned pool on the
 *              master TX side, which lives in the used ring waiting to be
 *              re-allocated). */
static const char *classify_desc(struct virtqueue *vq, uint16_t idx)
{
    if (!vq || !vq->vq_ring.avail || !vq->vq_ring.used) return "free";

    uint16_t n = vq->vq_nentries;

    /* Check the unconsumed-avail window */
    uint16_t a_from = vq->vq_available_idx;
    uint16_t a_to   = vq->vq_ring.avail->idx;
    uint16_t a_cnt  = (uint16_t)(a_to - a_from);
    for (uint16_t k = 0; k < a_cnt && k < n; k++)
    {
        uint16_t pos = (uint16_t)((a_from + k) & (n - 1));
        if (vq->vq_ring.avail->ring[pos] == idx) return "avail";
    }

    /* Check the unconsumed-used window */
    uint16_t u_from = vq->vq_used_cons_idx;
    uint16_t u_to   = vq->vq_ring.used->idx;
    uint16_t u_cnt  = (uint16_t)(u_to - u_from);
    for (uint16_t k = 0; k < u_cnt && k < n; k++)
    {
        uint16_t pos = (uint16_t)((u_from + k) & (n - 1));
        if (vq->vq_ring.used->ring[pos].id == idx) return "used";
    }

    return "free";
}

/* ========================================================================== */
/* State -> JSON                                                              */
/* ========================================================================== */

static int serialize_vq_json(struct virtqueue *vq, char *buf, int buf_size)
{
    if (!vq) return snprintf(buf, buf_size, "null");

    int len = snprintf(buf, buf_size,
                       "{\"name\":\"%s\",\"nentries\":%u,\"free_cnt\":%u,"
                       "\"desc_head_idx\":%u,\"avail_idx\":%u,\"used_cons_idx\":%u,"
                       "\"queued_cnt\":%u",
                       vq->vq_name,
                       vq->vq_nentries, vq->vq_free_cnt,
                       vq->vq_desc_head_idx, vq->vq_available_idx, vq->vq_used_cons_idx,
                       vq->vq_queued_cnt);

    /* desc[]: addr/len/flags/next + classification */
    len += snprintf(buf + len, buf_size - len, ",\"desc\":[");
    for (uint16_t i = 0; i < vq->vq_nentries && i < 32; i++)
    {
        if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
        struct vring_desc *d = &vq->vq_ring.desc[i];
        len += snprintf(buf + len, buf_size - len,
                        "{\"addr\":%llu,\"len\":%u,\"flags\":%u,\"next\":%u,\"state\":\"%s\"}",
                        (unsigned long long)d->addr, d->len, d->flags, d->next,
                        classify_desc(vq, i));
    }
    len += snprintf(buf + len, buf_size - len, "]");

    /* avail ring */
    len += snprintf(buf + len, buf_size - len,
                    ",\"avail\":{\"flags\":%u,\"idx\":%u,\"ring\":[",
                    vq->vq_ring.avail->flags, vq->vq_ring.avail->idx);
    for (uint16_t i = 0; i < vq->vq_nentries && i < 32; i++)
    {
        if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
        len += snprintf(buf + len, buf_size - len, "%u", vq->vq_ring.avail->ring[i]);
    }
    len += snprintf(buf + len, buf_size - len, "]}");

    /* used ring */
    len += snprintf(buf + len, buf_size - len,
                    ",\"used\":{\"flags\":%u,\"idx\":%u,\"ring\":[",
                    vq->vq_ring.used->flags, vq->vq_ring.used->idx);
    for (uint16_t i = 0; i < vq->vq_nentries && i < 32; i++)
    {
        if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
        len += snprintf(buf + len, buf_size - len,
                        "{\"id\":%u,\"len\":%u}",
                        vq->vq_ring.used->ring[i].id, vq->vq_ring.used->ring[i].len);
    }
    len += snprintf(buf + len, buf_size - len, "]}");

    len += snprintf(buf + len, buf_size - len, "}");
    return len;
}

static int serialize_core_json(sim_core_state_t *cs, const char *name, char *buf, int buf_size)
{
    int len = snprintf(buf, buf_size,
                       "\"%s\":{\"initialized\":%s,\"link_state\":%u,\"endpoints\":[",
                       name,
                       cs->initialized ? "true" : "false",
                       cs->initialized ? cs->instance->link_state : 0U);

    int first = 1;
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        if (cs->endpoints[i].active)
        {
            if (!first) len += snprintf(buf + len, buf_size - len, ",");
            len += snprintf(buf + len, buf_size - len, "{\"addr\":%u}", cs->endpoints[i].addr);
            first = 0;
        }
    }
    len += snprintf(buf + len, buf_size - len, "]");

    if (cs->initialized)
    {
        len += snprintf(buf + len, buf_size - len, ",\"rvq\":");
        len += serialize_vq_json(cs->instance->rvq, buf + len, buf_size - len);
        len += snprintf(buf + len, buf_size - len, ",\"tvq\":");
        len += serialize_vq_json(cs->instance->tvq, buf + len, buf_size - len);
    }

    len += snprintf(buf + len, buf_size - len, "}");
    return len;
}

/* Find which buffer pool slot a desc.addr belongs to, returns -1 if not in pool. */
static int buffer_slot_for_addr(uint64_t addr, uint8_t **out_ptr)
{
    uint64_t base = (uint64_t)RL_VRING_OVERHEAD;
    uint64_t buf_size = (uint64_t)RL_BUFFER_SIZE;
    uint64_t total = (uint64_t)RL_BUFFER_COUNT * 2ULL * buf_size;
    if (addr < base || addr >= base + total) return -1;
    uint64_t off = addr - base;
    if (off % buf_size != 0) return -1;
    int slot = (int)(off / buf_size);
    if (out_ptr) *out_ptr = g_sim_state.shmem + (size_t)addr;
    return slot;
}

/* Classify each buffer by walking both vrings.
 * Each buffer ends up labelled by where it currently "lives":
 *   - "master_tx_pool" : sitting in master_tvq.used (free pool waiting for vq_tx_alloc)
 *   - "remote_rx_pool" : sitting in remote_rvq.avail (the SAME shmem ring, mirror view)
 *   - "inflight_m2r"   : posted by master to its avail, not yet consumed by remote
 *   - "consumed_m2r"   : remote has consumed but master hasn't re-collected via used
 *   - similarly for remote -> master direction
 *   - "free"           : not currently referenced (shouldn't happen normally) */
static const char *classify_buffer_global(int slot)
{
    /* Buffers 0..N-1 belong to vring0 (master TX / remote RX),
     * Buffers N..2N-1 belong to vring1 (remote TX / master RX). */
    int N = (int)RL_BUFFER_COUNT;
    int local_idx = slot % N;
    int dir = slot / N; /* 0=m2r, 1=r2m */

    struct virtqueue *producer_vq = NULL;
    struct virtqueue *consumer_vq = NULL;

    if (dir == 0)
    {
        producer_vq = g_sim_state.master.initialized ? g_sim_state.master.instance->tvq : NULL;
        consumer_vq = g_sim_state.remote.initialized ? g_sim_state.remote.instance->rvq : NULL;
    }
    else
    {
        producer_vq = g_sim_state.remote.initialized ? g_sim_state.remote.instance->tvq : NULL;
        consumer_vq = g_sim_state.master.initialized ? g_sim_state.master.instance->rvq : NULL;
    }

    /* Use producer view as source of truth for desc classification */
    const char *cls = producer_vq ? classify_desc(producer_vq, (uint16_t)local_idx) : "free";

    if (dir == 0)
    {
        if (strcmp(cls, "used") == 0)  return "m_tx_pool";   /* master holds it */
        if (strcmp(cls, "avail") == 0) return "inflight_m2r";
    }
    else
    {
        if (strcmp(cls, "used") == 0)  return "r_tx_pool";
        if (strcmp(cls, "avail") == 0) return "inflight_r2m";
    }
    return "free";
}

int sim_state_to_json(char *buf, int buf_size)
{
    int len = snprintf(buf, buf_size, "{");

    len += serialize_core_json(&g_sim_state.master, "master", buf + len, buf_size - len);
    len += snprintf(buf + len, buf_size - len, ",");
    len += serialize_core_json(&g_sim_state.remote, "remote", buf + len, buf_size - len);

    const char *mode_str = "normal";
    if (g_sim_state.mode == SIM_MODE_SLOW) mode_str = "slow";
    else if (g_sim_state.mode == SIM_MODE_STEP) mode_str = "step";

    len += snprintf(buf + len, buf_size - len,
                    ",\"mode\":\"%s\",\"delay_ms\":%u,\"step_pending\":%s,"
                    "\"shmem_size\":%u,\"buffer_count\":%u,\"buffer_payload_size\":%u,"
                    "\"vring_overhead\":%u,\"buffer_size\":%u",
                    mode_str, g_sim_state.delay_ms,
                    g_sim_state.step_pending ? "true" : "false",
                    g_sim_state.shmem_size,
                    (unsigned)RL_BUFFER_COUNT,
                    (unsigned)RL_BUFFER_PAYLOAD_SIZE,
                    (unsigned)RL_VRING_OVERHEAD,
                    (unsigned)RL_BUFFER_SIZE);

    /* Buffer pool dump */
    len += snprintf(buf + len, buf_size - len, ",\"buffers\":[");
    if (g_sim_state.master.initialized || g_sim_state.remote.initialized)
    {
        uint8_t *buf_base = g_sim_state.shmem + RL_VRING_OVERHEAD;
        uint32_t total_bufs = RL_BUFFER_COUNT * 2;
        for (uint32_t i = 0; i < total_bufs && (len + 300) < buf_size; i++)
        {
            uint8_t *b = buf_base + i * RL_BUFFER_SIZE;
            if (i > 0) len += snprintf(buf + len, buf_size - len, ",");

            struct rpmsg_std_msg *msg = (struct rpmsg_std_msg *)b;
            uint16_t msg_len = msg->hdr.len;
            int valid_payload = (msg_len > 0 && msg_len <= RL_BUFFER_PAYLOAD_SIZE);

            len += snprintf(buf + len, buf_size - len,
                            "{\"idx\":%u,\"dir\":\"%s\",\"state\":\"%s\","
                            "\"src\":%u,\"dst\":%u,\"len\":%u,\"flags\":%u,\"data\":\"",
                            i,
                            (i < RL_BUFFER_COUNT) ? "m2r" : "r2m",
                            classify_buffer_global((int)i),
                            msg->hdr.src, msg->hdr.dst,
                            valid_payload ? msg_len : 0,
                            msg->hdr.flags);

            if (valid_payload)
            {
                uint32_t show_len = msg_len;
                if (show_len > 96) show_len = 96; /* cap to avoid huge JSON */
                for (uint32_t j = 0; j < show_len && (len + 4) < buf_size; j++)
                    len += snprintf(buf + len, buf_size - len, "%02x", msg->data[j]);
            }
            len += snprintf(buf + len, buf_size - len, "\"}");
        }
    }
    len += snprintf(buf + len, buf_size - len, "]");

    len += snprintf(buf + len, buf_size - len, "}");
    return len;
}
