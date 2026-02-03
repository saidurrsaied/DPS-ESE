#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include <stdio.h>
#include "cruise_control.h"
#include "matrix_clock.h"

extern int follower_idx;
extern MatrixClock follower_clock;

#ifndef MAX_TURN_EVENTS
#define MAX_TURN_EVENTS 10
#endif


/* -------------------- Validation Tests -------------------- */
void test_cruise_control_calculate_speed(void)
{
    printf("\n[VALIDATION] cruise_control_calculate_speed()\n");

    float leader_speed = 50.0f;

    float new_speed = cruise_control_calculate_speed(
        40.0f,   // current_speed
        60.0f,   // front_pos_x
        0.0f,    // front_pos_y
        50.0f,   // front_speed
        leader_speed,
        50.0f,   // my_x
        0.0f     // my_y
    );

    CU_ASSERT(new_speed > 0);
    CU_ASSERT(new_speed <= leader_speed + 25.0f);
    printf("[PASS] cruise_control_calculate_speed verified\n");
}

void test_turn_queue_init_and_push(void)
{
    printf("\n[VALIDATION] turn_queue_init_and_push()\n");
    TurnQueue q;
    turn_queue_init(&q);

    CU_ASSERT_EQUAL(q.count, 0);
    CU_ASSERT_EQUAL(q.head, 0);
    CU_ASSERT_EQUAL(q.tail, 0);

    int res = turn_queue_push(&q, 10.0f, 20.0f, NORTH);
    CU_ASSERT_EQUAL(res, 0);
    CU_ASSERT_EQUAL(q.count, 1);
    CU_ASSERT_EQUAL(q.head, 0);
    CU_ASSERT_EQUAL(q.tail, 1);

    CU_ASSERT_DOUBLE_EQUAL(q.events[0].x, 10.0f, 0.001);
    CU_ASSERT_DOUBLE_EQUAL(q.events[0].y, 20.0f, 0.001);
    CU_ASSERT_EQUAL(q.events[0].dir, NORTH);

    printf("[PASS] turn_queue_init_and_push verified\n");
}

void test_turning_check_and_update(void)
{
    printf("\n[VALIDATION] turning_check_and_update()\n");
    TurnQueue q;
    turn_queue_init(&q);
    turn_queue_push(&q, 5.0f, 5.0f, EAST);

    DIRECTION out_dir;
    float out_x, out_y;

    follower_idx = 1;
    mc_init(&follower_clock);

    int turned = turning_check_and_update(&q, 5.0f, 5.0f, NORTH, &out_dir, &out_x, &out_y, follower_idx);

    CU_ASSERT_EQUAL(turned, 1);
    CU_ASSERT_EQUAL(out_dir, EAST);
    CU_ASSERT_DOUBLE_EQUAL(out_x, 5.0f, 0.001);
    CU_ASSERT_DOUBLE_EQUAL(out_y, 5.0f, 0.001);
    CU_ASSERT_EQUAL(follower_clock.mc[follower_idx][follower_idx], 1);
    CU_ASSERT_EQUAL(q.count, 0);

    printf("[PASS] turning_check_and_update verified\n");
}

void test_cruise_control_following_distance(void)
{
    printf("\n[VALIDATION] cruise_control maintains safe distance\n");
    float new_speed = cruise_control_calculate_speed(
        50.0f, 70.0f, 0.0f, 60.0f, 60.0f, 50.0f, 0.0f
    );
    CU_ASSERT(new_speed < 60.0f); // should slow down if too close
    printf("[PASS] following distance validation verified\n");
}

void test_cruise_control_max_speed(void)
{
    printf("\n[VALIDATION] cruise_control respects max speed\n");
    float new_speed = cruise_control_calculate_speed(
        80.0f, 100.0f, 0.0f, 80.0f, 80.0f, 50.0f, 0.0f
    );
    CU_ASSERT(new_speed <= 105.0f); // leader_speed + 25
    printf("[PASS] max speed validation verified\n");
}

