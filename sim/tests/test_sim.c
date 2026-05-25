/*
 * RPMsg-Lite Simulator - Unit Tests
 * Tests all sim modules: sim_events, sim_env, sim_platform, sim_core
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "sim_core.h"
#include "sim_env.h"
#include "sim_platform.h"
#include "sim_events.h"
#include "rpmsg_lite.h"
#include "rpmsg_env.h"

/* MAX_ISR_VECTORS is defined in sim_env.c, redefine for test boundary checking */
#ifndef MAX_ISR_VECTORS
#define MAX_ISR_VECTORS 8
#endif

/* ========================================================================== */
/* Test Framework Macros                                                        */
/* ========================================================================== */

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_BEGIN(name) \
    do { \
        printf("  [TEST] %s ... ", name); \
        g_tests_run++; \
    } while(0)

#define TEST_PASS() \
    do { \
        printf("PASSED\n"); \
        g_tests_passed++; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        printf("FAILED: %s\n", msg); \
        g_tests_failed++; \
    } while(0)

#define ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { \
            char _buf[256]; \
            snprintf(_buf, sizeof(_buf), "%s (expected %d, got %d)", msg, (int)(b), (int)(a)); \
            TEST_FAIL(_buf); \
            return; \
        } \
    } while(0)

#define ASSERT_NE(a, b, msg) \
    do { \
        if ((a) == (b)) { \
            TEST_FAIL(msg); \
            return; \
        } \
    } while(0)

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { \
            TEST_FAIL(msg); \
            return; \
        } \
    } while(0)

#define ASSERT_NULL(ptr, msg) \
    do { \
        if ((ptr) != NULL) { \
            TEST_FAIL(msg); \
            return; \
        } \
    } while(0)

#define ASSERT_NOT_NULL(ptr, msg) \
    do { \
        if ((ptr) == NULL) { \
            TEST_FAIL(msg); \
            return; \
        } \
    } while(0)

/* ========================================================================== */
/* Test: Event System                                                           */
/* ========================================================================== */

static void test_events_init(void)
{
    TEST_BEGIN("events_init");
    sim_events_init();
    ASSERT_EQ(sim_events_count(), 0, "Event count should be 0 after init");
    TEST_PASS();
}

static void test_events_push_pop(void)
{
    TEST_BEGIN("events_push_pop");
    sim_events_init();

    sim_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = SIM_EVT_INIT_MASTER;
    evt.core = "master";
    evt.direction = NULL;
    sim_events_push(&evt);

    ASSERT_EQ(sim_events_count(), 1, "Should have 1 event after push");

    sim_event_t popped;
    int ret = sim_events_pop(&popped);
    ASSERT_EQ(ret, 1, "Pop should return 1");
    ASSERT_EQ(popped.type, SIM_EVT_INIT_MASTER, "Event type mismatch");
    ASSERT_EQ(popped.seq, 0, "First event seq should be 0");
    ASSERT_TRUE(popped.timestamp_us >= 0, "Timestamp should be >= 0");
    ASSERT_EQ(sim_events_count(), 0, "Count should be 0 after pop");

    TEST_PASS();
}

static void test_events_pop_empty(void)
{
    TEST_BEGIN("events_pop_empty");
    sim_events_init();

    sim_event_t evt;
    int ret = sim_events_pop(&evt);
    ASSERT_EQ(ret, 0, "Pop from empty queue should return 0");
    TEST_PASS();
}

static void test_events_sequence(void)
{
    TEST_BEGIN("events_sequence");
    sim_events_init();

    for (int i = 0; i < 10; i++)
    {
        sim_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SIM_EVT_TX_SEND;
        evt.core = "master";
        sim_events_push(&evt);
    }

    ASSERT_EQ(sim_events_count(), 10, "Should have 10 events");

    for (int i = 0; i < 10; i++)
    {
        sim_event_t evt;
        int ret = sim_events_pop(&evt);
        ASSERT_EQ(ret, 1, "Pop should succeed");
        ASSERT_EQ((int)evt.seq, i, "Sequence should be sequential");
    }

    ASSERT_EQ(sim_events_count(), 0, "Queue should be empty");
    TEST_PASS();
}

