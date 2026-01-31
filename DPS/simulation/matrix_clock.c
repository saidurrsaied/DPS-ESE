// matrix_clock.c

#include "matrix_clock.h"
#include <string.h>

/*
 Truck IDs
 0 -> Leader
 1 -> Follower 1
 2 -> Follower 2
 3 -> Follower 3
*/

// Initialize matrix clock to zero
void mc_init(MatrixClock *clock)
{
    memset(clock->mc, 0, sizeof(clock->mc));
}

// Local event (internal computation)
//not required
void mc_local_event(MatrixClock *clock, int truck_id)
{
    clock->mc[truck_id][truck_id]++;
}

// Send event (before sending message)
void mc_send_event(MatrixClock *clock, int truck_id)
{
    // sending is a local event
    clock->mc[truck_id][truck_id]++;
}

// Receive event (on message arrival)
void mc_receive_event(MatrixClock *local, MatrixClock *received, int truck_id)
{
    // Step 1: merge matrices
    for (int i = 0; i < NUM_TRUCKS; i++) {
        for (int j = 0; j < NUM_TRUCKS; j++) {
            if (received->mc[i][j] > local->mc[i][j]) {
                local->mc[i][j] = received->mc[i][j];
            }
        }
    }

    // Step 2: receiving itself is a local event
    local->mc[truck_id][truck_id]++;
}

// Print matrix clock
void mc_print(MatrixClock *clock)
{
    printf("Matrix Clock:\n");
    for (int i = 0; i < NUM_TRUCKS; i++) {
        for (int j = 0; j < NUM_TRUCKS; j++) {
            printf("%3d ", clock->mc[i][j]);
        }
        printf("\n");
    }
    printf("\n");
}

