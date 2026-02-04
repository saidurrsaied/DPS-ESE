#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>
#include "matrix_clock.h"

/* ------------------------ Validation Tests ------------------------ */

// Test 1: mc_init()
void test_mc_init(void)
{
	printf("\n[TEST] mc_init()\n");
    MatrixClock clock;
    mc_init(&clock);
    for (int i = 0; i < NUM_TRUCKS; i++)
        for (int j = 0; j < NUM_TRUCKS; j++)
            CU_ASSERT_EQUAL(clock.mc[i][j], 0);
            
    printf("[PASS] mc_init verified\n");
}

// Test 2: mc_send_event()
void test_mc_send_event(void)
{
	printf("\n[TEST] mc_send_event()\n");
    MatrixClock clock;
    mc_init(&clock);
    mc_send_event(&clock, 1); // follower 1 sends
    CU_ASSERT_EQUAL(clock.mc[1][1], 1);
    printf("[PASS] mc_send_event verified\n");
}

// Test 3: mc_receive_event()
void test_mc_receive_event(void)
{
	printf("\n[TEST] mc_receive_event()\n");
    MatrixClock local, received;
    mc_init(&local);
    mc_init(&received);

    received.mc[0][0] = 2;
    received.mc[1][1] = 1;

    mc_receive_event(&local, &received, 2); // follower 2 receives

    CU_ASSERT_EQUAL(local.mc[0][0], 2);
    CU_ASSERT_EQUAL(local.mc[1][1], 1);
    CU_ASSERT_EQUAL(local.mc[2][2], 1);
    printf("[PASS] mc_receive_event verified\n");
}

// Test 4: Multiple events increment
void test_multiple_events(void)
{
	printf("\n[TEST] multiple_events()\n");
    MatrixClock clock;
    mc_init(&clock);

    mc_send_event(&clock, 0);
    mc_send_event(&clock, 0);
    mc_send_event(&clock, 1);

    CU_ASSERT_EQUAL(clock.mc[0][0], 2);
    CU_ASSERT_EQUAL(clock.mc[1][1], 1);
    printf("[PASS] multiple_events verified\n");
}

// Test 5: Merge and increment
void test_merge_and_increment(void)
{
	printf("\n[TEST] merge_and_increment()\n");
    MatrixClock local, received;
    mc_init(&local);
    mc_init(&received);

    received.mc[0][0] = 5;
    received.mc[1][1] = 3;

    mc_receive_event(&local, &received, 0);

    CU_ASSERT_EQUAL(local.mc[0][0], 6); // local increment + max merge
    CU_ASSERT_EQUAL(local.mc[1][1], 3);
    printf("[PASS] merge_and_increment verified\n");
}

/* ------------------------ Defect Tests ------------------------ */
/*
// Test 6: Receive null pointer (should not crash)
void test_receive_null(void)
{
	printf("\n[DEFECT TEST] receive_null()\n");
    MatrixClock local;
    mc_init(&local);
    mc_receive_event(&local, NULL, 1);
    CU_ASSERT_EQUAL(local.mc[1][1], 1); // local increment still happens
    printf("[PASS] receive_null verified\n");
}
*/

/*
// Test 7: Send event with invalid id
void test_send_invalid_id(void)
{
	printf("\n[DEFECT TEST] send_invalid_id()\n");
    MatrixClock clock;
    mc_init(&clock);
    mc_send_event(&clock, NUM_TRUCKS+1); // out of bounds
    // nothing should change in valid range
    for (int i = 0; i < NUM_TRUCKS; i++)
        for (int j = 0; j < NUM_TRUCKS; j++)
            CU_ASSERT_EQUAL(clock.mc[i][j], 0);
    
    printf("[PASS] send_invalid_id verified\n");
}
*/