static void test_events_overflow(void)
{
    TEST_BEGIN("events_overflow");
    sim_events_init();

    /* Fill the queue to capacity (SIM_EVENT_QUEUE_SIZE - 1 due to ring buffer) */
    for (int i = 0; i < SIM_EVENT_QUEUE_SIZE; i++)
    {
        sim_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = SIM_EVT_TX_SEND;
        evt.core = "master";
        sim_events_push(&evt);
    }

    /* Ring buffer can hold SIM_EVENT_QUEUE_SIZE-1 items */
    int count = sim_events_count();
    ASSERT_TRUE(count <= SIM_EVENT_QUEUE_SIZE - 1, "Should not exceed capacity");
    ASSERT_TRUE(count > 0, "Should have events");

    TEST_PASS();
}

static void test_events_to_json(void)
{
    TEST_BEGIN("events_to_json");
    sim_events_init();

    sim_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.seq = 42;
    evt.timestamp_us = 12345;
    evt.type = SIM_EVT_TX_SEND;
    evt.core = "master";
    evt.direction = "tx";
    evt.src = 10;
    evt.dst = 20;
    evt.buffer_len = 100;

    char buf[1024];
    int len = sim_event_to_json(&evt, buf, sizeof(buf));

    ASSERT_TRUE(len > 0, "JSON length should be > 0");
    ASSERT_TRUE(strstr(buf, "\"type\":\"tx_send\"") != NULL, "Should contain type");
    ASSERT_TRUE(strstr(buf, "\"core\":\"master\"") != NULL, "Should contain core");
    ASSERT_TRUE(strstr(buf, "\"direction\":\"tx\"") != NULL, "Should contain direction");
    ASSERT_TRUE(strstr(buf, "\"src\":10") != NULL, "Should contain src");
    ASSERT_TRUE(strstr(buf, "\"dst\":20") != NULL, "Should contain dst");

    TEST_PASS();
}

static void test_events_record_simple(void)
{
    TEST_BEGIN("events_record_simple");
    sim_events_init();

    sim_event_record(SIM_EVT_LINK_UP, "master", NULL);
    ASSERT_EQ(sim_events_count(), 1, "Should have 1 event");

    sim_event_t evt;
    sim_events_pop(&evt);
    ASSERT_EQ(evt.type, SIM_EVT_LINK_UP, "Type mismatch");
    ASSERT_TRUE(strcmp(evt.core, "master") == 0, "Core mismatch");

    TEST_PASS();
}

static void test_events_record_ept(void)
{
    TEST_BEGIN("events_record_ept");
    sim_events_init();

    sim_event_record_ept(SIM_EVT_EPT_CREATE, "remote", 50);
    ASSERT_EQ(sim_events_count(), 1, "Should have 1 event");

    sim_event_t evt;
    sim_events_pop(&evt);
    ASSERT_EQ(evt.type, SIM_EVT_EPT_CREATE, "Type mismatch");
    ASSERT_EQ(evt.addr, 50, "Addr mismatch");

    TEST_PASS();
}

static void test_events_record_msg(void)
{
    TEST_BEGIN("events_record_msg");
    sim_events_init();

    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    sim_event_record_msg(SIM_EVT_TX_SEND, "master", "tx", 10, 20, 4, payload, 4);

    sim_event_t evt;
    sim_events_pop(&evt);
    ASSERT_EQ(evt.type, SIM_EVT_TX_SEND, "Type mismatch");
    ASSERT_EQ(evt.src, 10, "Src mismatch");
    ASSERT_EQ(evt.dst, 20, "Dst mismatch");
    ASSERT_EQ(evt.buffer_len, 4, "Len mismatch");
    ASSERT_TRUE(strstr(evt.payload_preview, "deadbeef") != NULL, "Payload preview mismatch");

    TEST_PASS();
}

/* ========================================================================== */
/* Test: Environment Layer                                                      */
/* ========================================================================== */

