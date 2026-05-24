/*
 * sim_main.c  —  RPMsg-Lite PC simulator entry point
 *
 * Runs Master and Remote each in their own thread, sharing a malloc'd
 * buffer as "shared memory".  All state changes appear in the trace ring
 * and are printed to stdout (Phase 1).  The GUI layer will consume the
 * same ring in Phase 2.
 *
 *  Thread layout:
 *   main()
 *    ├─ ipc_sim_init()
 *    ├─ master_thread   →  rpmsg_lite_master_init → create_ept(50) → send loop
 *    └─ remote_thread   →  rpmsg_lite_remote_init → wait_link_up → create_ept(51) → recv loop
 *
 *  ISR dispatch threads are started inside platform_init() (see rpmsg_platform_sim.c).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "rpmsg_platform_sim.h"
#include "ipc_sim.h"
#include "rpmsg_trace.h"

/* Declared in rpmsg_env_sim.c — must be called before any rpmsg init */
void sim_set_shmem_base(void *base);
/* ---- Shared memory ------------------------------------------------------ */

#define SHMEM_SIZE   (0x8000u)   /* 32 KB — enough for 2×vring + 8 buffers */
#define LINK_ID      (0u)        /* RL_PLATFORM_LPC55S69_M33_M33_LINK_ID */

static uint8_t g_shmem[SHMEM_SIZE];

/* ---- Endpoint addresses ------------------------------------------------- */

#define MASTER_EPT_ADDR  (50u)
#define REMOTE_EPT_ADDR  (51u)

/* ---- Shared state between threads --------------------------------------- */

static volatile int  g_stop            = 0;
static volatile int  g_ping_count      = 0;
static volatile int  g_master_init_done = 0; /* Set when master_init completes */
#define MAX_PINGS 10

/* ---- Remote RX callback ------------------------------------------------- */

static int32_t remote_rx_cb(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    (void)priv;
    char buf[128] = {0};
    uint32_t copy_len = payload_len < 127 ? payload_len : 127;
    memcpy(buf, payload, copy_len);

    printf("[Remote] recv from %u: \"%s\" (%u bytes)\n", src, buf, payload_len);

    /* Trace RX event */
    rpmsg_event_t evt = {0};
    evt.type        = RPMSG_EVT_RX_CALLBACK;
    evt.side        = SIM_SIDE_REMOTE;
    evt.data.msg.src = src;
    evt.data.msg.dst = REMOTE_EPT_ADDR;
    evt.data.msg.len = (uint16_t)payload_len;
    memcpy(evt.data.msg.preview, payload, copy_len);
    rpmsg_trace_emit(&evt);

    return RL_RELEASE;
}

/* ---- Master RX callback ------------------------------------------------- */

static int32_t master_rx_cb(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    (void)priv;
    char buf[128] = {0};
    uint32_t copy_len = payload_len < 127 ? payload_len : 127;
    memcpy(buf, payload, copy_len);

    printf("[Master] recv from %u: \"%s\" (%u bytes)\n", src, buf, payload_len);

    rpmsg_event_t evt = {0};
    evt.type         = RPMSG_EVT_RX_CALLBACK;
    evt.side         = SIM_SIDE_MASTER;
    evt.data.msg.src = src;
    evt.data.msg.dst = MASTER_EPT_ADDR;
    evt.data.msg.len = (uint16_t)payload_len;
    memcpy(evt.data.msg.preview, payload, copy_len);
    rpmsg_trace_emit(&evt);

    g_ping_count++;
    return RL_RELEASE;
}

/* ---- Remote thread ------------------------------------------------------ */

