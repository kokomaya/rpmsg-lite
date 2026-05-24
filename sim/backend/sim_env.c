/*
 * RPMsg-Lite Simulator - Environment Layer Implementation
 * Implements all env_* functions required by rpmsg_env.h for simulation.
 * This replaces the hardware/RTOS-specific environment.
 */

#include "rpmsg_env.h"
#include "sim_env.h"
#include "sim_events.h"
#include "sim_core.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#endif

/* Global shared memory pointer */
uint8_t *g_sim_shmem_base = NULL;
uint32_t g_sim_shmem_size = 0;
volatile int g_sim_link_up_signal = 0;

/* ISR registry */
#define MAX_ISR_VECTORS 8
static void *g_isr_table[MAX_ISR_VECTORS] = {0};
static int g_isr_enabled[MAX_ISR_VECTORS] = {0};

/* Simple queue implementation for simulation */
typedef struct sim_queue
{
    uint8_t *storage;
    int32_t element_size;
    int32_t capacity;
    volatile int32_t head;
    volatile int32_t tail;
    volatile int32_t count;
#ifdef _WIN32
    CRITICAL_SECTION lock;
    HANDLE not_empty_event;
#else
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
#endif
} sim_queue_t;

/* ========================================================================== */
/* Environment Init/Deinit                                                     */
/* ========================================================================== */

static int g_env_initialized = 0;

int32_t env_init(void)
{
    if (g_env_initialized)
        return 0;
    g_env_initialized = 1;
    return 0;
}

int32_t env_deinit(void)
{
    g_env_initialized = 0;
    return 0;
}

/* ========================================================================== */
/* Memory Management                                                           */
/* ========================================================================== */

void *env_allocate_memory(uint32_t size)
{
    return malloc(size);
}

void env_free_memory(void *ptr)
{
    free(ptr);
}

/* ========================================================================== */
/* RTL Functions                                                                */
/* ========================================================================== */

void env_memset(void *ptr, int32_t value, uint32_t size)
{
    memset(ptr, value, size);
}

void env_memcpy(void *dst, void const *src, uint32_t len)
{
    memcpy(dst, src, len);
}

int32_t env_strcmp(const char *dst, const char *src)
{
    return (int32_t)strcmp(dst, src);
}

void env_strncpy(char *dest, const char *src, uint32_t len)
{
    strncpy(dest, src, len);
}

int32_t env_strncmp(char *dest, const char *src, uint32_t len)
{
    return (int32_t)strncmp(dest, src, len);
}

/* ========================================================================== */
/* Address Translation (identity mapping in simulation)                        */
/* ========================================================================== */

uint32_t env_map_vatopa(void *address)
{
    return (uint32_t)(uintptr_t)address;
}

void *env_map_patova(uint32_t address)
{
    return (void *)(uintptr_t)address;
}

/* ========================================================================== */
/* Memory Barriers (no-op in single-threaded simulation)                       */
/* ========================================================================== */

void env_mb(void)
{
    /* No hardware memory barrier needed in simulation */
}

void env_rmb(void)
{
}

void env_wmb(void)
{
}

/* ========================================================================== */
/* Mutex (uses OS primitives)                                                  */
/* ========================================================================== */

#ifdef _WIN32
int32_t env_create_mutex(void **lock, int32_t count, void *context)
{
    (void)context;
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return -1;
    InitializeCriticalSection(cs);
    *lock = cs;
    return 0;
}

void env_delete_mutex(void *lock)
{
    if (lock)
    {
        DeleteCriticalSection((CRITICAL_SECTION *)lock);
        free(lock);
    }
}

void env_lock_mutex(void *lock)
{
    if (lock)
        EnterCriticalSection((CRITICAL_SECTION *)lock);
}

void env_unlock_mutex(void *lock)
{
    if (lock)
        LeaveCriticalSection((CRITICAL_SECTION *)lock);
}
#else
int32_t env_create_mutex(void **lock, int32_t count, void *context)
{
    (void)context;
    pthread_mutex_t *mtx = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!mtx) return -1;
    pthread_mutex_init(mtx, NULL);
    *lock = mtx;
    return 0;
}

void env_delete_mutex(void *lock)
{
    if (lock)
    {
        pthread_mutex_destroy((pthread_mutex_t *)lock);
        free(lock);
    }
}

void env_lock_mutex(void *lock)
{
    if (lock)
        pthread_mutex_lock((pthread_mutex_t *)lock);
}

void env_unlock_mutex(void *lock)
{
    if (lock)
        pthread_mutex_unlock((pthread_mutex_t *)lock);
}
#endif

/* ========================================================================== */
/* Sync Lock (same as mutex for simulation)                                    */
/* ========================================================================== */

int32_t env_create_sync_lock(void **lock, int32_t state, void *context)
{
    return env_create_mutex(lock, state, context);
}

void env_delete_sync_lock(void *lock)
{
    env_delete_mutex(lock);
}

/* ========================================================================== */
/* Sleep                                                                        */
/* ========================================================================== */

void env_sleep_msec(uint32_t num_msec)
{
#ifdef _WIN32
    Sleep(num_msec);
#else
    usleep(num_msec * 1000);
#endif
}

/* ========================================================================== */
/* ISR Registration                                                            */
/* ========================================================================== */

void env_register_isr(uint32_t vector_id, void *data)
{
    if (vector_id < MAX_ISR_VECTORS)
    {
        g_isr_table[vector_id] = data;
    }
}

void env_unregister_isr(uint32_t vector_id)
{
    if (vector_id < MAX_ISR_VECTORS)
    {
        g_isr_table[vector_id] = NULL;
    }
}

void env_enable_interrupt(uint32_t vector_id)
{
    if (vector_id < MAX_ISR_VECTORS)
    {
        g_isr_enabled[vector_id] = 1;
    }
}

