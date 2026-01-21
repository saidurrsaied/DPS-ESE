#define _POSIX_C_SOURCE 200809L
#include "truckplatoon.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PATH_BUFFER 1024
#define FOLLOW_DELAY 5

int main(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in leader_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(LEADER_PORT)
    };

    inet_pton(AF_INET, LEADER_IP, &leader_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&leader_addr,
                sizeof(leader_addr)) < 0) {
        perror("connect");
        return -1;
    }

    printf("Follower connected\n");

    TruckState leader_path[PATH_BUFFER];
    int path_head = 0;

    TruckState follower = {0};

    while (1) {
        LeaderMessage msg;
        int bytes = recv(sock, &msg, sizeof(msg), 0);
        if (bytes <= 0) {
            printf("Leader disconnected\n");
            break;
        }

        /* Store leader trajectory */
        leader_path[path_head % PATH_BUFFER] = msg.leader;
        path_head++;

        int follow_idx = path_head - FOLLOW_DELAY;
        if (follow_idx >= 0) {
            follower = leader_path[follow_idx % PATH_BUFFER];

            printf(
                "F follow cmd=%lu pos=(%d,%d) dir=%d spd=%d state=%d\n",
                msg.command_id,
                follower.x,
                follower.y,
                follower.dir,
                follower.speed,
                follower.state
            );
            fflush(stdout);
        }
    }

    close(sock);
    return 0;
}
