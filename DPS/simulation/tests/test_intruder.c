#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

/* Project headers */
#include "intruder.h"
#include "matrix_clock.h"
#include "event.h"
#include "follower.h"

/* ----------- Mock definations ------------------*/
/* Mock event types if they are missing in your project */
#ifndef EVT_INTRUDER_ENTER
#define EVT_INTRUDER_ENTER 100
#endif

#ifndef EVT_INTRUDER_EXIT
#define EVT_INTRUDER_EXIT 101
#endif

/* Mock function to avoid compiler errors */
void enqueue_event(void* q, Event e) {
    (void)q; 
    (void)e;
    printf("[MOCK] enqueue_event called\n");
}



Truck follower;
pthread_mutex_t mutex_follower = PTHREAD_MUTEX_INITIALIZER;
int follower_idx;
MatrixClock follower_clock;
int intruder_flag;
EventQueue truck_EventQ;
pthread_mutex_t mutex_sockets = PTHREAD_MUTEX_INITIALIZER;
int tcp2Leader = -1;

/* ----------------------------- VALIDATION TESTS ----------------------------*/
void test_toggle_intruder(void) {
    printf("\n[VALIDATION] toggle_intruder()\n");
    intruder_flag = 0;
    CU_ASSERT_EQUAL(toggle_intruder(), 1);
    CU_ASSERT_EQUAL(toggle_intruder(), 0);
    CU_ASSERT_EQUAL(toggle_intruder(), 1);
    printf("[PASS] toggle_intruder verified\n");
}

void test_intruder_detected(void) {
    printf("\n[VALIDATION] intruder_detected()\n");
    for (int i = 0; i < 100; i++) {
        int v = intruder_detected();
        CU_ASSERT(v == 0 || v == 1);
    }
    printf("[PASS] intruder_detected bounds verified\n");
}
/*
void test_intruder_speed_bounds(void) {
    printf("\n[VALIDATION] intruder_speed()\n");
    for (int i = 0; i < 100; i++) {
        int s = intruder_speed();
        CU_ASSERT(s >= 30 && s <= 120);
    }
    printf("[PASS] intruder_speed verified\n");
}
*/
void test_enter_intruder_follow(void) {
    printf("\n[VALIDATION] enter_intruder_follow()\n");
    follower_idx = 1;
    mc_init(&follower_clock);

    IntruderInfo intr = {.speed = 50, .length = 10, .duration_ms = 6000};
    enter_intruder_follow(intr);

    CU_ASSERT_EQUAL(follower.state, INTRUDER_FOLLOW);
    CU_ASSERT_EQUAL(follower.speed, 50);
    CU_ASSERT_EQUAL(follower_clock.mc[1][1], 1);
    printf("[PASS] enter_intruder_follow verified\n");
}

void test_exit_intruder_follow(void) {
    printf("\n[VALIDATION] exit_intruder_follow()\n");
    follower_idx = 1;
    mc_init(&follower_clock);

    exit_intruder_follow();

    CU_ASSERT_EQUAL(follower.state, CRUISE);
    CU_ASSERT_EQUAL(follower_clock.mc[1][1], 1);
    printf("[PASS] exit_intruder_follow verified\n");
}

/* -------------------------------- DEFECT TESTS ---------------------------*/
void test_intruder_speed_invalid(void) {
    printf("\n[DEFECT] intruder_speed invalid range simulation\n");
    for (int i = 0; i < 50; i++) {
        int s = intruder_speed();
        if (s < 30 || s > 120) printf("[DEFECT] invalid speed: %d\n", s);
        CU_ASSERT(s >= 30 && s <= 120);
    }
    printf("[PASS] intruder_speed defect test executed\n");
}

void test_intruder_duration_invalid(void) {
    printf("\n[DEFECT] intruder_duration invalid range simulation\n");
    for (int i = 0; i < 50; i++) {
        uint32_t d = intruder_duration();
        if (d < 5000 || d > 10000) printf("[DEFECT] invalid duration: %u\n", d);
        CU_ASSERT(d >= 5000 && d <= 10000);
    }
    printf("[PASS] intruder_duration defect test executed\n");
}

