//truckplatoon.h

#ifndef TRUCKPLATOON_H
#define TRUCKPLATOON_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <pthread.h>

/* Networking*/
#define LEADER_IP   "127.0.0.1"
#define LEADER_PORT 5000 

#define LEADER_SLEEP 1
#define MAX_FOLLOWERS 5
#define CMD_QUEUE_SIZE 10

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
    MSG_LDR_EMERGENCY_BRAKE
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
    TURNING, 
    ACCELERATE,
    DECELERATE
} TRUCK_CONTROL_STATE;

/* Truck struct */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t speed;
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
} LeaderCommand;

/* Registration message*/
typedef struct {
    NetInfo selfAddress;
} FollowerRegisterMsg;

/* Topology message */
typedef struct {
    int32_t has_rearTruck;
    NetInfo rearTruck_Address;
} RearInfoMsg;


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
    } payload;       
}LD_MESSAGE;

/* Front Truck UDP Message Typedefs */
typedef struct {
    int32_t x; 
    int32_t y; 
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
}FT_MESSAGE; 

#endif
