/*
 * RPMsg-Lite Simulator - Main Entry Point
 *
 * Threading model
 * ---------------
 *   Main thread : runs mongoose poll loop. Receives WebSocket commands,
 *                 enqueues blocking ones onto a single-slot job queue, and
 *                 broadcasts events + state snapshots to all clients.
 *   Worker thread: drains the job queue and executes blocking sim calls
 *                 (init_master/remote, send, ept create/destroy, deinit,
 *                  reset). Those calls may call sim_apply_delay() which
 *                  blocks for slow-motion sleep or waits on the step
 *                  signal — the main thread is never blocked, so the UI
 *                  stays responsive and the user can press "Step".
 *
 * All JSON parsing is hand-rolled (no external lib) — fields used are simple.
 */

#include "mongoose.h"

/* Undefine Windows MEM_MAPPED before rpmsg headers define it */
#ifdef MEM_MAPPED
#undef MEM_MAPPED
#endif

#include "sim_core.h"
#include "sim_events.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static const char *s_listen_url = "http://0.0.0.0:8088";
static volatile int s_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    s_running = 0;
    sim_get_state()->running = 0;
    sim_step(); /* unblock any step-mode waiter */
}

/* ========================================================================== */
/* Minimal JSON helpers                                                        */
/* ========================================================================== */

static const char *json_get_str(const char *json, const char *key, char *out, int out_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    const char *end = p;
    while (*end && *end != '"') end++;
    if (!*end) return NULL;
    int len = (int)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

static int json_get_int(const char *json, const char *key, int *out)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    *out = atoi(p);
    return 1;
}

/* ========================================================================== */
/* Base64 decoder                                                              */
/* ========================================================================== */

static int b64_decode(const char *in, uint8_t *out, int out_max)
{
    static int8_t T[256];
    static int initialized = 0;
    if (!initialized) {
        for (int i = 0; i < 256; i++) T[i] = -1;
        const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[(unsigned char)alpha[i]] = (int8_t)i;
        T['='] = -2;
        initialized = 1;
    }

    int out_len = 0;
    int val = 0, bits = 0;
    for (const char *p = in; *p; p++) {
        int8_t c = T[(unsigned char)*p];
        if (c == -2) break;
        if (c < 0) continue;
        val = (val << 6) | c;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len < out_max) out[out_len] = (uint8_t)((val >> bits) & 0xFF);
            out_len++;
        }
    }
    return out_len;
}

/* ========================================================================== */
/* Job queue (single-slot, command thread drains it)                           */
/* ========================================================================== */

typedef struct sim_job
{
    char *json;
    size_t len;
    struct sim_job *next;
} sim_job_t;

static sim_job_t *g_jobs_head = NULL;
static sim_job_t *g_jobs_tail = NULL;
#ifdef _WIN32
static CRITICAL_SECTION g_jobs_lock;
static HANDLE g_jobs_event;
#else
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_jobs_cond = PTHREAD_COND_INITIALIZER;
#endif

static void jobs_init(void)
{
#ifdef _WIN32
    InitializeCriticalSection(&g_jobs_lock);
    g_jobs_event = CreateEvent(NULL, FALSE, FALSE, NULL);
#endif
}

static void jobs_push(const char *json, size_t len)
{
    sim_job_t *j = (sim_job_t *)malloc(sizeof(sim_job_t));
    if (!j) return;
    j->json = (char *)malloc(len + 1);
    if (!j->json) { free(j); return; }
    memcpy(j->json, json, len);
    j->json[len] = '\0';
    j->len = len;
    j->next = NULL;

#ifdef _WIN32
    EnterCriticalSection(&g_jobs_lock);
#else
    pthread_mutex_lock(&g_jobs_lock);
#endif
    if (g_jobs_tail) g_jobs_tail->next = j; else g_jobs_head = j;
    g_jobs_tail = j;
#ifdef _WIN32
    SetEvent(g_jobs_event);
    LeaveCriticalSection(&g_jobs_lock);
#else
    pthread_cond_signal(&g_jobs_cond);
    pthread_mutex_unlock(&g_jobs_lock);
#endif
}

