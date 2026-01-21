//file: emergency.c

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/socket.h>

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


// FUNC: Entry Actions for emergency Brake 
void enter_emergency(void) {
    follower.state = EMERGENCY_BRAKE;
    follower.speed = 0;
    brake_start_time = time(NULL);
    propagate_emergency();
}