#ifndef EVENT_H
#define EVENT_H

#include "truckplatoon.h"
#include <pthread.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_EVENTS 32
#define NUM_PRIORITIES 6 //DOOOOOOOOOOO NOTTTTTT Forgwet to update based on the EventType

typedef enum {
    EVT_EMERGENCY  = 0,   // highest priority
    EVT_INTRUDER   = 1,
    EVT_DISTANCE   = 2, 
    EVT_CRUISE_CMD = 3,
    EVT_INTRUDER_CLEAR = 4, 
    EVT_EMERGENCY_TIMER = 5
} EventType;



typedef struct {
    EventType type;
    union {
        LeaderCommand leader_cmd;
        FT_POSITION ft_pos; 
        IntruderInfo intruder; 
    } event_data;
} Event;

// Event Ring
typedef struct {
    Event queue[MAX_EVENTS];
    int head;
    int tail;
} EventRing;

// Event Queue 
typedef struct {
    EventRing eventRings[NUM_PRIORITIES];
    pthread_mutex_t mutex_eventQueue;
    pthread_cond_t cond_eventQueue;
    pthread_mutexattr_t attr_eventQueue;
} EventQueue;

#endif