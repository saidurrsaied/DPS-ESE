// follower.c

#define _POSIX_C_SOURCE 200809L
#include "truckplatoon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>

#define INTRUDER_PROB 10   // %

Truck follower;
NetInfo rearTruck_Address;
int has_rearTruck = 0;

pthread_mutex_t follower_mutex;
int udp_sock;
int tcp2Leader;
pthread_t tid;

// Timer to track when braking started
time_t brake_start_time = 0;

void* udp_listener(void* arg);
void maybe_intruder(void);

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <LEADER_IP> <LEADER_TCP_PORT> <MY_IP> <MY_UDP_PORT>\n", argv[0]);
        return 1;
    }

    const char* leader_ip = LEADER_IP;
    uint16_t leader_tcp_port = LEADER_PORT;
    const char* my_ip = LEADER_IP;
    uint16_t my_port = atoi(argv[1]);

    /* Seed RNG and init mutex */
    srand(time(NULL) ^ getpid());
    pthread_mutex_init(&follower_mutex, NULL);

    /* --- TCP Setup (To Leader) --- */
    tcp2Leader = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in leader_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(leader_tcp_port)
    };
    inet_pton(AF_INET, leader_ip, &leader_addr.sin_addr);
    
    if (connect(tcp2Leader, (struct sockaddr*)&leader_addr, sizeof(leader_addr)) < 0) {
        perror("Connection to leader failed");
        return 1;
    }

    // 1. Send Registration
    FollowerRegisterMsg reg = {0};
    strcpy(reg.selfAddress.ip, my_ip);
    reg.selfAddress.udp_port = my_port;
    send(tcp2Leader, &reg, sizeof(reg), 0);

    // 2. Receive Initial Handshake (UnifiedMessage)
    LeaderMessage initMsg;
    if (recv(tcp2Leader, &initMsg, sizeof(initMsg), 0) <= 0) {
        perror("recv initial info");
        return 1;
    }
    
    if (initMsg.type == MSG_UPDATE_REAR) {
        printf("Received rear info: has_rearTruck=%d\n", 
               initMsg.payload.rearInfo.has_rearTruck);
        pthread_mutex_lock(&follower_mutex);
        has_rearTruck = initMsg.payload.rearInfo.has_rearTruck;
        if (has_rearTruck) {
            rearTruck_Address = initMsg.payload.rearInfo.rearTruck_Address;
        }
        pthread_mutex_unlock(&follower_mutex);
    }

    /* --- UDP Setup (follower-to-follower) --- */
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in udp_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(my_port),
        .sin_addr.s_addr = INADDR_ANY
    };
    bind(udp_sock, (struct sockaddr*)&udp_addr, sizeof(udp_addr));

   
    pthread_create(&tid, NULL, udp_listener, NULL);

    printf("Follower started (UDP %d)\n", my_port);

    /* --- Main Loop: Handle Leader TCP Messages --- */
    while (1) {
        LeaderMessage msg;
        if (recv(tcp2Leader, &msg, sizeof(msg), 0) <= 0) break; // Connection closed

        if (msg.type == MSG_CMD) {
            // CASE 1: Leader sent a cruise command
            pthread_mutex_lock(&follower_mutex);

            // Check if we can exit Emergency Brake (5 second timer)
            if (follower.state == EMERGENCY_BRAKE) {
                if (difftime(time(NULL), brake_start_time) >= 5.0) {
                    printf(">> 5 seconds passed. Resuming CRUISE.\n");
                    follower.state = CRUISE;
                }
            }

            // Only update position if we are NOT braking
            if (follower.state != EMERGENCY_BRAKE) {
                follower = msg.payload.cmd.leader;
            }

            printf("Follower pos (%d,%d) state=%d\n",
                   follower.x, follower.y, follower.state);
            pthread_mutex_unlock(&follower_mutex);

            // Randomly simulate an intruder
            maybe_intruder();
        } 
        else if (msg.type == MSG_UPDATE_REAR) {
            // CASE 2: Leader sent an update about a new truck behind us
            pthread_mutex_lock(&follower_mutex);
            printf(">> Update: New truck joined behind me!\n");
            
            has_rearTruck = msg.payload.rearInfo.has_rearTruck;
            if (has_rearTruck) {
                rearTruck_Address = msg.payload.rearInfo.rearTruck_Address;
            }
            pthread_mutex_unlock(&follower_mutex);
        }
    }

    close(tcp2Leader);
    close(udp_sock);
    return 0;
}

void maybe_intruder(void) {
    if ((rand() % 100) < INTRUDER_PROB) {
        pthread_mutex_lock(&follower_mutex);
        
        follower.state = EMERGENCY_BRAKE;
        follower.speed = 0;
        brake_start_time = time(NULL); // <--- FIX: Start timer here
        
        printf("!!! INTRUDER DETECTED !!!\n");

        if (has_rearTruck) {
            struct sockaddr_in dst = {
                .sin_family = AF_INET,
                .sin_port = htons(rearTruck_Address.udp_port)
            };
            inet_pton(AF_INET, rearTruck_Address.ip, &dst.sin_addr);

            const char* msg = "EMERGENCY_BRAKE";
            sendto(udp_sock, msg, strlen(msg), 0,
                   (struct sockaddr*)&dst, sizeof(dst));
        }
        pthread_mutex_unlock(&follower_mutex);
    }
}

// Dedicated thread for listening to UDP emergency messages from other trucks
void* udp_listener(void* arg) {
    (void)arg;
    char buf[64];
    
    while (1) {
        // Listen for UDP packets
        ssize_t n = recvfrom(udp_sock, buf, sizeof(buf)-1, 0, NULL, NULL);
        if (n <= 0) continue;
        buf[n] = '\0';

        pthread_mutex_lock(&follower_mutex);
        
        follower.state = EMERGENCY_BRAKE;
        follower.speed = 0;
        brake_start_time = time(NULL); // <--- FIX: Start timer here

        printf("⚠ EMERGENCY WARNING RECEIVED from UDP ⚠\n");

        // Propagate warning to the truck behind me
        if (has_rearTruck) {
            struct sockaddr_in dst = {
                .sin_family = AF_INET,
                .sin_port = htons(rearTruck_Address.udp_port)
            };
            inet_pton(AF_INET, rearTruck_Address.ip, &dst.sin_addr);
            const char* msg = "EMERGENCY_BRAKE";
            sendto(udp_sock, msg, strlen(msg), 0,
                   (struct sockaddr*)&dst, sizeof(dst));
        }
        pthread_mutex_unlock(&follower_mutex);
    }
    return NULL;
}