static DWORD WINAPI remote_thread(LPVOID arg)
{
    (void)arg;
    platform_set_current_side(SIM_SIDE_REMOTE);
    /* Wait until master_init completes — remote MUST NOT zero vrings before master fills them */
    while (!g_master_init_done) Sleep(1);
    printf("[Remote] starting remote_init...\n");
    rpmsg_trace_emit_simple(RPMSG_EVT_REMOTE_INIT_START, LINK_ID);

    struct rpmsg_lite_instance *rdev =
        rpmsg_lite_remote_init((void *)g_shmem, LINK_ID, RL_NO_FLAGS);
    if (!rdev) {
        printf("[Remote] remote_init FAILED\n");
        rpmsg_trace_emit_simple(RPMSG_EVT_ERROR, 0);
        return 1;
    }
    ipc_sim_register_side(SIM_SIDE_REMOTE, rdev);
    rpmsg_trace_emit_simple(RPMSG_EVT_REMOTE_INIT_DONE, LINK_ID);
    printf("[Remote] remote_init OK, waiting for link...\n");

    rpmsg_lite_wait_for_link_up(rdev, RL_BLOCK);
    rpmsg_trace_emit_simple(RPMSG_EVT_LINK_UP, LINK_ID);
    printf("[Remote] link UP\n");

    struct rpmsg_lite_endpoint *ept =
        rpmsg_lite_create_ept(rdev, REMOTE_EPT_ADDR, remote_rx_cb, NULL);
    rpmsg_trace_emit_simple(RPMSG_EVT_EPT_CREATED, REMOTE_EPT_ADDR);
    printf("[Remote] endpoint %u created\n", REMOTE_EPT_ADDR);

    /* Keep alive — reply handled in RX callback of master; remote just replies */
    while (!g_stop) {
        Sleep(50);
        /* Echo back whatever we received (simple: send fixed reply) */
        const char *reply = "pong";
        rpmsg_event_t te = {0};
        te.type         = RPMSG_EVT_TX_START;
        te.side         = SIM_SIDE_REMOTE;
        te.data.msg.src = REMOTE_EPT_ADDR;
        te.data.msg.dst = MASTER_EPT_ADDR;
        te.data.msg.len = (uint16_t)(strlen(reply) + 1);
        memcpy(te.data.msg.preview, reply, te.data.msg.len);
        rpmsg_trace_emit(&te);

        (void)rpmsg_lite_send(rdev, ept, MASTER_EPT_ADDR,
                              (void *)reply, (uint32_t)strlen(reply) + 1, RL_DONT_BLOCK);

        rpmsg_trace_emit_simple(RPMSG_EVT_TX_DONE, REMOTE_EPT_ADDR);
    }

    rpmsg_lite_destroy_ept(rdev, ept);
    rpmsg_trace_emit_simple(RPMSG_EVT_EPT_DESTROYED, REMOTE_EPT_ADDR);

    rpmsg_trace_emit_simple(RPMSG_EVT_DEINIT_START, LINK_ID);
    rpmsg_lite_deinit(rdev);
    rpmsg_trace_emit_simple(RPMSG_EVT_DEINIT_DONE, LINK_ID);
    printf("[Remote] deinit done\n");
    return 0;
}

/* ---- Master thread ------------------------------------------------------ */

static DWORD WINAPI master_thread(LPVOID arg)
{
    (void)arg;
    platform_set_current_side(SIM_SIDE_MASTER);
    printf("[Master] starting master_init...\n");
    rpmsg_trace_emit_simple(RPMSG_EVT_MASTER_INIT_START, LINK_ID);

    struct rpmsg_lite_instance *mdev =
        rpmsg_lite_master_init((void *)g_shmem, SHMEM_SIZE, LINK_ID, RL_NO_FLAGS);
    if (!mdev) {
        printf("[Master] master_init FAILED\n");
        rpmsg_trace_emit_simple(RPMSG_EVT_ERROR, 0);
        return 1;
    }
    ipc_sim_register_side(SIM_SIDE_MASTER, mdev);
    rpmsg_trace_emit_simple(RPMSG_EVT_MASTER_INIT_DONE, LINK_ID);
    printf("[Master] master_init OK\n");
    g_master_init_done = 1; /* Signal remote that shared memory is ready */

    rpmsg_lite_wait_for_link_up(mdev, RL_BLOCK);
    rpmsg_trace_emit_simple(RPMSG_EVT_LINK_UP, LINK_ID);
    printf("[Master] link UP\n");

    struct rpmsg_lite_endpoint *ept =
        rpmsg_lite_create_ept(mdev, MASTER_EPT_ADDR, master_rx_cb, NULL);
    rpmsg_trace_emit_simple(RPMSG_EVT_EPT_CREATED, MASTER_EPT_ADDR);
    printf("[Master] endpoint %u created\n", MASTER_EPT_ADDR);

    /* Send MAX_PINGS ping messages */
    for (int i = 0; i < MAX_PINGS && !g_stop; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "ping #%d", i);

        rpmsg_event_t te = {0};
        te.type         = RPMSG_EVT_TX_START;
        te.side         = SIM_SIDE_MASTER;
        te.data.msg.src = MASTER_EPT_ADDR;
        te.data.msg.dst = REMOTE_EPT_ADDR;
        te.data.msg.len = (uint16_t)(strlen(msg) + 1);
        memcpy(te.data.msg.preview, msg, te.data.msg.len);
        rpmsg_trace_emit(&te);

        printf("[Master] send: \"%s\"\n", msg);
        int32_t rc = rpmsg_lite_send(mdev, ept, REMOTE_EPT_ADDR,
                                     msg, (uint32_t)strlen(msg) + 1, 1000);
        rpmsg_trace_emit_simple(RPMSG_EVT_TX_DONE, (uint32_t)rc);

        Sleep(200); /* give Remote time to reply */
    }

    g_stop = 1;

    rpmsg_lite_destroy_ept(mdev, ept);
    rpmsg_trace_emit_simple(RPMSG_EVT_EPT_DESTROYED, MASTER_EPT_ADDR);

    rpmsg_trace_emit_simple(RPMSG_EVT_DEINIT_START, LINK_ID);
    rpmsg_lite_deinit(mdev);
    rpmsg_trace_emit_simple(RPMSG_EVT_DEINIT_DONE, LINK_ID);
    printf("[Master] deinit done\n");
    return 0;
}

