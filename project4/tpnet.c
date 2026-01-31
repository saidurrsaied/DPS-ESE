//FILE: tpnet.c

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "truckplatoon.h"
#include "event.h"
#include "tpnet.h"



//FUNC: Join Platoon

int32_t join_platoon(int32_t leader_FD, const char *self_ip, uint16_t self_port){

    FollowerRegisterMsg reg = {0};
    strcpy(reg.selfAddress.ip, self_ip);
    reg.selfAddress.udp_port = self_port;
    int32_t platoon_join_status =  send(leader_FD, &reg, sizeof(reg), 0);
    if(platoon_join_status <0){
        printf("Platoon join failed"); 
    }
    printf("Joined Platoon \n"); 
    return platoon_join_status; 
}

//FUNC: Create TCP Socket and connect to Leader

int32_t connect2Leader(){
    //Create
    int32_t leader_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(leader_fd < 0){
        perror("TCP socket creation failed. Could not connect to leader"); 
        return leader_fd;
    }

    //Config Address
    struct sockaddr_in leader_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(LEADER_PORT)
    };
    inet_pton(AF_INET, LEADER_IP, &leader_addr.sin_addr);
    
    //Connect
    int32_t status; 
    status = connect(leader_fd, (struct sockaddr*)&leader_addr, sizeof(leader_addr)); 
    if (status < 0) {
        perror("Connection to leader failed");
        return status;
    }
    printf("Connected to Leader\n");
    // If creation and connection succeeds, return the socket fd
    return leader_fd;
}

//FUNC: Create UDP Server to recieve warnings 

int32_t createUDPServer(uint16_t udp_port){
   int32_t udp_sock = socket(AF_INET, SOCK_DGRAM, 0);

   if(udp_sock < 0){
        perror("TCP socket creation failed. Could not connect to leader"); 
        return udp_sock;
    }
    //Config Address
    struct sockaddr_in udp_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(udp_port),
        .sin_addr.s_addr = INADDR_ANY
    };

    int32_t status;
    status =  bind(udp_sock, (struct sockaddr*)&udp_addr, sizeof(udp_addr));
    if (status < 0) {
        perror("Connection to leader failed");
        return status;
    }

    return udp_sock; 
}