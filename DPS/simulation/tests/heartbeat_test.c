#include <CUnit/Basic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Heartbeat tests aligned with current follower behavior:
   - Any leader TCP message updates "last seen"
   - Watchdog checks for stale leader messages
   - Timeout drives follower to STOPPED (safe state)
   - Leader messages resuming move follower back to CRUISE
   - Timeouts are ignored while PLATOONING
*/

#define LEADER_RX_TIMEOUT_MS 2000

typedef enum {
    PLATOONING = 0,
    CRUISE = 1,
    INTRUDER_FOLLOW = 2,
    STOPPED = 3
} HbState;

typedef struct {
    uint64_t last_msg_ms;
    int timeout_emitted;
    HbState state;
} HeartbeatState;

typedef struct {
    int timeout_event;
    int resumed;
} HeartbeatAction;

static void hb_init(HeartbeatState *st, uint64_t now_ms, HbState initial_state) {
    memset(st, 0, sizeof(*st));
    st->last_msg_ms = now_ms;
    st->state = initial_state;
    st->timeout_emitted = 0;
}

static HeartbeatAction hb_on_leader_msg(HeartbeatState *st, uint64_t now_ms) {
    HeartbeatAction a = {0};
    st->last_msg_ms = now_ms;
    st->timeout_emitted = 0;
    if (st->state == STOPPED) {
        st->state = CRUISE;
        a.resumed = 1;
    }
    return a;
}

static HeartbeatAction hb_watchdog_tick(HeartbeatState *st, uint64_t now_ms) {
    HeartbeatAction a = {0};

    if (st->state == PLATOONING) {
        return a; /* Ignore timeouts during formation */
    }
    if (now_ms < st->last_msg_ms) {
        return a; /* Clock went backwards */
    }
    if (st->timeout_emitted) {
        return a; /* Only emit once */
    }
    if ((now_ms - st->last_msg_ms) > LEADER_RX_TIMEOUT_MS) {
        st->timeout_emitted = 1;
        a.timeout_event = 1;
        if (st->state == CRUISE || st->state == INTRUDER_FOLLOW) {
            st->state = STOPPED;
        }
    }
    return a;
}

/* --- Tests --- */

static void test_no_timeout_during_platooning(void) {
    HeartbeatState st;
    hb_init(&st, 1000, PLATOONING);
    HeartbeatAction a = hb_watchdog_tick(&st, 5000);
    CU_ASSERT_EQUAL(a.timeout_event, 0);
    CU_ASSERT_EQUAL(st.state, PLATOONING);
}

static void test_timeout_in_cruise(void) {
    HeartbeatState st;
    hb_init(&st, 1000, CRUISE);
    HeartbeatAction a = hb_watchdog_tick(&st, 1000 + LEADER_RX_TIMEOUT_MS + 1);
    CU_ASSERT_EQUAL(a.timeout_event, 1);
    CU_ASSERT_EQUAL(st.state, STOPPED);
}

static void test_timeout_only_once(void) {
    HeartbeatState st;
    hb_init(&st, 1000, CRUISE);
    HeartbeatAction a1 = hb_watchdog_tick(&st, 1000 + LEADER_RX_TIMEOUT_MS + 1);
    HeartbeatAction a2 = hb_watchdog_tick(&st, 1000 + LEADER_RX_TIMEOUT_MS + 500);
    CU_ASSERT_EQUAL(a1.timeout_event, 1);
    CU_ASSERT_EQUAL(a2.timeout_event, 0);
    CU_ASSERT_EQUAL(st.timeout_emitted, 1);
}

static void test_resume_on_leader_msg(void) {
    HeartbeatState st;
    hb_init(&st, 1000, CRUISE);
    (void)hb_watchdog_tick(&st, 1000 + LEADER_RX_TIMEOUT_MS + 1);
    CU_ASSERT_EQUAL(st.state, STOPPED);
    HeartbeatAction a = hb_on_leader_msg(&st, 4000);
    CU_ASSERT_EQUAL(a.resumed, 1);
    CU_ASSERT_EQUAL(st.state, CRUISE);
    CU_ASSERT_EQUAL(st.timeout_emitted, 0);
}

static void test_message_updates_prevent_timeout(void) {
    HeartbeatState st;
    hb_init(&st, 1000, CRUISE);
    (void)hb_on_leader_msg(&st, 1400);
    HeartbeatAction a = hb_watchdog_tick(&st, 1400 + (LEADER_RX_TIMEOUT_MS - 100));
    CU_ASSERT_EQUAL(a.timeout_event, 0);
    CU_ASSERT_EQUAL(st.state, CRUISE);
}

static void test_timeout_in_intruder_follow(void) {
    HeartbeatState st;
    hb_init(&st, 1000, INTRUDER_FOLLOW);
    HeartbeatAction a = hb_watchdog_tick(&st, 1000 + LEADER_RX_TIMEOUT_MS + 1);
    CU_ASSERT_EQUAL(a.timeout_event, 1);
    CU_ASSERT_EQUAL(st.state, STOPPED);
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

    CU_add_test(suite, "HB1 No timeout during platooning", test_no_timeout_during_platooning);
    CU_add_test(suite, "HB2 Timeout in cruise", test_timeout_in_cruise);
    CU_add_test(suite, "HB3 Timeout only once", test_timeout_only_once);
    CU_add_test(suite, "HB4 Resume on leader message", test_resume_on_leader_msg);
    CU_add_test(suite, "HB5 Message prevents timeout", test_message_updates_prevent_timeout);
    CU_add_test(suite, "HB6 Timeout in intruder follow", test_timeout_in_intruder_follow);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();
    return CU_get_error();
}