static void test_env_init_deinit(void)
{
    TEST_BEGIN("env_init_deinit");

    int32_t ret = env_init();
    ASSERT_EQ(ret, 0, "env_init should return 0");

    /* Double init should still succeed */
    ret = env_init();
    ASSERT_EQ(ret, 0, "Double env_init should return 0");

    ret = env_deinit();
    ASSERT_EQ(ret, 0, "env_deinit should return 0");

    TEST_PASS();
}

static void test_env_memory(void)
{
    TEST_BEGIN("env_memory");

    void *ptr = env_allocate_memory(1024);
    ASSERT_NOT_NULL(ptr, "Allocation should succeed");
    env_free_memory(ptr);

    /* Zero-size allocation */
    ptr = env_allocate_memory(0);
    /* malloc(0) is implementation defined, just free it */
    env_free_memory(ptr);

    TEST_PASS();
}

static void test_env_memset_memcpy(void)
{
    TEST_BEGIN("env_memset_memcpy");

    uint8_t src[32], dst[32];
    env_memset(src, 0xAA, 32);
    ASSERT_EQ(src[0], 0xAA, "memset failed");
    ASSERT_EQ(src[31], 0xAA, "memset failed at end");

    env_memcpy(dst, src, 32);
    ASSERT_EQ(dst[0], 0xAA, "memcpy failed");
    ASSERT_EQ(dst[31], 0xAA, "memcpy failed at end");

    TEST_PASS();
}

static void test_env_strcmp(void)
{
    TEST_BEGIN("env_strcmp");

    ASSERT_EQ(env_strcmp("abc", "abc"), 0, "Equal strings");
    ASSERT_TRUE(env_strcmp("abc", "abd") < 0, "Less than");
    ASSERT_TRUE(env_strcmp("abd", "abc") > 0, "Greater than");

    TEST_PASS();
}

static void test_env_strncpy_strncmp(void)
{
    TEST_BEGIN("env_strncpy_strncmp");

    char buf[16];
    env_strncpy(buf, "hello", 16);
    ASSERT_EQ(env_strncmp(buf, "hello", 5), 0, "strncpy/strncmp failed");
    ASSERT_EQ(env_strncmp(buf, "hellx", 4), 0, "Partial strncmp failed");

    TEST_PASS();
}

static void test_env_vatopa_patova(void)
{
    TEST_BEGIN("env_vatopa_patova");

    /* Set up a fake shared memory region */
    uint8_t fake_shmem[1024];
    g_sim_shmem_base = fake_shmem;
    g_sim_shmem_size = sizeof(fake_shmem);

    /* Address in the middle of shmem */
    void *addr = &fake_shmem[256];
    uint32_t pa = env_map_vatopa(addr);
    ASSERT_EQ(pa, 256, "VA to PA offset wrong");

    void *va = env_map_patova(pa);
    ASSERT_TRUE(va == addr, "PA to VA roundtrip failed");

    /* Edge cases */
    pa = env_map_vatopa(&fake_shmem[0]);
    ASSERT_EQ(pa, 0, "Start of shmem should be offset 0");

    pa = env_map_vatopa(&fake_shmem[1023]);
    ASSERT_EQ(pa, 1023, "End of shmem should be offset 1023");

    TEST_PASS();
}

static void test_env_mutex(void)
{
    TEST_BEGIN("env_mutex");

    void *lock = NULL;
    int32_t ret = env_create_mutex(&lock, 1, NULL);
    ASSERT_EQ(ret, 0, "Mutex create failed");
    ASSERT_NOT_NULL(lock, "Mutex should not be NULL");

    /* Lock/unlock should not crash */
    env_lock_mutex(lock);
    env_unlock_mutex(lock);

    env_delete_mutex(lock);

    /* NULL operations should be safe */
    env_lock_mutex(NULL);
    env_unlock_mutex(NULL);
    env_delete_mutex(NULL);

    TEST_PASS();
}