static sim_job_t *jobs_pop_blocking(void)
{
    sim_job_t *j = NULL;
#ifdef _WIN32
    while (s_running)
    {
        EnterCriticalSection(&g_jobs_lock);
        if (g_jobs_head)
        {
            j = g_jobs_head;
            g_jobs_head = j->next;
            if (!g_jobs_head) g_jobs_tail = NULL;
            LeaveCriticalSection(&g_jobs_lock);
            return j;
        }
        LeaveCriticalSection(&g_jobs_lock);
        WaitForSingleObject(g_jobs_event, 200);
    }
#else
    pthread_mutex_lock(&g_jobs_lock);
    while (s_running && !g_jobs_head)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_jobs_cond, &g_jobs_lock, &ts);
    }
    if (g_jobs_head)
    {
        j = g_jobs_head;
        g_jobs_head = j->next;
        if (!g_jobs_head) g_jobs_tail = NULL;
    }
    pthread_mutex_unlock(&g_jobs_lock);
#endif
    return j;
}

/* ========================================================================== */
/* Command execution (runs on worker thread)                                   */
/* ========================================================================== */

static void execute_command(const char *json)
{
    char cmd[32] = {0};
    char core[16] = {0};
    char mode_str[16] = {0};
    char b64_data[4096] = {0};
    int addr = 0, src_addr = 0, dst_addr = 0, delay = 0;

    json_get_str(json, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "init_master") == 0)            sim_init_master();
    else if (strcmp(cmd, "init_remote") == 0)       sim_init_remote();
    else if (strcmp(cmd, "create_ept") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        json_get_int(json, "addr", &addr);
        sim_create_ept(core, (uint32_t)addr);
    }
    else if (strcmp(cmd, "destroy_ept") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        json_get_int(json, "addr", &addr);
        sim_destroy_ept(core, (uint32_t)addr);
    }
    else if (strcmp(cmd, "send") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        json_get_int(json, "src", &src_addr);
        json_get_int(json, "dst", &dst_addr);
        json_get_str(json, "data", b64_data, sizeof(b64_data));

        uint8_t payload[2048];
        int plen = b64_decode(b64_data, payload, sizeof(payload));
        if (plen < 0) plen = 0;
        sim_send(core, (uint32_t)src_addr, (uint32_t)dst_addr, payload, (uint32_t)plen);
    }
    else if (strcmp(cmd, "set_mode") == 0)
    {
        json_get_str(json, "mode", mode_str, sizeof(mode_str));
        json_get_int(json, "delay_ms", &delay);
        sim_mode_t mode = SIM_MODE_NORMAL;
        if (strcmp(mode_str, "slow") == 0) mode = SIM_MODE_SLOW;
        else if (strcmp(mode_str, "step") == 0) mode = SIM_MODE_STEP;
        sim_set_mode(mode, (uint32_t)delay);
    }
    else if (strcmp(cmd, "step") == 0)              sim_step();
    else if (strcmp(cmd, "reset") == 0)             sim_reset();
    else if (strcmp(cmd, "deinit") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        sim_deinit_core(core);
    }
    /* "get_state" is handled in the main thread (read-only, fast) */
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID arg)
#else
static void *worker_main(void *arg)
#endif
{
    (void)arg;
    while (s_running)
    {
        sim_job_t *j = jobs_pop_blocking();
        if (!j) continue;
        execute_command(j->json);
        free(j->json);
        free(j);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ========================================================================== */
/* WebSocket Command Handler (main thread)                                     */
/* ========================================================================== */

/* These commands are handled inline on the main thread because they must not
 * be reordered behind a blocking operation: set_mode, step, get_state, reset.
 * "reset" is special — it cancels in-flight blocking ops by clearing
 * step_pending and waking sleepers. */
static int is_inline_cmd(const char *cmd)
{
    return (strcmp(cmd, "set_mode") == 0) ||
           (strcmp(cmd, "step") == 0) ||
           (strcmp(cmd, "get_state") == 0);
}

static void ws_send_str(struct mg_connection *c, const char *s)
{
    mg_ws_send(c, s, strlen(s), WEBSOCKET_OP_TEXT);
}

static void send_full_state(struct mg_connection *c)
{
    size_t cap = 32768;
    char *state_buf = (char *)malloc(cap);
    if (!state_buf) return;
    int offset = snprintf(state_buf, cap, "{\"type\":\"state\",\"state\":");
    int slen = sim_state_to_json(state_buf + offset, (int)(cap - offset - 4));
    offset += slen;
    state_buf[offset++] = '}';
    state_buf[offset] = '\0';
    mg_ws_send(c, state_buf, (size_t)offset, WEBSOCKET_OP_TEXT);
    free(state_buf);
}

static void broadcast_full_state(struct mg_mgr *mgr)
{
    size_t cap = 32768;
    char *state_buf = (char *)malloc(cap);
    if (!state_buf) return;
    int offset = snprintf(state_buf, cap, "{\"type\":\"state\",\"state\":");
    int slen = sim_state_to_json(state_buf + offset, (int)(cap - offset - 4));
    offset += slen;
    state_buf[offset++] = '}';
    state_buf[offset] = '\0';
    for (struct mg_connection *c = mgr->conns; c; c = c->next)
        if (c->is_websocket)
            mg_ws_send(c, state_buf, (size_t)offset, WEBSOCKET_OP_TEXT);
    free(state_buf);
}

static void handle_ws_command(struct mg_connection *c, struct mg_mgr *mgr,
                              const char *data, size_t len)
{
    char cmd[32] = {0};
    char *json = (char *)malloc(len + 1);
    if (!json) return;
    memcpy(json, data, len);
    json[len] = '\0';
    json_get_str(json, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "get_state") == 0)
    {
        send_full_state(c);
    }
    else if (strcmp(cmd, "step") == 0)
    {
        sim_step();
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"step\"}");
    }
    else if (strcmp(cmd, "set_mode") == 0)
    {
        /* Apply inline so step-mode toggles take effect immediately even if
         * a blocking send is currently waiting */
        char mode_str[16] = {0};
        int delay = 0;
        json_get_str(json, "mode", mode_str, sizeof(mode_str));
        json_get_int(json, "delay_ms", &delay);
        sim_mode_t mode = SIM_MODE_NORMAL;
        if (strcmp(mode_str, "slow") == 0) mode = SIM_MODE_SLOW;
        else if (strcmp(mode_str, "step") == 0) mode = SIM_MODE_STEP;
        sim_set_mode(mode, (uint32_t)delay);
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"set_mode\"}");
        broadcast_full_state(mgr);
    }
    else if (strcmp(cmd, "reset") == 0)
    {
        /* Run reset inline so it can interrupt any pending step wait */
        sim_reset();
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"reset\"}");
        broadcast_full_state(mgr);
    }
    else if (is_inline_cmd(cmd))
    {
        /* fallthrough handled above; placeholder */
    }
    else
    {
        /* Enqueue for the worker */
        jobs_push(json, len);
        char resp[96];
        snprintf(resp, sizeof(resp), "{\"type\":\"queued\",\"cmd\":\"%s\"}", cmd);
        ws_send_str(c, resp);
    }

    free(json);
}

