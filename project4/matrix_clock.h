#ifndef MATRIX_CLOCK_H
#define MATRIX_CLOCK_H

#include <stdio.h>

#define NUM_TRUCKS 4   // 1 Leader + 3 Followers

// Matrix Clock structure
typedef struct {
    int mc[NUM_TRUCKS][NUM_TRUCKS];
} MatrixClock;

// Initialize matrix clock
void mc_init(MatrixClock *clock);

// Local event (internal processing)
void mc_local_event(MatrixClock *clock, int truck_id); //not required

// Before sending a message
void mc_send_event(MatrixClock *clock, int truck_id);

// On receiving a message
void mc_receive_event(MatrixClock *local,
                      MatrixClock *received,
                      int truck_id);

// function to print matrix clock
void mc_print(MatrixClock *clock);

#endif