/*
 * RPMsg-Lite Simulator - Event System Header
 */

#ifndef SIM_EVENTS_H_
#define SIM_EVENTS_H_

#include <stdint.h>

/* Maximum events in the ring buffer */
#define SIM_EVENT_QUEUE_SIZE 256

/* Event types */
typedef enum
{
    SIM_EVT_INIT_MASTER = 0,
    SIM_EVT_INIT_REMOTE,
    SIM_EVT_DEINIT,
    SIM_EVT_LINK_UP,
    SIM_EVT_LINK_DOWN,
    SIM_EVT_EPT_CREATE,
    SIM_EVT_EPT_DESTROY,
    SIM_EVT_TX_ALLOC,
    SIM_EVT_TX_SEND,
    SIM_EVT_RX_CALLBACK,
    SIM_EVT_RX_RELEASE,
    SIM_EVT_RX_HOLD,
    SIM_EVT_VRING_ADD_BUFFER,
    SIM_EVT_VRING_GET_BUFFER,
    SIM_EVT_VRING_KICK,
    SIM_EVT_NS_ANNOUNCE,
    SIM_EVT_PLATFORM_NOTIFY,
    SIM_EVT_BUFFER_FILL,
} sim_event_type_t;

/* Event structure */
typedef struct
{
    uint32_t seq;
    uint64_t timestamp_us;
    sim_event_type_t type;
    const char *core;       /* "master" or "remote" */
    const char *direction;  /* "tx" or "rx" or NULL */
    /* Detail fields (type-specific) */
    uint32_t addr;          /* endpoint addr or buffer addr */
    uint32_t dst;
    uint32_t src;
    uint16_t desc_idx;
    uint16_t avail_idx;
    uint16_t used_idx;
    uint32_t buffer_len;
    char payload_preview[1024]; /* Full payload as hex string */
} sim_event_t;

/* Initialize event system */
void sim_events_init(void);

/* Push an event */
void sim_events_push(sim_event_t *evt);

/* Pop an event (returns 0 if empty) */
int sim_events_pop(sim_event_t *evt);

/* Get number of pending events */
int sim_events_count(void);

/* Format event to JSON string (returns length written) */
int sim_event_to_json(const sim_event_t *evt, char *buf, int buf_size);

/* Record a simple event with minimal info */
void sim_event_record(sim_event_type_t type, const char *core, const char *direction);

/* Record event with endpoint info */
void sim_event_record_ept(sim_event_type_t type, const char *core, uint32_t addr);

/* Record event with message info */
void sim_event_record_msg(sim_event_type_t type, const char *core, const char *dir,
                          uint32_t src, uint32_t dst, uint32_t len,
                          const uint8_t *payload, uint32_t payload_len);

/* Record vring event */
void sim_event_record_vring(sim_event_type_t type, const char *core, const char *dir,
                            uint16_t desc_idx, uint16_t avail_idx, uint16_t used_idx);

#endif /* SIM_EVENTS_H_ */
