/*
 * RPMsg-Lite Simulator - Core Logic Implementation
 * Manages the simulation of Master and Remote rpmsg instances
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
#endif

/* Global simulation state */
static sim_state_t g_sim_state;

sim_state_t *sim_get_state(void)
{
    return &g_sim_state;
}

/* RX callback for simulation - receives into a simple buffer for web display */
static int32_t sim_rx_callback(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    /* Record the receive event */
    uint32_t dst_addr = 0;
    const char *core = "unknown";

    /* Determine which core this belongs to */
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        if (g_sim_state.master.endpoints[i].active &&
            g_sim_state.master.endpoints[i].ept &&
            g_sim_state.master.endpoints[i].ept->rx_cb_data == priv)
        {
            dst_addr = g_sim_state.master.endpoints[i].addr;
            core = "master";
            break;
        }
        if (g_sim_state.remote.endpoints[i].active &&
            g_sim_state.remote.endpoints[i].ept &&
            g_sim_state.remote.endpoints[i].ept->rx_cb_data == priv)
        {
            dst_addr = g_sim_state.remote.endpoints[i].addr;
            core = "remote";
            break;
        }
    }

    sim_event_record_msg(SIM_EVT_RX_CALLBACK, core, "rx",
                         src, dst_addr, payload_len,
                         (const uint8_t *)payload, payload_len);

    return RL_RELEASE; /* Release buffer immediately */
}

/* ========================================================================== */
/* Simulation Init/Reset                                                       */
/* ========================================================================== */

int sim_init(void)
{
    memset(&g_sim_state, 0, sizeof(g_sim_state));
    g_sim_state.mode = SIM_MODE_NORMAL;
    g_sim_state.delay_ms = 500;
    g_sim_state.running = 1;

    /* Allocate shared memory */
    g_sim_state.shmem_size = SIM_SHMEM_SIZE;
    g_sim_state.shmem = (uint8_t *)malloc(g_sim_state.shmem_size);
    if (!g_sim_state.shmem)
    {
        fprintf(stderr, "Failed to allocate shared memory (%u bytes)\n", g_sim_state.shmem_size);
        return -1;
    }
    memset(g_sim_state.shmem, 0, g_sim_state.shmem_size);

    /* Set up the environment globals */
    g_sim_shmem_base = g_sim_state.shmem;
    g_sim_shmem_size = g_sim_state.shmem_size;
    sim_platform_set_shmem(g_sim_state.shmem, g_sim_state.shmem_size);

    /* Initialize event system */
    sim_events_init();

    printf("[SIM] Initialized. Shared memory: %u bytes at %p\n",
           g_sim_state.shmem_size, (void *)g_sim_state.shmem);

    return 0;
}

void sim_reset(void)
{
    /* Deinit both cores if initialized */
    if (g_sim_state.master.initialized)
        sim_deinit_core("master");
    if (g_sim_state.remote.initialized)
        sim_deinit_core("remote");

    /* Clear shared memory */
    if (g_sim_state.shmem)
    {
        memset(g_sim_state.shmem, 0, g_sim_state.shmem_size);
    }

    g_sim_link_up_signal = 0;
    sim_events_init();

    printf("[SIM] Reset complete\n");
}

/* ========================================================================== */
/* Master/Remote Init                                                           */
/* ========================================================================== */

int sim_init_master(void)
{
    if (g_sim_state.master.initialized)
    {
        fprintf(stderr, "[SIM] Master already initialized\n");
        return -1;
    }

    sim_apply_delay();
    sim_event_record(SIM_EVT_INIT_MASTER, "master", NULL);

    env_init();

    g_sim_state.master.instance = rpmsg_lite_master_init(
        g_sim_state.shmem,
        g_sim_state.shmem_size,
        RL_PLATFORM_SIM_LINK_ID,
        RL_NO_FLAGS,
        &g_sim_state.master.instance_static);

    if (g_sim_state.master.instance == RL_NULL)
    {
        fprintf(stderr, "[SIM] Master init failed\n");
        return -1;
    }

    g_sim_state.master.initialized = 1;
    printf("[SIM] Master initialized, link_state=%u\n",
           g_sim_state.master.instance->link_state);

    return 0;
}

int sim_init_remote(void)
{
    if (g_sim_state.remote.initialized)
    {
        fprintf(stderr, "[SIM] Remote already initialized\n");
        return -1;
    }

    if (!g_sim_state.master.initialized)
    {
        fprintf(stderr, "[SIM] Master must be initialized first\n");
        return -1;
    }

    sim_apply_delay();
    sim_event_record(SIM_EVT_INIT_REMOTE, "remote", NULL);

    g_sim_state.remote.instance = rpmsg_lite_remote_init(
        g_sim_state.shmem,
        RL_PLATFORM_SIM_LINK_ID,
        RL_NO_FLAGS,
        &g_sim_state.remote.instance_static);

    if (g_sim_state.remote.instance == RL_NULL)
    {
        fprintf(stderr, "[SIM] Remote init failed\n");
        return -1;
    }

    g_sim_state.remote.initialized = 1;
    printf("[SIM] Remote initialized, link_state=%u\n",
           g_sim_state.remote.instance->link_state);

    return 0;
}

