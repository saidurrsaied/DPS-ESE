#ifndef CRUISE_CONTROL_H
#define CRUISE_CONTROL_H

#include "truckplatoon.h"




#define TURN_QUEUE_MAX 16

typedef struct {
  float x, y;
  DIRECTION dir;
} TurnEvent;

typedef struct {
  TurnEvent events[TURN_QUEUE_MAX];
  int head;
  int tail;
  int count;
} TurnQueue;



/**
 * @brief Calculate the gap (distance) between two points.
 *
 * @param x1 X coordinate of point 1
 * @param y1 Y coordinate of point 1
 * @param x2 X coordinate of point 2
 * @param y2 Y coordinate of point 2
 * @return float Distance between points
 */
float calculate_gap(float x1, float y1, float x2, float y2);

/**
 * @brief Calculate the new speed for a follower truck based on front truck
 * status.
 *
 * @param current_speed Current speed of the follower
 * @param front_pos_x X coordinate of the truck ahead
 * @param front_pos_y Y coordinate of the truck ahead
 * @param front_speed Current speed of the truck ahead
 * @param leader_base_speed The speed the leader is currently commanding (or
 * moving at)
 * @param my_x My current X coordinate
 * @param my_y My current Y coordinate
 * @return float New target speed
 */
float cruise_control_calculate_speed(float current_speed, float front_pos_x,
                                     float front_pos_y, float front_speed,
                                     float leader_base_speed, float my_x,
                                     float my_y);






/**
 * @brief Initialize the turn queue.
 *
 * @param q Pointer to the TurnQueue
 */
void turn_queue_init(TurnQueue *q);

/**
 * @brief Push a new turn event to the queue.
 *
 * @param q Pointer to the TurnQueue
 * @param x X coordinate of the turn point
 * @param y Y coordinate of the turn point
 * @param dir New direction after the turn
 * @return int 0 on success, -1 if queue is full
 */
int turn_queue_push(TurnQueue *q, float x, float y, DIRECTION dir);

/**
 * @brief Check if the truck has reached the next turn point and update its
 * status.
 *
 * @param q Pointer to the TurnQueue
 * @param x Current X coordinate of the truck
 * @param y Current Y coordinate of the truck
 * @param current_dir Current direction of the truck
 * @param out_dir Output: New direction if turned
 * @param out_x Output: Snapped X coordinate if turned
 * @param out_y Output: Snapped Y coordinate if turned
 * @return int 1 if a turn was executed, 0 otherwise
 */
int turning_check_and_update(TurnQueue *q, float x, float y,
                             DIRECTION current_dir, DIRECTION *out_dir,
                             float *out_x, float *out_y);



#endif // CRUISE_CONTROL_H