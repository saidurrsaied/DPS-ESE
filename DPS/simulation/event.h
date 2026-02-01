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
#define NUM_PRIORITIES 9 // Updated to include leader-specific events

typedef enum {
    EVT_EMERGENCY  = 0,   // highest priority
    EVT_INTRUDER   = 1,
    EVT_DISTANCE   = 2, 
    EVT_CRUISE_CMD = 3,
    EVT_INTRUDER_CLEAR = 4, 
    EVT_EMERGENCY_TIMER = 5,
    EVT_TICK_UPDATE = 6,  // For physics simulation steps (leader)
    EVT_USER_INPUT  = 7,  // For keyboard commands (leader)
    EVT_FOLLOWER_MSG = 8  // Wraps incoming follower network messages (leader)
} EventType;

/* Data for user input events */
typedef struct {
    char key; // 'w','a','s','d', etc.
} UserInputData;

/* Data wrapper for follower-originated messages */
typedef struct {
    int follower_id;      // sender index (0..N)
    FT_MESSAGE msg;       // the original FT_MESSAGE
} FollowerMsgData;

typedef struct {
    EventType type;
    union {
        LeaderCommand leader_cmd;
        FT_POSITION ft_pos; 
        IntruderInfo intruder;
        UserInputData input;        /* EVT_USER_INPUT */
        FollowerMsgData follower_msg; /* EVT_FOLLOWER_MSG */
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

/* Event queue API (generic) */
void event_queue_init(EventQueue* queue);
void push_event(EventQueue* queue, Event* event);
Event pop_event(EventQueue* queue);

#endif