/* ========================================================================== */
/* Deinit                                                                      */
/* ========================================================================== */

int sim_deinit_core(const char *core)
{
    sim_core_state_t *cs = NULL;

    if (strcmp(core, "master") == 0)
        cs = &g_sim_state.master;
    else if (strcmp(core, "remote") == 0)
        cs = &g_sim_state.remote;
    else
        return -1;

    if (!cs->initialized)
        return -1;

    sim_apply_delay();
    sim_event_record(SIM_EVT_DEINIT, core, NULL);

    /* Destroy all endpoints first */
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
/* Endpoints                                                                   */
/* ========================================================================== */

int sim_create_ept(const char *core, uint32_t addr)
{
    sim_core_state_t *cs = NULL;

    if (strcmp(core, "master") == 0)
        cs = &g_sim_state.master;
    else if (strcmp(core, "remote") == 0)
        cs = &g_sim_state.remote;
    else
        return -1;

    if (!cs->initialized)
        return -1;

    if (cs->ept_count >= SIM_MAX_ENDPOINTS)
        return -1;

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        if (!cs->endpoints[i].active)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    sim_apply_delay();

    /* Use the endpoint's own address as the priv pointer for identification in callback */
    cs->endpoints[slot].ept = rpmsg_lite_create_ept(
        cs->instance,
        addr,
        sim_rx_callback,
        &cs->endpoints[slot],  /* priv = pointer to this slot for identification */
        &cs->endpoints[slot].ept_ctx);

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

    if (strcmp(core, "master") == 0)
        cs = &g_sim_state.master;
    else if (strcmp(core, "remote") == 0)
        cs = &g_sim_state.remote;
    else
        return -1;

    if (!cs->initialized)
        return -1;

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
/* Send Message                                                                */
/* ========================================================================== */

int sim_send(const char *core, uint32_t src, uint32_t dst,
             const uint8_t *data, uint32_t len)
{
    sim_core_state_t *cs = NULL;

    if (strcmp(core, "master") == 0)
        cs = &g_sim_state.master;
    else if (strcmp(core, "remote") == 0)
        cs = &g_sim_state.remote;
    else
        return -1;

    if (!cs->initialized)
        return -1;

    /* Find the source endpoint */
    struct rpmsg_lite_endpoint *ept = NULL;
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        if (cs->endpoints[i].active && cs->endpoints[i].addr == src)
        {
            ept = cs->endpoints[i].ept;
            break;
        }
    }

    if (!ept)
    {
        fprintf(stderr, "[SIM] Source endpoint %u not found on %s\n", src, core);
        return -1;
    }

    sim_apply_delay();
    sim_event_record_msg(SIM_EVT_TX_SEND, core, "tx", src, dst, len, data, len);

    int32_t ret = rpmsg_lite_send(cs->instance, ept, dst, (char *)data, len, RL_DONT_BLOCK);

    if (ret != RL_SUCCESS)
    {
        fprintf(stderr, "[SIM] Send failed on %s: %d\n", core, (int)ret);
        return (int)ret;
    }

    printf("[SIM] Sent %u bytes from %s:%u to %u\n", len, core, src, dst);
    return 0;
}

/* ========================================================================== */
/* Mode Control                                                                */
/* ========================================================================== */

void sim_set_mode(sim_mode_t mode, uint32_t delay_ms)
{
    g_sim_state.mode = mode;
    if (delay_ms > 0)
        g_sim_state.delay_ms = delay_ms;
    printf("[SIM] Mode set to %d, delay=%u ms\n", (int)mode, g_sim_state.delay_ms);
}

void sim_step(void)
{
    g_sim_state.step_pending = 0;
}

void sim_apply_delay(void)
{
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
        /* Wait for step command */
        while (g_sim_state.step_pending && g_sim_state.running)
        {
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
        }
    }
}

/* ========================================================================== */
/* Link Status                                                                 */
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
/* State Serialization to JSON                                                 */
/* ========================================================================== */