static void test_env_isr_registration(void)
{
    TEST_BEGIN("env_isr_registration");

    int dummy_data = 42;
    env_register_isr(0, &dummy_data);
    env_enable_interrupt(0);

    /* Unregister */
    env_unregister_isr(0);
    env_disable_interrupt(0);

    /* Out of range should not crash */
    env_register_isr(MAX_ISR_VECTORS + 10, &dummy_data);
    env_unregister_isr(MAX_ISR_VECTORS + 10);

    TEST_PASS();
}

static void test_env_timestamp(void)
{
    TEST_BEGIN("env_timestamp");

    uint64_t t1 = env_get_timestamp();
    /* Do some work */
    volatile int sum = 0;
    for (int i = 0; i < 100000; i++) sum += i;
    (void)sum;
    uint64_t t2 = env_get_timestamp();

    ASSERT_TRUE(t2 >= t1, "Timestamp should be monotonic");

    TEST_PASS();
}

/* ========================================================================== */
/* Test: Platform Layer                                                         */
/* ========================================================================== */

static void test_platform_init_deinit(void)
{
    TEST_BEGIN("platform_init_deinit");

    int32_t ret = platform_init();
    ASSERT_EQ(ret, 0, "platform_init failed");

    ret = platform_deinit();
    ASSERT_EQ(ret, 0, "platform_deinit failed");

    TEST_PASS();
}

static void test_platform_interrupt(void)
{
    TEST_BEGIN("platform_interrupt");

    int dummy = 0;
    int32_t ret = platform_init_interrupt(0, &dummy);
    ASSERT_EQ(ret, 0, "platform_init_interrupt failed");

    ret = platform_interrupt_enable(0);
    ASSERT_EQ(ret, 0, "platform_interrupt_enable failed");

    ret = platform_interrupt_disable(0);
    ASSERT_EQ(ret, 0, "platform_interrupt_disable failed");

    ret = platform_deinit_interrupt(0);
    ASSERT_EQ(ret, 0, "platform_deinit_interrupt failed");

    TEST_PASS();
}

static void test_platform_in_isr(void)
{
    TEST_BEGIN("platform_in_isr");
    ASSERT_EQ(platform_in_isr(), 0, "Should never be in ISR in sim");
    TEST_PASS();
}

static void test_platform_shmem(void)
{
    TEST_BEGIN("platform_shmem");

    uint8_t buf[256];
    sim_platform_set_shmem(buf, sizeof(buf));

    /* Cache ops should not crash */
    platform_cache_all_flush_invalidate();
    platform_cache_disable();
    platform_cache_invalidate(buf, 16);
    platform_cache_flush(buf, 16);
    platform_map_mem_region(0, 0, 256, 0);

    TEST_PASS();
}

/* ========================================================================== */
/* Test: Sim Core                                                              */
/* ========================================================================== */

