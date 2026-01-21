//fl.c

#define _POSIX_C_SOURCE 200809L
#include "truckplatoon.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in leader_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(LEADER_PORT)
    };

    inet_pton(AF_INET, LEADER_IP, &leader_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&leader_addr, sizeof(leader_addr)) < 0) {
        perror("connect failed");
        return -1;
    }

    printf("Connected to leader\n");

    while (1) {
        LeaderMessage msg; 
        int bytes = recv(sock, &msg, sizeof(msg), 0);
        uint64_t rcv_ts = now_ms(); 
        uint64_t latency = rcv_ts - msg.cmd_timestamp; 

        if (bytes <= 0) {
            printf("Leader disconnected\n");
            break;
        }

        printf("Received cmd: %lu @timestamp: %lu. [Leader @ (%d,%d), speed=%d]. Latency: %lu\n",
                msg.command_id, 
                rcv_ts, 
                msg.leaderState.positionX,
                msg.leaderState.positionY,
                msg.leaderState.currentSpeed,
                latency
                );
            fflush(stdout);
    }

    close(sock);
    return 0;
}