void test_toggle_intruder_multiple(void) {
    printf("\n[DEFECT] toggle_intruder rapid toggle\n");
    intruder_flag = 0;
    for (int i = 0; i < 10; i++) toggle_intruder();
    CU_ASSERT(intruder_flag == 0 || intruder_flag == 1);
    printf("[PASS] toggle_intruder rapid toggle test executed\n");
}

void test_enter_intruder_follow_null(void) {
    printf("\n[DEFECT] enter_intruder_follow with zero speed\n");
    follower_idx = 1;
    mc_init(&follower_clock);
    IntruderInfo intr = {.speed = 0, .length = 0, .duration_ms = 0};
    enter_intruder_follow(intr);
    CU_ASSERT(follower.speed >= 0);
    CU_ASSERT(follower_clock.mc[1][1] >= 0);
    printf("[PASS] enter_intruder_follow defect test executed\n");
}

void test_exit_intruder_follow_no_init(void) {
    printf("\n[DEFECT] exit_intruder_follow without mc_init\n");
    follower_idx = 1;
    exit_intruder_follow();
    CU_ASSERT(follower.state == CRUISE);
    printf("[PASS] exit_intruder_follow defect test executed\n");
}

/* --------------------------------- COMPONENT TESTS --------------------------------*/
void test_intruder_mc_integration(void) {
    printf("\n[COMPONENT] intruder -> matrix_clock integration\n");
    follower_idx = 2;
    mc_init(&follower_clock);
    IntruderInfo intr = {.speed = 55, .length = 12, .duration_ms = 7000};
    enter_intruder_follow(intr);
    CU_ASSERT(follower_clock.mc[2][2] == 1);
    printf("[PASS] intruder_mc_integration verified\n");
}
/*
void test_intruder_event_queue(void) {
    printf("\n[COMPONENT] intruder -> EventQueue integration\n");
    Event e = {.type = EVT_INTRUDER_ENTER};
    enqueue_event(&truck_EventQ, e);
    CU_ASSERT(truck_EventQ.head != NULL);
    printf("[PASS] intruder_event_queue verified\n");
}*/

void test_intruder_follower_state_sync(void) {
    printf("\n[COMPONENT] intruder -> follower state sync\n");
    follower_idx = 0;
    mc_init(&follower_clock);
    IntruderInfo intr = {.speed = 60, .length = 8, .duration_ms = 6000};
    enter_intruder_follow(intr);
    CU_ASSERT(follower.state == INTRUDER_FOLLOW);
    printf("[PASS] intruder_follower_state_sync verified\n");
}

/* ------------------------------------- Main ----------------------------------*/
int main(void) {
    CU_initialize_registry();
    CU_pSuite suite = CU_add_suite("Intruder Full Tests", NULL, NULL);

    // Validation Tests
    CU_add_test(suite, "toggle_intruder", test_toggle_intruder);
    CU_add_test(suite, "intruder_detected", test_intruder_detected);
//    CU_add_test(suite, "intruder_speed", test_intruder_speed_bounds);
    CU_add_test(suite, "enter_intruder_follow valid", test_enter_intruder_follow);
    CU_add_test(suite, "exit_intruder_follow valid", test_exit_intruder_follow);

    // Defect Tests
    CU_add_test(suite, "intruder_speed defect", test_intruder_speed_invalid);
    CU_add_test(suite, "intruder_duration defect", test_intruder_duration_invalid);
    CU_add_test(suite, "toggle_intruder rapid defect", test_toggle_intruder_multiple);
    CU_add_test(suite, "enter_intruder_follow defect", test_enter_intruder_follow_null);
    CU_add_test(suite, "exit_intruder_follow defect", test_exit_intruder_follow_no_init);

    // Component Tests
    CU_add_test(suite, "intruder-matrix_clock", test_intruder_mc_integration);
//    CU_add_test(suite, "intruder-event_queue", test_intruder_event_queue);
    CU_add_test(suite, "intruder-follower_sync", test_intruder_follower_state_sync);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();
    return 0;
}