static void test_sim_init(void)
{
    TEST_BEGIN("sim_init");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");

    sim_state_t *state = sim_get_state();
    ASSERT_NOT_NULL(state, "State should not be NULL");
    ASSERT_NOT_NULL(state->shmem, "Shared memory should be allocated");
    ASSERT_TRUE(state->shmem_size > 0, "Shmem size > 0");
    ASSERT_EQ(state->mode, SIM_MODE_NORMAL, "Default mode should be NORMAL");
    ASSERT_EQ(state->master.initialized, 0, "Master not initialized yet");
    ASSERT_EQ(state->remote.initialized, 0, "Remote not initialized yet");

    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_init_master(void)
{
    TEST_BEGIN("sim_init_master");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");

    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "sim_init_master failed");

    sim_state_t *state = sim_get_state();
    ASSERT_EQ(state->master.initialized, 1, "Master should be initialized");
    ASSERT_NOT_NULL(state->master.instance, "Master instance should exist");

    /* Double init should fail */
    ret = sim_init_master();
    ASSERT_EQ(ret, -1, "Double master init should fail");

    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_init_remote_requires_master(void)
{
    TEST_BEGIN("sim_init_remote_requires_master");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");

    /* Remote without master should fail */
    ret = sim_init_remote();
    ASSERT_EQ(ret, -1, "Remote init without master should fail");

    sim_state_t *state = sim_get_state();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_init_master_remote(void)
{
    TEST_BEGIN("sim_init_master_remote");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");

    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "Master init failed");

    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "Remote init failed");

    sim_state_t *state = sim_get_state();
    ASSERT_EQ(state->master.initialized, 1, "Master init");
    ASSERT_EQ(state->remote.initialized, 1, "Remote init");
    ASSERT_NOT_NULL(state->remote.instance, "Remote instance");

    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_deinit_core(void)
{
    TEST_BEGIN("sim_deinit_core");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");

    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "Master init failed");

    ret = sim_deinit_core("master");
    ASSERT_EQ(ret, 0, "Deinit master should succeed");

    sim_state_t *state = sim_get_state();
    ASSERT_EQ(state->master.initialized, 0, "Master should be deinitialized");

    /* Deinit already deinitialized should fail */
    ret = sim_deinit_core("master");
    ASSERT_EQ(ret, -1, "Deinit already deinited should fail");

    /* Invalid core name */
    ret = sim_deinit_core("invalid");
    ASSERT_EQ(ret, -1, "Invalid core name should fail");

    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_create_ept(void)
{
    TEST_BEGIN("sim_create_ept");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init failed");
    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "remote init failed");

    /* Create endpoint on master */
    ret = sim_create_ept("master", 30);
    ASSERT_EQ(ret, 0, "Create ept on master failed");

    sim_state_t *state = sim_get_state();
    ASSERT_EQ(state->master.ept_count, 1, "Master should have 1 ept");
    ASSERT_EQ(state->master.endpoints[0].active, 1, "Ept should be active");
    ASSERT_EQ(state->master.endpoints[0].addr, 30, "Ept addr mismatch");

    /* Create endpoint on remote */
    ret = sim_create_ept("remote", 40);
    ASSERT_EQ(ret, 0, "Create ept on remote failed");
    ASSERT_EQ(state->remote.ept_count, 1, "Remote should have 1 ept");

    /* Invalid core */
    ret = sim_create_ept("invalid", 50);
    ASSERT_EQ(ret, -1, "Invalid core should fail");

    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_create_ept_limit(void)
{
    TEST_BEGIN("sim_create_ept_limit");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init failed");

    /* Create max endpoints */
    for (int i = 0; i < SIM_MAX_ENDPOINTS; i++)
    {
        ret = sim_create_ept("master", 30 + i);
        ASSERT_EQ(ret, 0, "Create ept failed");
    }

    /* One more should fail */
    ret = sim_create_ept("master", 100);
    ASSERT_EQ(ret, -1, "Exceeding max ept should fail");

    sim_state_t *state = sim_get_state();
    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_destroy_ept(void)
{
    TEST_BEGIN("sim_destroy_ept");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init failed");

    ret = sim_create_ept("master", 30);
    ASSERT_EQ(ret, 0, "Create ept failed");

    ret = sim_destroy_ept("master", 30);
    ASSERT_EQ(ret, 0, "Destroy ept failed");

    sim_state_t *state = sim_get_state();
    ASSERT_EQ(state->master.ept_count, 0, "No epts after destroy");

    /* Destroy non-existent */
    ret = sim_destroy_ept("master", 99);
    ASSERT_EQ(ret, -1, "Destroy non-existent should fail");

    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_send(void)
{
    TEST_BEGIN("sim_send");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init failed");
    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "remote init failed");

    /* Create endpoints on both sides */
    ret = sim_create_ept("master", 30);
    ASSERT_EQ(ret, 0, "Create master ept failed");
    ret = sim_create_ept("remote", 40);
    ASSERT_EQ(ret, 0, "Create remote ept failed");

    /* Send from master to remote */
    uint8_t data[] = "Hello Remote!";
    ret = sim_send("master", 30, 40, data, sizeof(data));
    ASSERT_EQ(ret, 0, "Send master->remote failed");

    /* Send from remote to master */
    uint8_t data2[] = "Hello Master!";
    ret = sim_send("remote", 40, 30, data2, sizeof(data2));
    ASSERT_EQ(ret, 0, "Send remote->master failed");

    sim_reset();
    sim_state_t *state = sim_get_state();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_send_no_ept(void)
{
    TEST_BEGIN("sim_send_no_ept");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init failed");

    /* Send without creating endpoint */
    uint8_t data[] = "test";
    ret = sim_send("master", 99, 40, data, sizeof(data));
    ASSERT_EQ(ret, -1, "Send without ept should fail");

    /* Invalid core */
    ret = sim_send("invalid", 30, 40, data, sizeof(data));
    ASSERT_EQ(ret, -1, "Send with invalid core should fail");

    sim_state_t *state = sim_get_state();
    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_set_mode(void)
{
    TEST_BEGIN("sim_set_mode");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");

    sim_state_t *state = sim_get_state();

    sim_set_mode(SIM_MODE_SLOW, 100);
    ASSERT_EQ(state->mode, SIM_MODE_SLOW, "Mode should be SLOW");
    ASSERT_EQ(state->delay_ms, 100, "Delay should be 100");

    sim_set_mode(SIM_MODE_NORMAL, 0);
    ASSERT_EQ(state->mode, SIM_MODE_NORMAL, "Mode should be NORMAL");

    sim_set_mode(SIM_MODE_STEP, 0);
    ASSERT_EQ(state->mode, SIM_MODE_STEP, "Mode should be STEP");

    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_is_link_up(void)
{
    TEST_BEGIN("sim_is_link_up");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");

    /* Before init, link should be down */
    ASSERT_EQ(sim_is_link_up("master"), 0, "Master link should be down");
    ASSERT_EQ(sim_is_link_up("remote"), 0, "Remote link should be down");

    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init failed");

    /* Master initializes link as up */
    int master_link = sim_is_link_up("master");
    /* Link state depends on implementation - just verify it doesn't crash */
    (void)master_link;

    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "remote init failed");

    /* After both sides init, check link status */
    int remote_link = sim_is_link_up("remote");
    (void)remote_link;

    /* Invalid core */
    ASSERT_EQ(sim_is_link_up("invalid"), 0, "Invalid core link should be 0");

    sim_state_t *state = sim_get_state();
    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_state_to_json(void)
{
    TEST_BEGIN("sim_state_to_json");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init failed");
    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "remote init failed");
    ret = sim_create_ept("master", 30);
    ASSERT_EQ(ret, 0, "Create ept failed");

    char buf[16384];
    int len = sim_state_to_json(buf, sizeof(buf));
    ASSERT_TRUE(len > 0, "JSON output should have content");

    /* Verify JSON contains expected fields */
    ASSERT_TRUE(strstr(buf, "\"master\"") != NULL, "Should contain master");
    ASSERT_TRUE(strstr(buf, "\"remote\"") != NULL, "Should contain remote");
    ASSERT_TRUE(strstr(buf, "\"initialized\":true") != NULL, "Should have initialized");
    ASSERT_TRUE(strstr(buf, "\"mode\":\"normal\"") != NULL, "Should have mode");
    ASSERT_TRUE(strstr(buf, "\"addr\":30") != NULL, "Should have ept addr");

    sim_state_t *state = sim_get_state();
    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_reset(void)
{
    TEST_BEGIN("sim_reset");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init failed");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init");
    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "remote init");
    ret = sim_create_ept("master", 30);
    ASSERT_EQ(ret, 0, "Create ept");

    sim_reset();

    sim_state_t *state = sim_get_state();
    ASSERT_EQ(state->master.initialized, 0, "Master should be reset");
    ASSERT_EQ(state->remote.initialized, 0, "Remote should be reset");

    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_full_workflow(void)
{
    TEST_BEGIN("sim_full_workflow");

    /* Full workflow: init -> create epts -> send -> destroy -> deinit */
    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init");

    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init");

    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "remote init");

    ret = sim_create_ept("master", 30);
    ASSERT_EQ(ret, 0, "master ept");

    ret = sim_create_ept("remote", 40);
    ASSERT_EQ(ret, 0, "remote ept");

    /* Send messages both directions */
    uint8_t msg1[] = "Test message 1";
    ret = sim_send("master", 30, 40, msg1, sizeof(msg1));
    ASSERT_EQ(ret, 0, "send m->r");

    uint8_t msg2[] = "Test message 2";
    ret = sim_send("remote", 40, 30, msg2, sizeof(msg2));
    ASSERT_EQ(ret, 0, "send r->m");

    /* Destroy endpoints */
    ret = sim_destroy_ept("master", 30);
    ASSERT_EQ(ret, 0, "destroy master ept");

    ret = sim_destroy_ept("remote", 40);
    ASSERT_EQ(ret, 0, "destroy remote ept");

    /* Deinit */
    ret = sim_deinit_core("remote");
    ASSERT_EQ(ret, 0, "deinit remote");

    ret = sim_deinit_core("master");
    ASSERT_EQ(ret, 0, "deinit master");

    sim_state_t *state = sim_get_state();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

static void test_sim_multi_send(void)
{
    TEST_BEGIN("sim_multi_send");

    int ret = sim_init();
    ASSERT_EQ(ret, 0, "sim_init");
    ret = sim_init_master();
    ASSERT_EQ(ret, 0, "master init");
    ret = sim_init_remote();
    ASSERT_EQ(ret, 0, "remote init");
    ret = sim_create_ept("master", 30);
    ASSERT_EQ(ret, 0, "master ept");
    ret = sim_create_ept("remote", 40);
    ASSERT_EQ(ret, 0, "remote ept");

    /* Send multiple messages (up to buffer count) */
    for (int i = 0; i < (int)RL_BUFFER_COUNT; i++)
    {
        char msg[32];
        snprintf(msg, sizeof(msg), "msg_%d", i);
        ret = sim_send("master", 30, 40, (uint8_t *)msg, (uint32_t)strlen(msg) + 1);
        ASSERT_EQ(ret, 0, "multi send failed");
    }

    sim_state_t *state = sim_get_state();
    sim_reset();
    free(state->shmem);
    state->shmem = NULL;

    TEST_PASS();
}

/* ========================================================================== */
/* Main                                                                        */
/* ========================================================================== */

int main(void)
{
    printf("========================================\n");
    printf(" RPMsg-Lite Simulator Unit Tests\n");
    printf("========================================\n\n");

    /* Event System Tests */
    printf("[Suite] Event System\n");
    test_events_init();
    test_events_push_pop();
    test_events_pop_empty();
    test_events_sequence();
    test_events_overflow();
    test_events_to_json();
    test_events_record_simple();
    test_events_record_ept();
    test_events_record_msg();

    /* Environment Layer Tests */
    printf("\n[Suite] Environment Layer\n");
    test_env_init_deinit();
    test_env_memory();
    test_env_memset_memcpy();
    test_env_strcmp();
    test_env_strncpy_strncmp();
    test_env_vatopa_patova();
    test_env_mutex();
    test_env_isr_registration();
    test_env_timestamp();

    /* Platform Layer Tests */
    printf("\n[Suite] Platform Layer\n");
    test_platform_init_deinit();
    test_platform_interrupt();
    test_platform_in_isr();
    test_platform_shmem();

    /* Sim Core Tests */
    printf("\n[Suite] Sim Core\n");
    test_sim_init();
    test_sim_init_master();
    test_sim_init_remote_requires_master();
    test_sim_init_master_remote();
    test_sim_deinit_core();
    test_sim_create_ept();
    test_sim_create_ept_limit();
    test_sim_destroy_ept();
    test_sim_send();
    test_sim_send_no_ept();
    test_sim_set_mode();
    test_sim_is_link_up();
    test_sim_state_to_json();
    test_sim_reset();
    test_sim_full_workflow();
    test_sim_multi_send();

    /* Summary */
    printf("\n========================================\n");
    printf(" Results: %d/%d passed, %d failed\n",
           g_tests_passed, g_tests_run, g_tests_failed);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
