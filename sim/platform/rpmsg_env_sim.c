#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

/* Pull in the headers via sim/core/include search path */
#include "rpmsg_env.h"
#include "rpmsg_platform_sim.h"
#include "ipc_sim.h"
#include "virtqueue.h"

/* ---- ISR table (per-side: one table for Master, one for Remote) --------- */

#define ISR_COUNT RL_PLATFORM_MAX_ISR_COUNT
struct isr_info { void *data; };
static struct isr_info isr_table[SIM_NUM_SIDES][ISR_COUNT];
static int32_t env_init_counter = 0;

/* ---- Lifecycle ---------------------------------------------------------- */

int32_t env_init(void)
{
    if (env_init_counter < 0) return -1;
    env_init_counter++;
    if (env_init_counter > 1) return 0;
    memset(isr_table, 0, sizeof(isr_table));
    return platform_init();
}
int32_t env_deinit(void)
{
    if (env_init_counter <= 0) return -1;
    env_init_counter--;
    if (env_init_counter > 0) return 0;
    return platform_deinit();
}

/* ---- Memory ------------------------------------------------------------- */

void *env_allocate_memory(uint32_t size)
{
    return malloc(size);
}
void  env_free_memory(void *ptr)                { if (ptr) free(ptr); }
void  env_memset(void *ptr, int32_t v, uint32_t n)      { memset(ptr, v & 0xFF, n); }
void  env_memcpy(void *dst, void const *src, uint32_t n){ memcpy(dst, src, n); }
int32_t env_strcmp(const char *a, const char *b)        { return strcmp(a, b); }
void  env_strncpy(char *d, const char *s, uint32_t n)   { strncpy(d, s, n); }
int32_t env_strncmp(char *d, const char *s, uint32_t n) { return strncmp(d, s, n); }
void  env_mb(void)  {}
void  env_rmb(void) {}
void  env_wmb(void) {}

/* ---- Address mapping (offset-based, to survive 64-bit pointer truncation) */
/* g_shmem_base is set by sim_main before any threads start                  */
static uint8_t *g_shmem_base = NULL;
void sim_set_shmem_base(void *base) { g_shmem_base = (uint8_t *)base; }

uint32_t env_map_vatopa(void *addr)
{
    /* Return shmem-relative offset (fits in 32 bits) */
    if (g_shmem_base && (uint8_t*)addr >= g_shmem_base)
        return (uint32_t)((uint8_t*)addr - g_shmem_base);
    /* Fallback for heap allocations (virtqueue structs) — use lower 32 bits.
     * These should never be stored in vring descriptors. */
    return (uint32_t)(uintptr_t)addr;
}

void *env_map_patova(uint32_t a)
{
    if (g_shmem_base)
        return (void *)(g_shmem_base + a);
    return (void *)(uintptr_t)a;
}

/* ---- Timing / misc ------------------------------------------------------ */

void     env_sleep_msec(uint32_t ms) { Sleep(ms); }
uint64_t env_get_timestamp(void)     { return (uint64_t)GetTickCount64(); }
void     env_disable_cache(void)     {}
void     env_cache_flush(void *d, uint32_t l)      { (void)d; (void)l; }
void     env_cache_invalidate(void *d, uint32_t l) { (void)d; (void)l; }
void     env_map_memory(uint32_t pa, uint32_t va, uint32_t sz, uint32_t fl)
         { (void)pa;(void)va;(void)sz;(void)fl; }

/* ---- Mutex -------------------------------------------------------------- */

typedef struct { CRITICAL_SECTION cs; } sim_mutex_t;

int32_t env_create_mutex(void **lock, int32_t count)
{
    sim_mutex_t *m = malloc(sizeof(sim_mutex_t));
    if (!m) return -1;
    InitializeCriticalSection(&m->cs);
    (void)count;
    *lock = m;
    return 0;
}

void env_delete_mutex(void *lock)
{
    if (!lock) return;
    DeleteCriticalSection(&((sim_mutex_t *)lock)->cs);
    free(lock);
}

void env_lock_mutex(void *lock)   { if (lock) EnterCriticalSection(&((sim_mutex_t *)lock)->cs); }
void env_unlock_mutex(void *lock) { if (lock) LeaveCriticalSection(&((sim_mutex_t *)lock)->cs); }

/* sync lock (binary semaphore, used by rpmsg_queue) */
typedef struct {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    int32_t state;   /* LOCKED=0, UNLOCKED=1 */
} sim_sync_t;

int32_t env_create_sync_lock(void **lock, int32_t state)
{
    sim_sync_t *s = malloc(sizeof(sim_sync_t));
    if (!s) return -1;
    InitializeCriticalSection(&s->cs);
    InitializeConditionVariable(&s->cv);
    s->state = state;
    *lock = s;
    return 0;
}

void env_delete_sync_lock(void *lock)
{
    if (!lock) return;
    DeleteCriticalSection(&((sim_sync_t *)lock)->cs);
    free(lock);
}

