//leader.c

//TODO: 
/*
    1. Add follower intruder notification receive capability and handle logic 
           **Details: If the follower send an intruder warning to the leader, the leader shall cruise at the speed of the 
            intruder (sent by follower in the intruder notification ). The leader shall exit upon intruder timeout/ clear event. 
*/

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "truckplatoon.h"
#include "matrix_clock.h"

int leader_socket_fd;
int follower_fd_list[MAX_FOLLOWERS];
NetInfo follower_Addresses[MAX_FOLLOWERS];
int follower_count = 0;

pthread_mutex_t mutex_client_fd_list;
pthread_t sender_tid;
pthread_t acceptor_tid;

Truck leader;
uint64_t cmd_id = 0;
CommandQueue cmd_queue; 

MatrixClock leader_clock; //matrix clock declaration

void* accept_handler(void* arg);
void* send_handler(void* arg);
void leader_decide_next_state(Truck* t);
void move_truck(Truck* t);
void queue_commands(LeaderCommand* ldr_cmd);

int main(void) {
    srand(time(NULL));
    pthread_mutex_init(&mutex_client_fd_list, NULL);

    leader = (Truck){0,0,1,NORTH,CRUISE};

    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    matrix_clock_init(&leader_clock, 0); // matrix clock

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(LEADER_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(leader_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(leader_socket_fd, MAX_FOLLOWERS) < 0) {
        perror("listen");
        return 1;
    }

    /* Initialize command queue before starting threads */
    cmd_queue.head = 0;
    cmd_queue.tail = 0;
    pthread_mutex_init(&cmd_queue.mutex, NULL);
    pthread_cond_init(&cmd_queue.not_empty, NULL);

    /* Start acceptor and sender threads */
    if (pthread_create(&acceptor_tid, NULL, accept_handler, NULL) != 0) {
        perror("pthread_create acceptor");
        return 1;
    }
    if (pthread_create(&sender_tid, NULL, send_handler, NULL) != 0) {
        perror("pthread_create sender");
        return 1;
    }

    printf("Leader started\n");

    while (1) {
        leader_decide_next_state(&leader);
        move_truck(&leader);

        LeaderCommand ldr_cmd = {
            .command_id = cmd_id++,
            .leader = leader
        };
        
        queue_commands(&ldr_cmd);

        printf("Leader pos (%d,%d), sent command: %lu \n", leader.x, leader.y, cmd_id);
        fflush(stdout);

        sleep(LEADER_SLEEP);
    }
}


void* accept_handler(void* arg) {
    (void)arg;

    while (1) {
        int follower_fd = accept(leader_socket_fd, NULL, NULL);
        if (follower_fd < 0) continue;

        FollowerRegisterMsg reg_msg;
        recv(follower_fd, &reg_msg, sizeof(reg_msg), 0);
        
        /* Matrix clock*/
        matrix_clock_on_receive(&leader_clock, msg.matrix_clock);
        matrix_clock_print(&leader_clock, "Leader received message");
		/**/

        pthread_mutex_lock(&mutex_client_fd_list);
        int id = follower_count;
        follower_fd_list[id] = follower_fd;
        follower_Addresses[id] = reg_msg.selfAddress;
        follower_count++;

        // Send rear-truck info to the newly connected follower. 
        //The first follower has no rear truck.but it expects a message
        LD_MESSAGE initialMsg = {0};
        initialMsg.type = MSG_LDR_UPDATE_REAR;
        initialMsg.payload.rearInfo.has_rearTruck = 0; // First connection has no rear
        
        /* Matrix clock*/
        matrix_clock_on_send(&leader_clock);
        memcpy(msg.matrix_clock, leader_clock.clock, sizeof(leader_clock.clock));
        /**/
        
        
        send(follower_fd, &initialMsg, sizeof(initialMsg), 0);

        // If there is a previous follower, update it so its rear points to this new follower 
        if (id > 0) {
            LD_MESSAGE update_rearInfo = {0};
            update_rearInfo.type = MSG_LDR_UPDATE_REAR;
            update_rearInfo.payload.rearInfo.rearTruck_Address = reg_msg.selfAddress;
            update_rearInfo.payload.rearInfo.has_rearTruck = 1; 
            
            /* Matrix clock*/
        matrix_clock_on_send(&leader_clock);
        memcpy(msg.matrix_clock, leader_clock.clock, sizeof(leader_clock.clock));
        /**/
            
            
            send(follower_fd_list[id-1], &update_rearInfo, sizeof(update_rearInfo), 0);
        }

        printf("Follower %d registered (%s:%d)\n",
               id, reg_msg.selfAddress.ip, reg_msg.selfAddress.udp_port);

        pthread_mutex_unlock(&mutex_client_fd_list);
    }
}


/*  Leader logic */
void leader_decide_next_state(Truck* t) {
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

void move_truck(Truck* t) {
    for (int i = 0; i < t->speed; i++) {
        switch (t->dir) {
            case NORTH: t->y++; break;
            case SOUTH: t->y--; break;
            case EAST:  t->x++; break;
            case WEST:  t->x--; break;
        }
    }
}

//Helper function for queuing commands 
void queue_commands(LeaderCommand* ldr_cmd) {
    pthread_mutex_lock(&cmd_queue.mutex);

    int next_tail = (cmd_queue.tail + 1) % CMD_QUEUE_SIZE;
    if (next_tail == cmd_queue.head) {
        /* Queue full; drop the message and log */
        fprintf(stderr, "cmd_queue full, dropping command %lu\n", ldr_cmd->command_id);
        pthread_mutex_unlock(&cmd_queue.mutex);
        return;
    }

    cmd_queue.queue[cmd_queue.tail] = *ldr_cmd;
    cmd_queue.tail = next_tail;

    pthread_cond_signal(&cmd_queue.not_empty);
    pthread_mutex_unlock(&cmd_queue.mutex);
}

// Dedicated thread function to handle sending cruise commands 
void* send_handler(void* arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&cmd_queue.mutex);

        while (cmd_queue.head == cmd_queue.tail) {
            pthread_cond_wait(&cmd_queue.not_empty, &cmd_queue.mutex);
        }

        LeaderCommand ldr_cmd = cmd_queue.queue[cmd_queue.head];
        cmd_queue.head = (cmd_queue.head + 1) % CMD_QUEUE_SIZE;

        LD_MESSAGE ldr_cmd_msg;
        ldr_cmd_msg.type = MSG_LDR_CMD;
        ldr_cmd_msg.payload.cmd = ldr_cmd;

        pthread_mutex_unlock(&cmd_queue.mutex);

        pthread_mutex_lock(&mutex_client_fd_list);
        for (int i = 0; i < follower_count; i++) {
            ssize_t sret = send(follower_fd_list[i], &ldr_cmd_msg, sizeof(ldr_cmd_msg), 0);
            if (sret < 0) {
                perror("send to follower");
            }
        }
        pthread_mutex_unlock(&mutex_client_fd_list);
    }
}