/* ---- Event logger (Phase-1 console) ------------------------------------- */

static const char *evt_name(rpmsg_event_type_t t)
{
    switch (t) {
    case RPMSG_EVT_MASTER_INIT_START:  return "MASTER_INIT_START";
    case RPMSG_EVT_MASTER_INIT_DONE:   return "MASTER_INIT_DONE";
    case RPMSG_EVT_REMOTE_INIT_START:  return "REMOTE_INIT_START";
    case RPMSG_EVT_REMOTE_INIT_DONE:   return "REMOTE_INIT_DONE";
    case RPMSG_EVT_DEINIT_START:       return "DEINIT_START";
    case RPMSG_EVT_DEINIT_DONE:        return "DEINIT_DONE";
    case RPMSG_EVT_LINK_UP:            return "LINK_UP";
    case RPMSG_EVT_LINK_DOWN:          return "LINK_DOWN";
    case RPMSG_EVT_EPT_CREATED:        return "EPT_CREATED";
    case RPMSG_EVT_EPT_DESTROYED:      return "EPT_DESTROYED";
    case RPMSG_EVT_TX_START:           return "TX_START";
    case RPMSG_EVT_TX_DONE:            return "TX_DONE";
    case RPMSG_EVT_RX_CALLBACK:        return "RX_CALLBACK";
    case RPMSG_EVT_RX_BUFFER_RELEASED: return "RX_BUFFER_RELEASED";
    case RPMSG_EVT_NS_ANNOUNCE:        return "NS_ANNOUNCE";
    case RPMSG_EVT_NS_DISCOVER:        return "NS_DISCOVER";
    case RPMSG_EVT_VRING_KICK:         return "VRING_KICK";
    case RPMSG_EVT_VRING_NOTIFY:       return "VRING_NOTIFY";
    case RPMSG_EVT_BUFFER_ALLOC:       return "BUFFER_ALLOC";
    case RPMSG_EVT_BUFFER_FREE:        return "BUFFER_FREE";
    case RPMSG_EVT_ERROR:              return "ERROR";
    default:                           return "UNKNOWN";
    }
}

static void drain_trace_log(void)
{
    rpmsg_event_t e;
    while (rpmsg_trace_poll(&e)) {
        const char *side = (e.side == SIM_SIDE_MASTER) ? "M" : "R";
        printf("[TRACE %s %6llums] %-24s extra=0x%x",
               side, (unsigned long long)e.timestamp_ms, evt_name(e.type), e.extra);
        if (e.type == RPMSG_EVT_TX_START || e.type == RPMSG_EVT_RX_CALLBACK) {
            printf("  src=%u dst=%u len=%u  payload=\"%.32s\"",
                   e.data.msg.src, e.data.msg.dst, e.data.msg.len,
                   (char *)e.data.msg.preview);
        }
        printf("\n");
    }
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered so we see all output */
    printf("=== RPMsg-Lite PC Simulator (Phase 1 --- Console) ===\n");
    rpmsg_trace_init();

    /* Clear shared memory */
    memset(g_shmem, 0, sizeof(g_shmem));

    /* Tell env_map_vatopa/patova where our shmem lives (needed on 64-bit) */
    sim_set_shmem_base(g_shmem);

    /* Start ISR dispatcher threads inside platform_init via env_init (called
     * automatically by rpmsg_lite_master/remote_init → env_init).            */

    HANDLE hMaster = CreateThread(NULL, 0, master_thread, NULL, 0, NULL);
    Sleep(10); /* small head-start so master sets up vring before remote */
    HANDLE hRemote = CreateThread(NULL, 0, remote_thread, NULL, 0, NULL);

    /* Poll events and wait for both threads to finish */
    while (WaitForSingleObject(hMaster, 50) != WAIT_OBJECT_0 ||
           WaitForSingleObject(hRemote, 0)  != WAIT_OBJECT_0) {
        drain_trace_log();
    }
    drain_trace_log(); /* flush remaining events */

    CloseHandle(hMaster);
    CloseHandle(hRemote);

    printf("\n=== Simulation finished. %d pings exchanged ===\n", g_ping_count);
    return 0;
}
