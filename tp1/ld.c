//ld.c

#define _POSIX_C_SOURCE 200809L

#include "truckplatoon.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#define MAX_FOLLOWERS 3
#define GRID_LENGTH 600
#define GRID_WIDTH 600

void* accept_handler(void* arg);
void driveForward(void);

// SOCKET
int32_t leader_socket_fd;
int32_t follower_fd_list[MAX_FOLLOWERS];
int32_t follower_count = 0;

pthread_mutex_t mutex_client_fd_list;

truck leader;
uint64_t cmd_id = 0;

int main() {
    memset(follower_fd_list, 0, sizeof(follower_fd_list));
    pthread_mutex_init(&mutex_client_fd_list, NULL);

    leader = (truck){0, 0, 0, NORTH, CRUISE};

    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in leader_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(LEADER_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    bind(leader_socket_fd, (struct sockaddr*)&leader_addr, sizeof(leader_addr));
    listen(leader_socket_fd, MAX_FOLLOWERS);

    pthread_t tid;
    pthread_create(&tid, NULL, accept_handler, NULL);

    printf("Leader started\n");

    while (1) {
        driveForward();

        LeaderMessage msg = {
            .leaderState = leader,
            .command_id = cmd_id++,
            .cmd_timestamp = now_ms()
        };

        pthread_mutex_lock(&mutex_client_fd_list);
        for (int i = 0; i < follower_count; i++) {
            send(follower_fd_list[i], &msg, sizeof(msg), 0);
        }
        pthread_mutex_unlock(&mutex_client_fd_list);

        printf("Leader pos: (%d,%d). sent cmd: %lu @timestamp: %lu\n",
               leader.positionX, 
               leader.positionY, 
               msg.command_id,
               msg.cmd_timestamp              
            );
        fflush(stdout);

        sleep(LEADER_SLEEP);
    }
}

void driveForward(void) {
    leader.positionY++;
}

void* accept_handler(void* arg) {
    while (1) {
        int fd = accept(leader_socket_fd, NULL, NULL);
        if (fd >= 0) {
            pthread_mutex_lock(&mutex_client_fd_list);
            if (follower_count < MAX_FOLLOWERS) {
                follower_fd_list[follower_count++] = fd;
                printf("Follower connected (%d)\n", follower_count);
            } else {
                close(fd);
            }
            pthread_mutex_unlock(&mutex_client_fd_list);
        }
    }
}
