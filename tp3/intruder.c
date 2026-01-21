#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "follower.h"


#define INTRUDER_PROBABILITY 10  // %

int intruder_detected(void) {
    return (rand() % 100) < INTRUDER_PROBABILITY;
}

int intruder_speed(void) {
    return 30 + rand() % (120 - 30 + 1);
}

int intruder_length(void) {
    return 3 + rand() % (20 - 3 + 1);
}

uint32_t intruder_duration(void) {
    return 5000 + rand() % (10000 - 5000 + 1);
}

typedef struct {
    int32_t intruder_speed;
} IntruderReportMsg;


void notify_leader_intruder(IntruderInfo intruder) {
    FT_MESSAGE msg = {0}; 
    msg.type = MSG_FT_INTRUDER_REPORT; 
    msg.payload.intruder = intruder; 

    int32_t status = send( tcp2Leader, &msg, sizeof(msg), 0);
    if (status < 0) {
        perror("notify_leader_intruder()");
    }
}

// intruder Time-out event generation 
void* intruder_timer_thread(void* arg) {
    uint32_t duration_ms = *(uint32_t*)arg;
    free(arg);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        perror("timerfd_create");
        return NULL;
    }

    struct itimerspec its = {0};
    its.it_value.tv_sec  = duration_ms / 1000;
    its.it_value.tv_nsec = (duration_ms % 1000) * 1000000;

    timerfd_settime(tfd, 0, &its, NULL);

    uint64_t expirations;
    read(tfd, &expirations, sizeof(expirations));

    Event e = {.type = EVT_INTRUDER_CLEAR};
    push_event(&truck_EventQ, &e);

    close(tfd);
    return NULL;
}


void start_intruder_timer(uint32_t duration_ms) {
    pthread_t tid;
    uint32_t* arg = malloc(sizeof(uint32_t));
    *arg = duration_ms;

    pthread_create(&tid, NULL, intruder_timer_thread, arg);
    pthread_detach(tid);
}


void* emergency_timer_thread(void* arg) {
    uint32_t duration_ms = *(uint32_t*)arg;
    free(arg);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        perror("timerfd_create");
        return NULL;
    }

    struct itimerspec its = {0};
    its.it_value.tv_sec  = duration_ms / 1000;
    its.it_value.tv_nsec = (duration_ms % 1000) * 1000000;

    timerfd_settime(tfd, 0, &its, NULL);

    uint64_t expirations;
    read(tfd, &expirations, sizeof(expirations));

    Event e = {.type = EVT_EMERGENCY_TIMER};
    push_event(&truck_EventQ, &e);

    close(tfd);
    return NULL;
}

//Start energency Timer 

void start_emergency_timer(uint32_t duration_ms) {
    pthread_t tid;
    uint32_t* arg = malloc(sizeof(uint32_t));
    *arg = duration_ms;

    pthread_create(&tid, NULL, emergency_timer_thread, arg);
    pthread_detach(tid);
}


void maybe_intruder(void) {
    if (!intruder_detected())
        return;

    IntruderInfo intr = {
        .speed       = intruder_speed(),
        .length      = intruder_length(),
        .duration_ms = intruder_duration()
    };

    Event e = {
        .type = EVT_INTRUDER,
        .event_data.intruder = intr
    };

    push_event(&truck_EventQ, &e);
}



//Func: Entry action to intruder follow state
void enter_intruder_follow(IntruderInfo intruder) {
    follower.state = INTRUDER_FOLLOW;

    follower.speed = intruder.speed;
    //TODO: 
    //Implement func: increase_follow_distance(intruder.length);

    notify_leader_intruder(intruder);
    start_intruder_timer(intruder.duration_ms);
}

//  FUNC: Exit action to intruder exit state 

void exit_intruder_follow(void) {
    follower.state = CRUISE;
    restore_nominal_distance();
}

//FUNC: Entry action while still in intruder state to match intruder speed 
void update_intruder(IntruderInfo intruder) {
    follower.speed = intruder.speed;
}

#define NOMINAL_FOLLOW_DISTANCE 10  // meters

void restore_nominal_distance(void) {
    // Placeholder logic — later replace with control law
    printf("Restoring nominal following distance: %d m\n",
           NOMINAL_FOLLOW_DISTANCE);
}



