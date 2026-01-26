#include "turning.h"

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
                             float *out_x, float *out_y) {
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

    // Pop from queue
    q->head = (q->head + 1) % TURN_QUEUE_MAX;
    q->count--;

    return 1;
  }

  return 0;
}
