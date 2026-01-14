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
    MSG_CMD,
    MSG_UPDATE_REAR
} MsgType;

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
    int has_rearTruck;
    NetInfo rearTruck_Address;
} RearInfoMsg;



typedef struct {
    LeaderCommand queue[CMD_QUEUE_SIZE];
    int head;
    int tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} CommandQueue;

typedef struct {
    MsgType type; 
    union {
        LeaderCommand cmd; 
        RearInfoMsg rearInfo; 
    } payload;       
}LeaderMessage;



#endif
