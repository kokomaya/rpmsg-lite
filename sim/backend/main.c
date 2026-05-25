/*
 * RPMsg-Lite Simulator - Main Entry Point
 * HTTP + WebSocket server using Mongoose library.
 * Serves the web GUI and handles simulation commands.
 */

/* Include mongoose before rpmsg headers to avoid MEM_MAPPED conflict */
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

#ifdef _WIN32
#include <windows.h>
#endif

static const char *s_listen_url = "http://0.0.0.0:8088";
static volatile int s_running = 1;

/* Signal handler for clean shutdown */
static void signal_handler(int sig)
{
    (void)sig;
    s_running = 0;
}

/* ========================================================================== */
/* JSON Command Parser (simple manual parsing, no external lib needed)         */
/* ========================================================================== */

static const char *json_get_str(const char *json, const char *key, char *out, int out_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return NULL;
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
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t') p++;
    *out = atoi(p);
    return 1;
}

/* Base64 decode (simplified for payload data) */
static int base64_decode(const char *src, int src_len, uint8_t *dst, int dst_size)
{
    static const int8_t b64_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    int out_len = 0;
    uint32_t accum = 0;
    int bits = 0;

    for (int i = 0; i < src_len && out_len < dst_size; i++)
    {
        if (src[i] == '=' || src[i] == '\n' || src[i] == '\r')
            continue;
        int8_t val = b64_table[(uint8_t)src[i]];
        if (val < 0) continue;
        accum = (accum << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            dst[out_len++] = (uint8_t)((accum >> bits) & 0xFF);
        }
    }
    return out_len;
}

/* ========================================================================== */
/* WebSocket Command Handler                                                   */
/* ========================================================================== */

/* Helper to send a string over WebSocket (auto-calculates length) */
static void ws_send_str(struct mg_connection *c, const char *s)
{
    mg_ws_send(c, s, strlen(s), WEBSOCKET_OP_TEXT);
}