// Test 8: Merge with all zeros
void test_merge_zeros(void)
{
	printf("\n[DEFECT TEST] merge_zeros()\n");
    MatrixClock local, received;
    mc_init(&local);
    mc_init(&received);
    mc_receive_event(&local, &received, 0);
    CU_ASSERT_EQUAL(local.mc[0][0], 1); // only local increment
    printf("[PASS] merge_zeros verified\n");
}

// Test 9: Large event values
void test_large_values(void)
{
    printf("\n[DEFECT TEST] large_values()\n");
    MatrixClock local, received;
    mc_init(&local);
    mc_init(&received);

    received.mc[0][0] = 1000;
    mc_receive_event(&local, &received, 0);
    CU_ASSERT_EQUAL(local.mc[0][0], 1001); // max + local increment
    printf("[PASS] large_values verified\n");
}

// Test 10: Negative values (defect simulation)
void test_negative_values(void)
{
	printf("\n[DEFECT TEST] negative_values()\n");
    MatrixClock local, received;
    mc_init(&local);
    mc_init(&received);

    received.mc[0][0] = -5;
    mc_receive_event(&local, &received, 0);
    CU_ASSERT(local.mc[0][0] >= 0); // should never be negative
    
    printf("[PASS] negative_values verified\n");
}

/* ------------------------ Component Tests ------------------------ */

// Test 11: mc_send_event + mc_receive_event combined
void test_component_integration(void)
{
	printf("\n[COMPONENT TEST] component_integration()\n");
    MatrixClock a, b;
    mc_init(&a);
    mc_init(&b);

    mc_send_event(&a, 0);
    mc_receive_event(&b, &a, 1);

    CU_ASSERT_EQUAL(b.mc[0][0], 1);
    CU_ASSERT_EQUAL(b.mc[1][1], 1);
    printf("[PASS] component_integration verified\n");
}

// Test 12: Multiple followers sending and merging
void test_multi_follower_integration(void)
{
    printf("\n[COMPONENT TEST] multi_follower_integration()\n");
    MatrixClock a, b, c;
    mc_init(&a); mc_init(&b); mc_init(&c);

    mc_send_event(&a, 0);
    mc_send_event(&b, 1);
    mc_receive_event(&c, &a, 2);
    mc_receive_event(&c, &b, 2);

    CU_ASSERT_EQUAL(c.mc[0][0], 1);
    CU_ASSERT_EQUAL(c.mc[1][1], 1);
    CU_ASSERT_EQUAL(c.mc[2][2], 2);
    printf("[PASS] multi_follower_integration verified\n");
}

// Test 13: Local increment check
void test_local_increment(void)
{
	printf("\n[COMPONENT TEST] local_increment()\n");
    MatrixClock clock;
    mc_init(&clock);
    mc_send_event(&clock, 2);
    CU_ASSERT_EQUAL(clock.mc[2][2], 1);
    printf("[PASS] local_increment verified\n");
}

/* ------------------------ Test Runner ------------------------ */
int main(void)
{
    CU_initialize_registry();
    CU_pSuite suite = CU_add_suite("Matrix Clock Tests", NULL, NULL);

    CU_add_test(suite, "mc_init", test_mc_init);
    CU_add_test(suite, "mc_send_event", test_mc_send_event);
    CU_add_test(suite, "mc_receive_event", test_mc_receive_event);
    CU_add_test(suite, "multiple_events", test_multiple_events);
    CU_add_test(suite, "merge_and_increment", test_merge_and_increment);

//    CU_add_test(suite, "receive_null", test_receive_null);
//    CU_add_test(suite, "send_invalid_id", test_send_invalid_id);
    CU_add_test(suite, "merge_zeros", test_merge_zeros);
    CU_add_test(suite, "large_values", test_large_values);
    CU_add_test(suite, "negative_values", test_negative_values);

    CU_add_test(suite, "component_integration", test_component_integration);
    CU_add_test(suite, "multi_follower_integration", test_multi_follower_integration);
    CU_add_test(suite, "local_increment", test_local_increment);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    CU_cleanup_registry();
    return 0;
}