static int serialize_vq_json(struct virtqueue *vq, char *buf, int buf_size)
{
    if (!vq)
        return snprintf(buf, buf_size, "null");

    int len = snprintf(buf, buf_size,
                       "{\"name\":\"%s\",\"nentries\":%u,\"free_cnt\":%u,"
                       "\"desc_head_idx\":%u,\"avail_idx\":%u,\"used_cons_idx\":%u,"
                       "\"queued_cnt\":%u",
                       vq->vq_name,
                       vq->vq_nentries, vq->vq_free_cnt,
                       vq->vq_desc_head_idx, vq->vq_available_idx, vq->vq_used_cons_idx,
                       vq->vq_queued_cnt);

    /* Serialize descriptor ring */
    len += snprintf(buf + len, buf_size - len, ",\"desc\":[");
    for (uint16_t i = 0; i < vq->vq_nentries && i < 16; i++)
    {
        if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
        struct vring_desc *d = &vq->vq_ring.desc[i];
        len += snprintf(buf + len, buf_size - len,
                        "{\"addr\":%llu,\"len\":%u,\"flags\":%u,\"next\":%u}",
                        (unsigned long long)d->addr, d->len, d->flags, d->next);
    }
    len += snprintf(buf + len, buf_size - len, "]");

    /* Serialize avail ring */
    len += snprintf(buf + len, buf_size - len,
                    ",\"avail\":{\"flags\":%u,\"idx\":%u,\"ring\":[",
                    vq->vq_ring.avail->flags, vq->vq_ring.avail->idx);
    for (uint16_t i = 0; i < vq->vq_nentries && i < 16; i++)
    {
        if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
        len += snprintf(buf + len, buf_size - len, "%u", vq->vq_ring.avail->ring[i]);
    }
    len += snprintf(buf + len, buf_size - len, "]}");

    /* Serialize used ring */
    len += snprintf(buf + len, buf_size - len,
                    ",\"used\":{\"flags\":%u,\"idx\":%u,\"ring\":[",
                    vq->vq_ring.used->flags, vq->vq_ring.used->idx);
    for (uint16_t i = 0; i < vq->vq_nentries && i < 16; i++)
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
                       "\"%s\":{\"initialized\":%s,\"link_state\":%u,"
                       "\"endpoints\":[",
                       name,
                       cs->initialized ? "true" : "false",
                       cs->initialized ? cs->instance->link_state : 0U);

    int first = 1;
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        if (cs->endpoints[i].active)
        {
            if (!first) len += snprintf(buf + len, buf_size - len, ",");
            len += snprintf(buf + len, buf_size - len, "{\"addr\":%u}",
                            cs->endpoints[i].addr);
            first = 0;
        }
    }
    len += snprintf(buf + len, buf_size - len, "]");

    /* VirtQueues */
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

int sim_state_to_json(char *buf, int buf_size)
{
    int len = snprintf(buf, buf_size, "{");

    /* Master */
    len += serialize_core_json(&g_sim_state.master, "master", buf + len, buf_size - len);
    len += snprintf(buf + len, buf_size - len, ",");

    /* Remote */
    len += serialize_core_json(&g_sim_state.remote, "remote", buf + len, buf_size - len);

    /* Mode info */
    const char *mode_str = "normal";
    if (g_sim_state.mode == SIM_MODE_SLOW) mode_str = "slow";
    else if (g_sim_state.mode == SIM_MODE_STEP) mode_str = "step";

    len += snprintf(buf + len, buf_size - len,
                    ",\"mode\":\"%s\",\"delay_ms\":%u,\"step_pending\":%s,"
                    "\"shmem_size\":%u,\"buffer_count\":%u,\"buffer_payload_size\":%u",
                    mode_str, g_sim_state.delay_ms,
                    g_sim_state.step_pending ? "true" : "false",
                    g_sim_state.shmem_size,
                    (unsigned)RL_BUFFER_COUNT,
                    (unsigned)RL_BUFFER_PAYLOAD_SIZE);

    /* Serialize buffer contents (first N bytes of each buffer as hex) */
    if (g_sim_state.master.initialized)
    {
        len += snprintf(buf + len, buf_size - len, ",\"buffers\":[");
        /* Buffers are located after the vring overhead */
        uint8_t *buf_base = g_sim_state.shmem + RL_VRING_OVERHEAD;
        for (uint32_t i = 0; i < RL_BUFFER_COUNT * 2 && (len + 200) < buf_size; i++)
        {
            uint8_t *b = buf_base + i * RL_BUFFER_SIZE;
            if (i > 0) len += snprintf(buf + len, buf_size - len, ",");

            /* Parse as rpmsg_std_msg */
            struct rpmsg_std_msg *msg = (struct rpmsg_std_msg *)b;
            len += snprintf(buf + len, buf_size - len,
                            "{\"idx\":%u,\"src\":%u,\"dst\":%u,\"len\":%u,\"flags\":%u,\"data\":\"",
                            i, msg->hdr.src, msg->hdr.dst, msg->hdr.len, msg->hdr.flags);

            /* First 32 bytes of payload as hex */
            uint32_t show_len = msg->hdr.len > 32 ? 32 : msg->hdr.len;
            if (show_len > RL_BUFFER_PAYLOAD_SIZE) show_len = 0;
            for (uint32_t j = 0; j < show_len; j++)
            {
                len += snprintf(buf + len, buf_size - len, "%02x", msg->data[j]);
            }
            len += snprintf(buf + len, buf_size - len, "\"}");
        }
        len += snprintf(buf + len, buf_size - len, "]");
    }

    len += snprintf(buf + len, buf_size - len, "}");
    return len;
}