static void handle_ws_command(struct mg_connection *c, const char *data, size_t len)
{
    char cmd[32] = {0};
    char core[16] = {0};
    char mode_str[16] = {0};
    char b64_data[2048] = {0};
    int addr = 0, src_addr = 0, dst_addr = 0, delay = 0;
    char resp[256];

    /* Ensure null-terminated for parsing */
    char *json = (char *)malloc(len + 1);
    if (!json) return;
    memcpy(json, data, len);
    json[len] = '\0';

    json_get_str(json, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "init_master") == 0)
    {
        int ret = sim_init_master();
        if (ret == 0)
            ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"init_master\"}");
        else
            ws_send_str(c, "{\"type\":\"error\",\"msg\":\"init_master failed\"}");
    }
    else if (strcmp(cmd, "init_remote") == 0)
    {
        int ret = sim_init_remote();
        if (ret == 0)
            ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"init_remote\"}");
        else
            ws_send_str(c, "{\"type\":\"error\",\"msg\":\"init_remote failed\"}");
    }
    else if (strcmp(cmd, "create_ept") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        json_get_int(json, "addr", &addr);
        int ret = sim_create_ept(core, (uint32_t)addr);
        if (ret == 0)
            snprintf(resp, sizeof(resp), "{\"type\":\"ok\",\"cmd\":\"create_ept\",\"core\":\"%s\",\"addr\":%d}", core, addr);
        else
            snprintf(resp, sizeof(resp), "{\"type\":\"error\",\"msg\":\"create_ept failed\"}");
        ws_send_str(c, resp);
    }
    else if (strcmp(cmd, "destroy_ept") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        json_get_int(json, "addr", &addr);
        sim_destroy_ept(core, (uint32_t)addr);
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"destroy_ept\"}");
    }
    else if (strcmp(cmd, "send") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        json_get_int(json, "src", &src_addr);
        json_get_int(json, "dst", &dst_addr);
        json_get_str(json, "data", b64_data, sizeof(b64_data));

        uint8_t payload[512];
        int payload_len = base64_decode(b64_data, (int)strlen(b64_data), payload, sizeof(payload));

        int ret = sim_send(core, (uint32_t)src_addr, (uint32_t)dst_addr, payload, (uint32_t)payload_len);
        if (ret == 0)
            snprintf(resp, sizeof(resp), "{\"type\":\"ok\",\"cmd\":\"send\",\"len\":%d}", payload_len);
        else
            snprintf(resp, sizeof(resp), "{\"type\":\"error\",\"msg\":\"send failed: %d\"}", ret);
        ws_send_str(c, resp);
    }
    else if (strcmp(cmd, "set_mode") == 0)
    {
        json_get_str(json, "mode", mode_str, sizeof(mode_str));
        json_get_int(json, "delay_ms", &delay);

        sim_mode_t mode = SIM_MODE_NORMAL;
        if (strcmp(mode_str, "slow") == 0) mode = SIM_MODE_SLOW;
        else if (strcmp(mode_str, "step") == 0) mode = SIM_MODE_STEP;

        sim_set_mode(mode, (uint32_t)delay);
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"set_mode\"}");
    }
    else if (strcmp(cmd, "step") == 0)
    {
        sim_step();
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"step\"}");
    }
    else if (strcmp(cmd, "reset") == 0)
    {
        sim_reset();
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"reset\"}");
    }
    else if (strcmp(cmd, "deinit") == 0)
    {
        json_get_str(json, "core", core, sizeof(core));
        sim_deinit_core(core);
        ws_send_str(c, "{\"type\":\"ok\",\"cmd\":\"deinit\"}");
    }
    else if (strcmp(cmd, "get_state") == 0)
    {
        /* Send full state snapshot */
        char *state_buf = (char *)malloc(65536);
        if (state_buf)
        {
            int offset = snprintf(state_buf, 65536, "{\"type\":\"state\",\"state\":");
            int slen = sim_state_to_json(state_buf + offset, 65536 - offset - 2);
            offset += slen;
            state_buf[offset++] = '}';
            state_buf[offset] = '\0';
            mg_ws_send(c, state_buf, (size_t)offset, WEBSOCKET_OP_TEXT);
            free(state_buf);
        }
    }
    else
    {
        snprintf(resp, sizeof(resp), "{\"type\":\"error\",\"msg\":\"unknown command: %s\"}", cmd);
        ws_send_str(c, resp);
    }

    free(json);
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
            /* Upgrade to WebSocket */
            mg_ws_upgrade(c, hm, NULL);
        }
        else
        {
            /* Serve static files from web/ directory */
            struct mg_http_serve_opts opts = {
                .root_dir = "web",
                .ssi_pattern = NULL
            };
            mg_http_serve_dir(c, hm, &opts);
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        handle_ws_command(c, wm->data.buf, wm->data.len);
    }
}

/* ========================================================================== */
/* Event Broadcast (push events to all WS clients)                             */
/* ========================================================================== */

static void broadcast_events(struct mg_mgr *mgr)
{
    sim_event_t evt;
    char buf[1024];
    char msg[1100];

    while (sim_events_pop(&evt))
    {
        sim_event_to_json(&evt, buf, sizeof(buf));
        int mlen = snprintf(msg, sizeof(msg), "{\"type\":\"event\",\"event\":%s}", buf);

        /* Broadcast to all WebSocket connections */
        for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next)
        {
            if (c->is_websocket)
            {
                mg_ws_send(c, msg, mlen, WEBSOCKET_OP_TEXT);
            }
        }
    }

    /* Also broadcast step_pending status if in step mode */
    sim_state_t *state = sim_get_state();
    if (state->mode == SIM_MODE_STEP && state->step_pending)
    {
        const char *waiting = "{\"type\":\"waiting_step\"}";
        for (struct mg_connection *c = mgr->conns; c != NULL; c = c->next)
        {
            if (c->is_websocket)
            {
                mg_ws_send(c, waiting, strlen(waiting), WEBSOCKET_OP_TEXT);
            }
        }
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
    printf("\n");

    /* Initialize simulation */
    if (sim_init() != 0)
    {
        fprintf(stderr, "Failed to initialize simulation\n");
        return 1;
    }

    /* Initialize Mongoose */
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, s_listen_url, ev_handler, NULL);

    printf("Listening on %s\n", s_listen_url);
    printf("Open http://localhost:8088 in your browser\n\n");

    /* Main loop */
    while (s_running)
    {
        mg_mgr_poll(&mgr, 50); /* 50ms poll interval */
        broadcast_events(&mgr);
    }

    printf("\nShutting down...\n");
    mg_mgr_free(&mgr);
    sim_reset();
    free(sim_get_state()->shmem);

    return 0;
}