/* ========================================================================== */
/* Recording save                                                              */
/* ========================================================================== */

static void get_exe_dir(char *buf, int buf_size)
{
#ifdef _WIN32
    GetModuleFileNameA(NULL, buf, buf_size);
    char *last_sep = strrchr(buf, '\\');
    if (!last_sep) last_sep = strrchr(buf, '/');
    if (last_sep) *(last_sep + 1) = '\0'; else buf[0] = '\0';
#else
    ssize_t len = readlink("/proc/self/exe", buf, buf_size - 1);
    if (len > 0) {
        buf[len] = '\0';
        char *last_sep = strrchr(buf, '/');
        if (last_sep) *(last_sep + 1) = '\0'; else buf[0] = '\0';
    } else buf[0] = '\0';
#endif
}

static void get_recordings_path(char *out, int out_size, const char *filename)
{
    char exe_dir[512];
    get_exe_dir(exe_dir, sizeof(exe_dir));
    if (filename)
        snprintf(out, out_size, "%srecordings%c%s", exe_dir,
#ifdef _WIN32
                 '\\',
#else
                 '/',
#endif
                 filename);
    else
        snprintf(out, out_size, "%srecordings", exe_dir);
}

static void ensure_recordings_dir(void)
{
    char dir_path[512];
    get_recordings_path(dir_path, sizeof(dir_path), NULL);
    MKDIR(dir_path);
}

