//truckplatoon.h

#ifndef TRUCKPLATOON_H
#define TRUCKPLATOON_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <pthread.h>
#include "matrix_clock.h"

/* Networking*/
#define LEADER_IP   "127.0.0.1"
#define LEADER_PORT 5000 

#define LEADER_SLEEP 1
#define MAX_FOLLOWERS 5
#define CMD_QUEUE_SIZE 10
#define SAFE_DISTANCE 15 
#define SIM_DT 0.1f // Legacy default; prefer *_DT constants below

/* Simulation/Control Rates (seconds)
 * - LEADER_TICK_DT: leader EVT_TICK_UPDATE generation
 * - FOLLOWER_PHYS_DT: follower physics + UDP position broadcast cadence
 * - CONTROL_DT: dt used inside cruise control prediction
 */
#define LEADER_TICK_DT 0.25f
#define FOLLOWER_PHYS_DT 0.25f
#define CONTROL_DT FOLLOWER_PHYS_DT

/* Speed limits
 * Current controller clamps follower speed to (leader_base_speed + MAX_SPEED_OVER_BASE).
 */
#define MAX_SPEED_OVER_BASE 100.0f

/* Print decimation (print every N ticks). Set to 1 to print every tick. */
#define LEADER_PRINT_EVERY_N 5
#define FOLLOWER_PRINT_EVERY_N 5
#define TARGET_GAP 10.0f //BW

/* Leader liveness / control freshness (follower-side watchdog)
 * The follower records the time of the last leader TCP message (any type) and enters a safe
 * state if messages go stale.
 */
#define LEADER_RX_TIMEOUT_MS 2000
#define LEADER_WATCHDOG_PERIOD_MS 100

/* GPU intruder integration (file-based IPC from GPU service). */ //Rajdeep
#define GPU_INTRUDER_FILE "intruder.txt" //Rajdeep
#define GPU_INTRUDER_POLL_MS 200 //Rajdeep
/* Directions & States */
typedef enum {
    NORTH,
    EAST,
    SOUTH,
    WEST
} DIRECTION;

/* Message Types */
typedef enum {
    MSG_LDR_CMD,
    MSG_LDR_UPDATE_REAR, 
    MSG_LDR_EMERGENCY_BRAKE, 
    MSG_LDR_ASSIGN_ID,
    MSG_LDR_SPAWN
} Leader_Truck_MSG_Type;

typedef enum {
    MSG_FT_POSITION, 
    MSG_FT_EMERGENCY_BRAKE, 
    MSG_FT_INTRUDER_REPORT
}Follower_Truck_MSG_Type;

typedef enum {
    LEADER, 
    FOLLOWER, 
} MSG_SENDER; 

typedef enum {
    CRUISE,
    EMERGENCY_BRAKE,
    STOPPED, 
    INTRUDER_FOLLOW,
    PLATOONING
} TRUCK_CONTROL_STATE;

/* Truck struct */
typedef struct {
    float x;
    float y;
    float speed;
    DIRECTION dir;
    TRUCK_CONTROL_STATE state;
} Truck;

/* Network info */
typedef struct {
    char ip[16];
    uint16_t udp_port;
} NetInfo;

/*  Leader messages*/
typedef struct {
    uint64_t command_id;
    Truck leader;
    int32_t is_turning_event; 
    float turn_point_x; 
    float turn_point_y; 
    DIRECTION turn_dir; 
} LeaderCommand;

/* Registration message*/
typedef struct {
    NetInfo selfAddress;
    MatrixClock matrix_clock;
} FollowerRegisterMsg;

/* Topology message */
typedef struct {
    int32_t has_rearTruck;
    NetInfo rearTruck_Address;
} RearInfoMsg;

/* Spawn message sent by leader to a newly joined follower (TCP).
 * Allows realistic join near the current platoon position rather than a fixed start slot.
 */
typedef struct {
    int32_t assigned_id; /* platoon position */
    float spawn_x;
    float spawn_y;
    DIRECTION spawn_dir;
} SpawnInfoMsg;


typedef struct {
    int32_t speed;          // intruder speed
    int32_t length;         // intruder length required ro claculate safe distance 
    uint32_t duration_ms;   // expected intrusion duration
} IntruderInfo;

typedef struct {
    LeaderCommand queue[CMD_QUEUE_SIZE];
    int32_t head;
    int32_t tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} CommandQueue;

typedef struct {
    Leader_Truck_MSG_Type type; 
    union {
        LeaderCommand cmd; 
        RearInfoMsg rearInfo; 
        int32_t assigned_id;
        SpawnInfoMsg spawn;
    } payload; 
    MatrixClock matrix_clock;      
}LD_MESSAGE;

/* Front Truck UDP Message Typedefs */
typedef struct {
    float x; 
    float y; 
    float speed;
}FT_POSITION; 

typedef struct {
    uint8_t emergency_Flag; 
    uint8_t resendFlag; 
}FT_EMERGENCY; 

typedef struct {
    Follower_Truck_MSG_Type type; 
    union{
        FT_POSITION position; 
        FT_EMERGENCY warning; 
        IntruderInfo intruder; 
    }payload; 
    MatrixClock matrix_clock;
}FT_MESSAGE; 

#endif
