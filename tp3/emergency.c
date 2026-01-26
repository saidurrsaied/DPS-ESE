//file: emergency.c

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include "truckplatoon.h"
#include "event.h"
#include "follower.h"



//FUNC: Propagate Emergency Brake 

void propagate_emergency(void){
    if (!has_rearTruck)
        return;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));

    dst.sin_family = AF_INET;
    dst.sin_port   = htons(rearTruck_Address.udp_port);

    if (inet_pton(AF_INET,
                  rearTruck_Address.ip,
                  &dst.sin_addr) != 1) {
        perror("inet_pton failed in propagate_emergency");
        return;
    }

    FT_MESSAGE emergency_warning = {.type=MSG_FT_EMERGENCY_BRAKE, 
                                    .payload.warning.emergency_Flag = 1, .payload.warning.resendFlag =0 }; 

    int32_t status = sendto(udp_sock,
                          &emergency_warning,
                          sizeof(emergency_warning),
                          0,
                          (struct sockaddr*)&dst,
                          sizeof(dst));

    if (status < 0) {
        perror("sendto EMERGENCY failed");
    } else {
        printf("[PROPAGATE] Emergency sent to rear truck %s:%d\n",
               rearTruck_Address.ip,
               rearTruck_Address.udp_port);
    }
}





// Emergency Timer thread 
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

// FUNC: Entry Actions for emergency Brake 
void enter_emergency(void) {
    follower.state = EMERGENCY_BRAKE;
    follower.speed = 0;
    propagate_emergency();
    start_emergency_timer(5000);   
}

void exit_emergency(void){
    follower.state = CRUISE; 
}