static void handle_save_recording(struct mg_connection *c, struct mg_http_message *hm)
{
    ensure_recordings_dir();
    char basename[64];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(basename, sizeof(basename),
             "recording_%04d%02d%02d_%02d%02d%02d.json",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
#else
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    snprintf(basename, sizeof(basename),
             "recording_%04d%02d%02d_%02d%02d%02d.json",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
#endif

    char filepath[512];
    get_recordings_path(filepath, sizeof(filepath), basename);

    FILE *f = fopen(filepath, "wb");
    if (!f)
    {
        mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                      "{\"ok\":false,\"msg\":\"Failed to create file\"}");
        return;
    }
    fwrite(hm->body.buf, 1, hm->body.len, f);
    fclose(f);
    printf("[SIM] Recording saved: %s (%d bytes)\n", filepath, (int)hm->body.len);

    char resp[512];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"filename\":\"%s\"}", basename);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", resp);
}

/* ========================================================================== */
/* HTTP/WebSocket Event Handler                                                */
/* ========================================================================== */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL))
        {
            mg_ws_upgrade(c, hm, NULL);
        }
        else if (mg_strcmp(hm->uri, mg_str("/api/save_recording")) == 0)
        {
            handle_save_recording(c, hm);
        }
        else
        {
            struct mg_http_serve_opts opts = { .root_dir = "web", .ssi_pattern = NULL };
            mg_http_serve_dir(c, hm, &opts);
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        handle_ws_command(c, c->mgr, wm->data.buf, wm->data.len);
    }
}

/* ========================================================================== */
/* Event/state broadcast                                                       */
/* ========================================================================== */

static void broadcast_pending_events(struct mg_mgr *mgr)
{
    sim_event_t evt;
    char evt_json[2400];
    char msg[2600];
    int any = 0;

    while (sim_events_pop(&evt))
    {
        sim_event_to_json(&evt, evt_json, sizeof(evt_json));
        int mlen = snprintf(msg, sizeof(msg), "{\"type\":\"event\",\"event\":%s}", evt_json);
        for (struct mg_connection *c = mgr->conns; c; c = c->next)
            if (c->is_websocket)
                mg_ws_send(c, msg, mlen, WEBSOCKET_OP_TEXT);
        any = 1;
    }

    /* After draining events, push one fresh state snapshot so the UI repaints
     * exactly once per poll cycle that had activity. This is much cheaper
     * than per-event get_state round-trips and avoids state messages
     * arriving out of order. */
    if (any)
        broadcast_full_state(mgr);

    sim_state_t *state = sim_get_state();
    if (state->mode == SIM_MODE_STEP && state->step_pending)
    {
        const char *waiting = "{\"type\":\"waiting_step\"}";
        for (struct mg_connection *c = mgr->conns; c; c = c->next)
            if (c->is_websocket)
                mg_ws_send(c, waiting, strlen(waiting), WEBSOCKET_OP_TEXT);
    }
}

/* ========================================================================== */
/* Main                                                                        */
/* ========================================================================== */

int main(void)
{
    struct mg_mgr mgr;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("=== RPMsg-Lite Web Simulator ===\n");
    printf("Buffer count: %u per direction\n", (unsigned)RL_BUFFER_COUNT);
    printf("Buffer payload size: %u bytes\n", (unsigned)RL_BUFFER_PAYLOAD_SIZE);
    printf("Shared memory size: %u bytes\n", (unsigned)SIM_SHMEM_SIZE);

    if (sim_init() != 0)
    {
        fprintf(stderr, "Failed to initialize simulation\n");
        return 1;
    }

    jobs_init();

#ifdef _WIN32
    HANDLE worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!worker) { fprintf(stderr, "CreateThread failed\n"); return 1; }
#else
    pthread_t worker;
    if (pthread_create(&worker, NULL, worker_main, NULL) != 0)
    {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }
#endif

    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, s_listen_url, ev_handler, NULL);

    printf("Listening on %s\n", s_listen_url);
    printf("Open http://localhost:8088 in your browser\n\n");

    while (s_running)
    {
        mg_mgr_poll(&mgr, 30);
        broadcast_pending_events(&mgr);
    }

    printf("\nShutting down...\n");
#ifdef _WIN32
    SetEvent(g_jobs_event);
    WaitForSingleObject(worker, 2000);
    CloseHandle(worker);
#else
    pthread_cond_broadcast(&g_jobs_cond);
    pthread_join(worker, NULL);
#endif
    mg_mgr_free(&mgr);
    sim_reset();
    free(sim_get_state()->shmem);

    return 0;
}
