#include <CUnit/Basic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

/* Heartbeat test component (TDD demo) */
/* Requirements trace: */
/* R1: Heartbeat interval 100ms (leader sends) */
/* R2: First heartbeat prints timestamp */
/* R3: Reconnect attempt every 500ms when heartbeat missing */
/* R4: After 10 attempts / 5000ms -> disconnected */
/* R5: Reconnect notice printed once */

#define HEARTBEAT_INTERVAL_MS 100
#define HEARTBEAT_RECONNECT_MS 500
#define HEARTBEAT_MAX_ATTEMPTS 10

typedef struct {
    uint64_t last_hb_ms;
    uint64_t last_attempt_ms;
    int attempts;
    int gave_up;
    int notice_printed;
    int heartbeat_printed;
    time_t last_hb_wall;
} HeartbeatState;

typedef struct {
    int print_first;
    int print_reconnect;
    int disconnected;
} HeartbeatAction;

/* -------- Tests (written first for TDD) -------- */

static void test_ut_first_heartbeat_prints_once(void) {
    HeartbeatState st = {0};
    HeartbeatAction a1, a2;
    a1 = (HeartbeatAction){0};
    a2 = (HeartbeatAction){0};
    st.last_hb_ms = 0;
    st.heartbeat_printed = 0;
    // first heartbeat should request print
    extern HeartbeatAction hb_on_heartbeat(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    a1 = hb_on_heartbeat(&st, 1000, 1700000000);
    CU_ASSERT_EQUAL(a1.print_first, 1);
    // second heartbeat should not print again without reset
    a2 = hb_on_heartbeat(&st, 1100, 1700000100);
    CU_ASSERT_EQUAL(a2.print_first, 0);
}

static void test_ut_no_reconnect_before_500ms(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1000, 1700000000);
    HeartbeatAction a = hb_tick(&st, 1400);
    CU_ASSERT_EQUAL(a.print_reconnect, 0);
    CU_ASSERT_EQUAL(a.disconnected, 0);
    CU_ASSERT_EQUAL(st.attempts, 0);
}

static void test_ut_reconnect_starts_at_500ms(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1000, 1700000000);
    HeartbeatAction a = hb_tick(&st, 1500);
    CU_ASSERT_EQUAL(a.print_reconnect, 1);
    CU_ASSERT_EQUAL(st.attempts, 1);
}

static void test_ut_disconnect_after_10_attempts(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1, 1700000000);
    // Simulate 10 attempts, each 500ms apart
    for (int i = 1; i <= HEARTBEAT_MAX_ATTEMPTS; i++) {
        HeartbeatAction a = hb_tick(&st, 1 + i * HEARTBEAT_RECONNECT_MS);
        if (i < HEARTBEAT_MAX_ATTEMPTS) {
            CU_ASSERT_EQUAL(a.disconnected, 0);
        } else {
            CU_ASSERT_EQUAL(a.disconnected, 1);
        }
    }
}

static void test_ut_reset_on_heartbeat(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    extern HeartbeatAction hb_on_heartbeat(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    hb_init(&st, 1000, 1700000000);
    (void)hb_tick(&st, 1500);
    CU_ASSERT(st.attempts > 0);
    hb_on_heartbeat(&st, 1600, 1700000100);
    CU_ASSERT_EQUAL(st.attempts, 0);
    CU_ASSERT_EQUAL(st.notice_printed, 0);
}

/* Defect tests (edge cases) */

static void test_dt_time_goes_back_no_action(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1000, 1700000000);
    HeartbeatAction a = hb_tick(&st, 900);
    CU_ASSERT_EQUAL(a.print_reconnect, 0);
    CU_ASSERT_EQUAL(a.disconnected, 0);
}

static void test_dt_notice_only_once(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1, 1700000000);
    HeartbeatAction a1 = hb_tick(&st, 1 + 500);
    HeartbeatAction a2 = hb_tick(&st, 1 + 1000);
    CU_ASSERT_EQUAL(a1.print_reconnect, 1);
    CU_ASSERT_EQUAL(a2.print_reconnect, 0);
}

static void test_dt_gave_up_stops_actions(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1, 1700000000);
    st.gave_up = 1;
    HeartbeatAction a = hb_tick(&st, 1 + 5000);
    CU_ASSERT_EQUAL(a.print_reconnect, 0);
    CU_ASSERT_EQUAL(a.disconnected, 0);
}

static void test_dt_attempt_clock_updates(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1, 1700000000);
    (void)hb_tick(&st, 1 + 500);
    CU_ASSERT_EQUAL(st.last_attempt_ms, 1 + 500);
}

static void test_dt_disconnect_exactly_on_attempt_10(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1, 1700000000);
    HeartbeatAction a = {0};
    for (int i = 1; i <= HEARTBEAT_MAX_ATTEMPTS; i++) {
        a = hb_tick(&st, 1 + i * HEARTBEAT_RECONNECT_MS);
    }
    CU_ASSERT_EQUAL(a.disconnected, 1);
    CU_ASSERT_EQUAL(st.attempts, HEARTBEAT_MAX_ATTEMPTS);
}