void env_disable_interrupt(uint32_t vector_id)
{
    if (vector_id < MAX_ISR_VECTORS)
    {
        g_isr_enabled[vector_id] = 0;
    }
}

/* ========================================================================== */
/* ISR Invocation (called by platform_notify to simulate interrupt)            */
/* ========================================================================== */

void env_isr(uint32_t vector_id)
{
    if (vector_id < MAX_ISR_VECTORS && g_isr_table[vector_id] && g_isr_enabled[vector_id])
    {
        struct virtqueue *vq = (struct virtqueue *)g_isr_table[vector_id];
        if (vq->callback_fc)
        {
            vq->callback_fc(vq);
        }
    }
}

/* ========================================================================== */
/* Memory Mapping (no-op in simulation)                                        */
/* ========================================================================== */

void env_map_memory(uint32_t pa, uint32_t va, uint32_t size, uint32_t flags)
{
    (void)pa;
    (void)va;
    (void)size;
    (void)flags;
}

/* ========================================================================== */
/* Timestamp                                                                   */
/* ========================================================================== */

uint64_t env_get_timestamp(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000 / freq.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
}

/* ========================================================================== */
/* Cache Operations (no-op in simulation)                                      */
/* ========================================================================== */

void env_disable_cache(void)
{
}

void env_cache_flush(void *data, uint32_t len)
{
    (void)data;
    (void)len;
}

void env_cache_invalidate(void *data, uint32_t len)
{
    (void)data;
    (void)len;
}

/* ========================================================================== */
/* Queue Implementation                                                        */
/* ========================================================================== */

int32_t env_create_queue(void **queue, int32_t length, int32_t element_size,
                         uint8_t *queue_static_storage,
                         rpmsg_static_queue_ctxt *queue_static_context)
{
    (void)queue_static_storage;
    (void)queue_static_context;

    sim_queue_t *q = (sim_queue_t *)malloc(sizeof(sim_queue_t));
    if (!q) return -1;

    q->element_size = element_size;
    q->capacity = length;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->storage = (uint8_t *)malloc((uint32_t)(length * element_size));
    if (!q->storage)
    {
        free(q);
        return -1;
    }

#ifdef _WIN32
    InitializeCriticalSection(&q->lock);
    q->not_empty_event = CreateEvent(NULL, FALSE, FALSE, NULL);
#else
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
#endif

    *queue = q;
    return 0;
}

void env_delete_queue(void *queue)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    if (!q) return;

#ifdef _WIN32
    DeleteCriticalSection(&q->lock);
    CloseHandle(q->not_empty_event);
#else
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
#endif

    free(q->storage);
    free(q);
}

int32_t env_put_queue(void *queue, void *msg, uintptr_t timeout_ms)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    if (!q) return 0;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count >= q->capacity)
    {
#ifdef _WIN32
        LeaveCriticalSection(&q->lock);
#else
        pthread_mutex_unlock(&q->lock);
#endif
        return 0;
    }

    memcpy(q->storage + q->head * q->element_size, msg, (size_t)q->element_size);
    q->head = (q->head + 1) % q->capacity;
    q->count++;

#ifdef _WIN32
    SetEvent(q->not_empty_event);
    LeaveCriticalSection(&q->lock);
#else
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
#endif

    (void)timeout_ms;
    return 1;
}

int32_t env_get_queue(void *queue, void *msg, uintptr_t timeout_ms)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    if (!q) return 0;

#ifdef _WIN32
    EnterCriticalSection(&q->lock);
    while (q->count == 0)
    {
        LeaveCriticalSection(&q->lock);
        if (timeout_ms == 0)
            return 0;
        WaitForSingleObject(q->not_empty_event, (DWORD)(timeout_ms == (uintptr_t)(~0UL) ? INFINITE : timeout_ms));
        EnterCriticalSection(&q->lock);
        if (q->count == 0 && timeout_ms != (uintptr_t)(~0UL))
        {
            LeaveCriticalSection(&q->lock);
            return 0;
        }
    }
#else
    pthread_mutex_lock(&q->lock);
    while (q->count == 0)
    {
        if (timeout_ms == 0)
        {
            pthread_mutex_unlock(&q->lock);
            return 0;
        }
        if (timeout_ms == (uintptr_t)(~0UL))
        {
            pthread_cond_wait(&q->not_empty, &q->lock);
        }
        else
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (timeout_ms % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000)
            {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            int ret = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
            if (ret != 0 && q->count == 0)
            {
                pthread_mutex_unlock(&q->lock);
                return 0;
            }
        }
    }
#endif

    memcpy(msg, q->storage + q->tail * q->element_size, (size_t)q->element_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif

    return 1;
}

int32_t env_get_current_queue_size(void *queue)
{
    sim_queue_t *q = (sim_queue_t *)queue;
    if (!q) return 0;
    return q->count;
}

/* ========================================================================== */
/* Link Up Wait / TX Callback                                                  */
/* ========================================================================== */

uint32_t env_wait_for_link_up(volatile uint32_t *link_state, uint32_t link_id, uint32_t timeout_ms)
{
    (void)link_id;
    uint32_t elapsed = 0;

    while (*link_state != 1U)
    {
        if (timeout_ms != (uint32_t)(~0UL) && elapsed >= timeout_ms)
        {
            return 0U; /* RL_FALSE */
        }
        env_sleep_msec(1);
        elapsed += 1;
    }
    return 1U; /* RL_TRUE */
}

void env_tx_callback(uint32_t link_id)
{
    (void)link_id;
    g_sim_link_up_signal = 1;
    sim_event_record(SIM_EVT_LINK_UP, "master", NULL);
}
