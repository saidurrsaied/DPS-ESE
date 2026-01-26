#ifndef CRUISE_CONTROL_H
#define CRUISE_CONTROL_H

#include "truckplatoon.h"

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

#endif // CRUISE_CONTROL_H
