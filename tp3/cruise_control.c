#include "cruise_control.h"
#include <math.h>

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
