#include "cruise_control.h"
#include <math.h>
#include "matrix_clock.h"

int follower_idx;
MatrixClock follower_clock;

float calculate_gap(float x1, float y1, float x2, float y2) {
  return sqrtf(powf(x1 - x2, 2) + powf(y1 - y2, 2));
}

float cruise_control_calculate_speed(float current_speed, float front_pos_x,
                                     float front_pos_y, float front_speed,
                                     float leader_base_speed, float my_x,
                                     float my_y) {

  // 1. Physical gap to the received front truck position
  float static_gap = calculate_gap(my_x, my_y, front_pos_x, front_pos_y);

  /*
   * Projected Error:
   * (static_gap - TARGET_GAP) - (front_speed * SIM_DT)
   *
   * This formula anticipates the leader's movement during the 0.1s tick.
   * By subtracting the expected movement, we shift the control target
   * to align perfectly with the terminal display.
   */
  float projected_error = (static_gap - TARGET_GAP) - (front_speed * SIM_DT);

  /* TUNING
     Kp: 0.35 (Correction gain)
     Kd: 0.70 (Damping gain - Matches speeds)
  */
  float Kp = 0.35f;
  float Kd = 0.70f;

  // 2. Control Law Structure
  // New Speed = Leader Intent + Damping + Correction
  float base = leader_base_speed;
  float damping = (front_speed - current_speed) * Kd;
  float correction = projected_error * Kp;

  float new_speed = base + damping + correction;

  // 3. Safety Clamps
  if (new_speed < 0)
    new_speed = 0;
  if (new_speed > base + 25.0f)
    new_speed = base + 25.0f;

  return new_speed;
}


/*
Functions for turning queue management
*/

void turn_queue_init(TurnQueue *q) {
  q->head = 0;
  q->tail = 0;
  q->count = 0;
}

int turn_queue_push(TurnQueue *q, float x, float y, DIRECTION dir) {
  if (q->count >= TURN_QUEUE_MAX) {
    return -1; // Queue full
  }
  q->events[q->tail].x = x;
  q->events[q->tail].y = y;
  q->events[q->tail].dir = dir;
  q->tail = (q->tail + 1) % TURN_QUEUE_MAX;
  q->count++;
  return 0;
}

int turning_check_and_update(TurnQueue *q, float x, float y,
                             DIRECTION current_dir, DIRECTION *out_dir,
                             float *out_x, float *out_y, int follower_idx) {
  if (q->count == 0) {
    return 0;
  }

  TurnEvent next_ev = q->events[q->head];
  int passed = 0;

  // Check if we have reached or passed the turning point based on current
  // direction A small buffer (0.1f) is used to ensure we don't miss it due to
  // discrete time steps
  switch (current_dir) {
  case NORTH:
    if (y >= next_ev.y - 0.1f)
      passed = 1;
    break;
  case SOUTH:
    if (y <= next_ev.y + 0.1f)
      passed = 1;
    break;
  case EAST:
    if (x >= next_ev.x - 0.1f)
      passed = 1;
    break;
  case WEST:
    if (x <= next_ev.x + 0.1f)
      passed = 1;
    break;
  }

  if (passed) {
    // Snap to the exact turn point and update direction
    *out_x = next_ev.x;
    *out_y = next_ev.y;
    *out_dir = next_ev.dir;
    
    mc_local_event(&follower_clock, follower_idx); //mc: update turn event in matrix clock

    // Pop from queue
    q->head = (q->head + 1) % TURN_QUEUE_MAX;
    q->count--;

    return 1;
  }

  return 0;
}