/* ---- Message queue ------------------------------------------------------ */

#define SIM_QUEUE_MAX 64

typedef struct {
    CRITICAL_SECTION  cs;
    CONDITION_VARIABLE cv_not_empty;
    CONDITION_VARIABLE cv_not_full;
    uint8_t          *buf;        /* ring buffer: length * element_size bytes */
    int32_t           elem_size;
    int32_t           capacity;
    int32_t           head, tail, count;
} sim_queue_t;

int32_t env_create_queue(void **queue, int32_t length, int32_t element_size)
{
    sim_queue_t *q = malloc(sizeof(sim_queue_t));
    if (!q) return -1;
    q->buf = malloc((size_t)length * (size_t)element_size);
    if (!q->buf) { free(q); return -1; }
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->cv_not_empty);
    InitializeConditionVariable(&q->cv_not_full);
    q->elem_size = element_size;
    q->capacity  = length;
    q->head = q->tail = q->count = 0;
    *queue = q;
    return 0;
}

void env_delete_queue(void *queue)
{
    if (!queue) return;
    sim_queue_t *q = (sim_queue_t *)queue;
    DeleteCriticalSection(&q->cs);
    free(q->buf);
    free(q);
}

int32_t env_put_queue(void *queue, void *msg, uintptr_t timeout_ms)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    DWORD wait_ms = (timeout_ms == (uintptr_t)~0UL) ? INFINITE : (DWORD)timeout_ms;

    EnterCriticalSection(&q->cs);
    while (q->count >= q->capacity) {
        if (!SleepConditionVariableCS(&q->cv_not_full, &q->cs, wait_ms)) {
            LeaveCriticalSection(&q->cs);
            return 0; /* timeout */
        }
    }
    memcpy(q->buf + (size_t)q->tail * (size_t)q->elem_size, msg, (size_t)q->elem_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    WakeConditionVariable(&q->cv_not_empty);
    LeaveCriticalSection(&q->cs);
    return 1;
}

int32_t env_get_queue(void *queue, void *msg, uintptr_t timeout_ms)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    DWORD wait_ms = (timeout_ms == (uintptr_t)~0UL) ? INFINITE : (DWORD)timeout_ms;

    EnterCriticalSection(&q->cs);
    while (q->count == 0) {
        if (!SleepConditionVariableCS(&q->cv_not_empty, &q->cs, wait_ms)) {
            LeaveCriticalSection(&q->cs);
            return 0; /* timeout */
        }
    }
    memcpy(msg, q->buf + (size_t)q->head * (size_t)q->elem_size, (size_t)q->elem_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    WakeConditionVariable(&q->cv_not_full);
    LeaveCriticalSection(&q->cs);
    return 1;
}

int32_t env_get_current_queue_size(void *queue)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    EnterCriticalSection(&q->cs);
    int32_t c = q->count;
    LeaveCriticalSection(&q->cs);
    return c;
}

/* ---- Interrupt handling ------------------------------------------------- */

void env_register_isr(uint32_t vector_id, void *data)
{
    uint32_t side = platform_get_current_side();
    if (vector_id < ISR_COUNT)
        isr_table[side][vector_id].data = data;
}

void env_unregister_isr(uint32_t vector_id)
{
    uint32_t side = platform_get_current_side();
    if (vector_id < ISR_COUNT)
        isr_table[side][vector_id].data = NULL;
}

void env_enable_interrupt(uint32_t vector_id)
{
    /* Replay: if vring already has pending work (master kicked before remote registered
     * its ISRs), calling env_isr here processes those pending entries so remote can
     * detect link_up and proceed. */
    uint32_t side = platform_get_current_side();
    if (vector_id < ISR_COUNT && isr_table[side][vector_id].data != NULL)
        virtqueue_notification((struct virtqueue *)isr_table[side][vector_id].data);
}
void env_disable_interrupt(uint32_t vector_id) { (void)vector_id; }

void env_isr(uint32_t vector)
{
    /* Called from ISR dispatcher thread which has set TLS side */
    uint32_t side = platform_get_current_side();
    if (vector < ISR_COUNT && isr_table[side][vector].data)
        virtqueue_notification((struct virtqueue *)isr_table[side][vector].data);
}

/* ---- Link-up polling (BM style) ----------------------------------------- */

uint32_t env_wait_for_link_up(volatile uint32_t *link_state, uint32_t link_id, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    (void)link_id;
    while (*link_state != 1U) {
        env_sleep_msec(RL_MS_PER_INTERVAL);
        if (timeout_ms != (uint32_t)~0UL) {
            elapsed += (uint32_t)RL_MS_PER_INTERVAL;
            if (elapsed >= timeout_ms) return 0U;
        }
    }
    return 1U;
}

void env_tx_callback(uint32_t link_id) { (void)link_id; }
