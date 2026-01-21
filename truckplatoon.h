//file: truckplatoon.h

#ifndef TRUCKPLATOON_H
#define TRUCKPLATOON_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>


//SOCKET
#define LEADER_IP "127.0.0.1"
#define LEADER_PORT 5000


#define CONNECTION_TIMEOUT_MS 500
#define MESSAGE_TIMEOUT_MS 200
#define LEADER_SLEEP 2 //sec

typedef enum {
    EAST,       // +x axis
    WEST,       // -x axis
    NORTH,      // +y axis
    SOUTH       // -y axis
}DIRECTION; 

typedef enum {
    IDLE, 
    CRUISE, 
    SOFT_BRAKE, 
    EMERGENCY_BRAKE
}TRUCK_CONTROL_STATE; 


//struct to define truck state

typedef struct __attribute__ ((packed)) {
    int32_t positionX ; 
    int32_t positionY; 
    int32_t currentSpeed; 
    DIRECTION heading; 
    TRUCK_CONTROL_STATE state; 
}truck;



//Struct used for leader messages 
typedef struct __attribute__((packed))  {
    truck leaderState; 
    uint64_t cmd_timestamp; 
    uint64_t command_id;
}LeaderMessage; 


//time

static inline uint64_t now_ms(){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); 
    uint64_t time_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000; 
    return time_ms;

}

#endif