#ifndef TRUCKPLATOON_H
#define TRUCKPLATOON_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <time.h>

/*  Networking */
#define LEADER_IP   "127.0.0.1"
#define LEADER_PORT 5000

#define LEADER_SLEEP 1   // seconds

/* Directions & States*/
typedef enum {
    NORTH = 0,
    EAST,
    SOUTH,
    WEST
} DIRECTION;

typedef enum {
    CRUISE = 0,
    ACCELERATE,
    DECELERATE,
    TURNING,
    EMERGENCY_BRAKE,
    STOPPED
} TRUCK_CONTROL_STATE;

/* Truck state */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t speed;   // cells per tick
    DIRECTION dir;
    TRUCK_CONTROL_STATE state;
} TruckState;

/* Leader → follower message */
typedef struct {
    uint64_t command_id;
    TruckState leader;
} LeaderMessage;

/* Time helper (monotonic) */
static inline uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

#endif