/* -------------------- Defect Tests -------------------- */
void test_cruise_control_negative_speed(void)
{
    printf("\n[DEFECT] cruise_control negative speed simulation\n");
    float speed = cruise_control_calculate_speed(-10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    CU_ASSERT(speed >= 0);
    printf("[PASS] negative speed defect test executed\n");
}

void test_turn_queue_push_overflow(void)
{
    printf("\n[DEFECT] turn_queue push overflow simulation\n");
    TurnQueue q;
    turn_queue_init(&q);
    for(int i=0;i<MAX_TURN_EVENTS;i++) turn_queue_push(&q, 0,0,NORTH);
    int res = turn_queue_push(&q, 1,1,SOUTH);
    CU_ASSERT(res != 0);
    printf("[PASS] queue overflow defect test executed\n");
}

void test_turning_check_no_events(void)
{
    printf("\n[DEFECT] turning_check with empty queue\n");
    TurnQueue q;
    turn_queue_init(&q);
    DIRECTION dir; float x, y;
    int turned = turning_check_and_update(&q, 0,0,NORTH,&dir,&x,&y,1);
    CU_ASSERT_EQUAL(turned, 0);
    printf("[PASS] turning_check empty queue defect test executed\n");
}

void test_cruise_control_high_speed(void)
{
    printf("\n[DEFECT] cruise_control extremely high speed\n");
    float speed = cruise_control_calculate_speed(1000.0f, 2000.0f,0.0f,1000.0f,1000.0f,0.0f,0.0f);
    CU_ASSERT(speed <= 1025.0f);
    printf("[PASS] extremely high speed defect test executed\n");
}

void test_cruise_control_zero_distance(void)
{
    printf("\n[DEFECT] cruise_control zero distance simulation\n");
    float speed = cruise_control_calculate_speed(50.0f, 50.0f, 0.0f, 50.0f, 50.0f, 50.0f,0.0f);
    CU_ASSERT(speed >= 0);
    printf("[PASS] zero distance defect test executed\n");
}

/* -------------------- Component Tests -------------------- */
void test_component_matrix_clock_integration(void)
{
    printf("\n[COMPONENT] Cruise Control -> Matrix Clock integration\n");
    follower_idx = 1;
    mc_init(&follower_clock);
    turning_check_and_update(&(TurnQueue){0}, 0,0,NORTH,NULL,NULL,NULL,follower_idx);
    CU_ASSERT_EQUAL(follower_clock.mc[follower_idx][follower_idx],1);
    printf("[PASS] matrix clock component test executed\n");
}

void test_component_turn_queue_interface(void)
{
    printf("\n[COMPONENT] Cruise Control -> TurnQueue interface\n");
    TurnQueue q;
    turn_queue_init(&q);
    turn_queue_push(&q, 1,2,NORTH);
    CU_ASSERT_EQUAL(q.count,1);
    printf("[PASS] turn queue component test executed\n");
}

void test_component_speed_calculation_interface(void)
{
    printf("\n[COMPONENT] Cruise Control speed calculation interface\n");
    float new_speed = cruise_control_calculate_speed(30.0f,40.0f,0.0f,50.0f,50.0f,30.0f,0.0f);
    CU_ASSERT(new_speed > 0);
    printf("[PASS] speed calculation component test executed\n");
}

/* -------------------- Main Test Runner -------------------- */
int main(void)
{
    CU_initialize_registry();
    CU_pSuite suite = CU_add_suite("Cruise Control Full Tests", NULL, NULL);

    // Validation Tests
    CU_add_test(suite, "cruise_control_calculate_speed", test_cruise_control_calculate_speed);
    CU_add_test(suite, "turn_queue_init_and_push", test_turn_queue_init_and_push);
    CU_add_test(suite, "turning_check_and_update", test_turning_check_and_update);
    CU_add_test(suite, "following_distance_validation", test_cruise_control_following_distance);
    CU_add_test(suite, "max_speed_validation", test_cruise_control_max_speed);

    // Defect Tests
    CU_add_test(suite, "negative_speed_defect", test_cruise_control_negative_speed);
    CU_add_test(suite, "queue_overflow_defect", test_turn_queue_push_overflow);
    CU_add_test(suite, "turning_empty_queue_defect", test_turning_check_no_events);
    CU_add_test(suite, "extremely_high_speed_defect", test_cruise_control_high_speed);
    CU_add_test(suite, "zero_distance_defect", test_cruise_control_zero_distance);

    // Component / Interface Tests
    CU_add_test(suite, "matrix_clock_component", test_component_matrix_clock_integration);
    CU_add_test(suite, "turn_queue_component", test_component_turn_queue_interface);
    CU_add_test(suite, "speed_calc_component", test_component_speed_calculation_interface);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();

    return 0;
}

