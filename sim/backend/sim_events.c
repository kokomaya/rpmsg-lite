/*
 * RPMsg-Lite Simulator - Event System Implementation
 */

#include "sim_events.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <pthread.h>
#endif

/* Ring buffer for events */
static sim_event_t g_event_queue[SIM_EVENT_QUEUE_SIZE];
static volatile int g_event_head = 0;
static volatile int g_event_tail = 0;
static uint32_t g_event_seq = 0;

#ifdef _WIN32
static CRITICAL_SECTION g_event_lock;
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_start;
#else
static pthread_mutex_t g_event_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timeval g_start;
#endif

static uint64_t get_timestamp_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart - g_start.QuadPart) * 1000000 / g_freq.QuadPart);
#else
    struct timeval now;
    gettimeofday(&now, NULL);
    return (uint64_t)(now.tv_sec - g_start.tv_sec) * 1000000ULL +
           (uint64_t)(now.tv_usec - g_start.tv_usec);
#endif
}

void sim_events_init(void)
{
    g_event_head = 0;
    g_event_tail = 0;
    g_event_seq = 0;

#ifdef _WIN32
    InitializeCriticalSection(&g_event_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
#else
    pthread_mutex_init(&g_event_lock, NULL);
    gettimeofday(&g_start, NULL);
#endif
}

void sim_events_push(sim_event_t *evt)
{
#ifdef _WIN32
    EnterCriticalSection(&g_event_lock);
#else
    pthread_mutex_lock(&g_event_lock);
#endif

    evt->seq = g_event_seq++;
    evt->timestamp_us = get_timestamp_us();

    int next = (g_event_head + 1) % SIM_EVENT_QUEUE_SIZE;
    if (next != g_event_tail)
    {
        g_event_queue[g_event_head] = *evt;
        g_event_head = next;
    }
    /* else: queue full, drop event */

#ifdef _WIN32
    LeaveCriticalSection(&g_event_lock);
#else
    pthread_mutex_unlock(&g_event_lock);
#endif
}

int sim_events_pop(sim_event_t *evt)
{
#ifdef _WIN32
    EnterCriticalSection(&g_event_lock);
#else
    pthread_mutex_lock(&g_event_lock);
#endif

    if (g_event_tail == g_event_head)
    {
#ifdef _WIN32
        LeaveCriticalSection(&g_event_lock);
#else
        pthread_mutex_unlock(&g_event_lock);
#endif
        return 0;
    }

    *evt = g_event_queue[g_event_tail];
    g_event_tail = (g_event_tail + 1) % SIM_EVENT_QUEUE_SIZE;

#ifdef _WIN32
    LeaveCriticalSection(&g_event_lock);
#else
    pthread_mutex_unlock(&g_event_lock);
#endif
    return 1;
}

int sim_events_count(void)
{
    int head = g_event_head;
    int tail = g_event_tail;
    return (head - tail + SIM_EVENT_QUEUE_SIZE) % SIM_EVENT_QUEUE_SIZE;
}

int sim_event_to_json(const sim_event_t *evt, char *buf, int buf_size)
{
    static const char *type_names[] = {
        "init_master", "init_remote", "deinit",
        "link_up", "link_down",
        "ept_create", "ept_destroy",
        "tx_alloc", "tx_send",
        "rx_callback", "rx_release", "rx_hold",
        "vring_add_buffer", "vring_get_buffer", "vring_kick",
        "ns_announce", "platform_notify", "buffer_fill"
    };

    const char *type_str = (evt->type < sizeof(type_names) / sizeof(type_names[0]))
                               ? type_names[evt->type]
                               : "unknown";

    int len = snprintf(buf, buf_size,
                       "{\"seq\":%u,\"timestamp_us\":%llu,\"type\":\"%s\","
                       "\"core\":\"%s\"",
                       evt->seq,
                       (unsigned long long)evt->timestamp_us,
                       type_str,
                       evt->core ? evt->core : "");

    if (evt->direction)
    {
        len += snprintf(buf + len, buf_size - len, ",\"direction\":\"%s\"", evt->direction);
    }

    if (evt->src || evt->dst || evt->addr)
    {
        len += snprintf(buf + len, buf_size - len,
                        ",\"src\":%u,\"dst\":%u,\"addr\":%u",
                        evt->src, evt->dst, evt->addr);
    }

    if (evt->desc_idx || evt->avail_idx || evt->used_idx)
    {
        len += snprintf(buf + len, buf_size - len,
                        ",\"desc_idx\":%u,\"avail_idx\":%u,\"used_idx\":%u",
                        evt->desc_idx, evt->avail_idx, evt->used_idx);
    }

    if (evt->buffer_len)
    {
        len += snprintf(buf + len, buf_size - len, ",\"buffer_len\":%u", evt->buffer_len);
    }

    if (evt->payload_preview[0])
    {
        len += snprintf(buf + len, buf_size - len, ",\"payload\":\"%s\"", evt->payload_preview);
    }

    len += snprintf(buf + len, buf_size - len, "}");
    return len;
}

void sim_event_record(sim_event_type_t type, const char *core, const char *direction)
{
    sim_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.core = core;
    evt.direction = direction;
    sim_events_push(&evt);
}

void sim_event_record_ept(sim_event_type_t type, const char *core, uint32_t addr)
{
    sim_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.core = core;
    evt.addr = addr;
    sim_events_push(&evt);
}

void sim_event_record_msg(sim_event_type_t type, const char *core, const char *dir,
                          uint32_t src, uint32_t dst, uint32_t len,
                          const uint8_t *payload, uint32_t payload_len)
{
    sim_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.core = core;
    evt.direction = dir;
    evt.src = src;
    evt.dst = dst;
    evt.buffer_len = len;

    /* Convert first bytes to hex preview */
    uint32_t preview_bytes = payload_len > 24 ? 24 : payload_len;
    for (uint32_t i = 0; i < preview_bytes && payload; i++)
    {
        snprintf(evt.payload_preview + i * 2, 3, "%02x", payload[i]);
    }

    sim_events_push(&evt);
}

void sim_event_record_vring(sim_event_type_t type, const char *core, const char *dir,
                            uint16_t desc_idx, uint16_t avail_idx, uint16_t used_idx)
{
    sim_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.core = core;
    evt.direction = dir;
    evt.desc_idx = desc_idx;
    evt.avail_idx = avail_idx;
    evt.used_idx = used_idx;
    sim_events_push(&evt);
}
