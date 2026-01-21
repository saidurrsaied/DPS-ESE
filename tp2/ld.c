#define _POSIX_C_SOURCE 200809L
#include "truckplatoon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_FOLLOWERS 3

/*  Globals */
int leader_socket_fd;
int follower_fd_list[MAX_FOLLOWERS];
int follower_count = 0;
pthread_mutex_t mutex_client_fd_list;

TruckState leader;
uint64_t cmd_id = 0;

/* Prototypes */
void* accept_handler(void* arg);
void leader_decide_next_state(TruckState* t);
void move_truck(TruckState* t);



/*  Main*/
int main(void) {
    srand(time(NULL));

    memset(follower_fd_list, 0, sizeof(follower_fd_list));
    pthread_mutex_init(&mutex_client_fd_list, NULL);

    leader = (TruckState){
        .x = 0, .y = 0,
        .speed = 1,
        .dir = NORTH,
        .state = CRUISE
    };

    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(LEADER_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    bind(leader_socket_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(leader_socket_fd, MAX_FOLLOWERS);

    pthread_t tid;
    pthread_create(&tid, NULL, accept_handler, NULL);

    printf("Leader started\n");

    while (1) {
        leader_decide_next_state(&leader);
        move_truck(&leader);

        LeaderMessage msg = {
            .command_id = cmd_id++,
            .leader = leader
        };

        pthread_mutex_lock(&mutex_client_fd_list);
        for (int i = 0; i < follower_count; i++) {
            send(follower_fd_list[i], &msg, sizeof(msg), 0);
        }
        pthread_mutex_unlock(&mutex_client_fd_list);

        printf(
            "L cmd=%lu pos=(%d,%d) dir=%d spd=%d state=%d\n",
            msg.command_id,
            leader.x,
            leader.y,
            leader.dir,
            leader.speed,
            leader.state
        );
        fflush(stdout);

        sleep(LEADER_SLEEP);
    }
}

/* Accept thread */
void* accept_handler(void* arg) {
    (void)arg;
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



/*  Leader logic */
void leader_decide_next_state(TruckState* t) {
    int r = rand() % 100;

    if (r < 2) {
        t->state = EMERGENCY_BRAKE;
        t->speed = 0;
        return;
    }

    if (r < 7) {
        t->state = TURNING;
        t->dir = (rand() % 2)
                   ? (t->dir + 1) % 4
                   : (t->dir + 3) % 4;
        return;
    }

    if (r < 17) {
        t->state = ACCELERATE;
        if (t->speed < 3) t->speed++;
        return;
    }

    if (r < 27) {
        t->state = DECELERATE;
        if (t->speed > 1) t->speed--;
        return;
    }

    t->state = CRUISE;
}

void move_truck(TruckState* t) {
    for (int i = 0; i < t->speed; i++) {
        switch (t->dir) {
            case NORTH: t->y++; break;
            case SOUTH: t->y--; break;
            case EAST:  t->x++; break;
            case WEST:  t->x--; break;
        }
    }
}