/* Component / Interface tests */

static void test_ct_leader_alive_no_disconnect(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_on_heartbeat(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1, 1700000000);
    for (int t = 1; t <= 1001; t += HEARTBEAT_INTERVAL_MS) {
        (void)hb_on_heartbeat(&st, t, 1700000000 + t / 1000);
        HeartbeatAction a = hb_tick(&st, t);
        CU_ASSERT_EQUAL(a.disconnected, 0);
    }
}

static void test_ct_leader_stops_disconnects(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    hb_init(&st, 1, 1700000000);
    HeartbeatAction a = {0};
    for (int i = 1; i <= HEARTBEAT_MAX_ATTEMPTS; i++) {
        a = hb_tick(&st, 1 + i * HEARTBEAT_RECONNECT_MS);
    }
    CU_ASSERT_EQUAL(a.disconnected, 1);
}

static void test_ct_recover_before_disconnect(void) {
    HeartbeatState st = {0};
    extern void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    extern HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms);
    extern HeartbeatAction hb_on_heartbeat(HeartbeatState *st, uint64_t now_ms, time_t now_wall);
    hb_init(&st, 1, 1700000000);
    (void)hb_tick(&st, 1 + 500);
    (void)hb_tick(&st, 1 + 1000);
    CU_ASSERT(st.attempts == 2);
    (void)hb_on_heartbeat(&st, 1 + 1100, 1700000011);
    CU_ASSERT_EQUAL(st.attempts, 0);
    CU_ASSERT_EQUAL(st.gave_up, 0);
}

/* -------- Component implementation (after tests for TDD) -------- */

void hb_init(HeartbeatState *st, uint64_t now_ms, time_t now_wall) {
    memset(st, 0, sizeof(*st));
    st->last_hb_ms = now_ms;
    st->last_attempt_ms = now_ms;
    st->last_hb_wall = now_wall;
}

HeartbeatAction hb_on_heartbeat(HeartbeatState *st, uint64_t now_ms, time_t now_wall) {
    HeartbeatAction a = {0};
    st->last_hb_ms = now_ms;
    st->last_attempt_ms = now_ms;
    st->last_hb_wall = now_wall;
    st->attempts = 0;
    st->gave_up = 0;
    st->notice_printed = 0;
    if (!st->heartbeat_printed) {
        st->heartbeat_printed = 1;
        a.print_first = 1;
    }
    return a;
}

HeartbeatAction hb_tick(HeartbeatState *st, uint64_t now_ms) {
    HeartbeatAction a = {0};
    if (st->last_hb_ms == 0) {
        return a;
    }
    if (now_ms < st->last_hb_ms) {
        return a;
    }
    if (now_ms - st->last_hb_ms < HEARTBEAT_RECONNECT_MS) {
        return a;
    }
    if (st->gave_up) {
        return a;
    }
    if (now_ms - st->last_attempt_ms >= HEARTBEAT_RECONNECT_MS) {
        if (!st->notice_printed) {
            a.print_reconnect = 1;
            st->notice_printed = 1;
        }
        st->attempts++;
        st->last_attempt_ms = now_ms;
        st->heartbeat_printed = 0;
        if (st->attempts >= HEARTBEAT_MAX_ATTEMPTS) {
            st->gave_up = 1;
            a.disconnected = 1;
        }
    }
    return a;
}

int main(void) {
    if (CU_initialize_registry() != CUE_SUCCESS) {
        return CU_get_error();
    }
    CU_pSuite suite = CU_add_suite("Heartbeat_Tests", 0, 0);
    if (!suite) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    // Unit tests (validation)
    CU_add_test(suite, "UT1 First heartbeat prints once", test_ut_first_heartbeat_prints_once);
    CU_add_test(suite, "UT2 No reconnect before 500ms", test_ut_no_reconnect_before_500ms);
    CU_add_test(suite, "UT3 Reconnect starts at 500ms", test_ut_reconnect_starts_at_500ms);
    CU_add_test(suite, "UT4 Disconnect after 10 attempts", test_ut_disconnect_after_10_attempts);
    CU_add_test(suite, "UT5 Reset on heartbeat", test_ut_reset_on_heartbeat);

    // Defect tests
    CU_add_test(suite, "DT1 Time goes back => no action", test_dt_time_goes_back_no_action);
    CU_add_test(suite, "DT2 Reconnect notice once", test_dt_notice_only_once);
    CU_add_test(suite, "DT3 Gave up => no actions", test_dt_gave_up_stops_actions);
    CU_add_test(suite, "DT4 Attempt clock updates", test_dt_attempt_clock_updates);
    CU_add_test(suite, "DT5 Disconnect exactly attempt 10", test_dt_disconnect_exactly_on_attempt_10);

    // Component / interface tests
    CU_add_test(suite, "CT1 Leader alive => no disconnect", test_ct_leader_alive_no_disconnect);
    CU_add_test(suite, "CT2 Leader stops => disconnect", test_ct_leader_stops_disconnects);
    CU_add_test(suite, "CT3 Recover before disconnect", test_ct_recover_before_disconnect);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();
    return CU_get_error();
}
