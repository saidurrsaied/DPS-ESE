
//leader.c
<<<<<<< HEAD

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
#include "intruder.h"

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

void* accept_handler(void* arg);
void* send_handler(void* arg);
void leader_decide_next_state(Truck* t);
void move_truck(Truck* t);
void queue_commands(LeaderCommand* ldr_cmd);

// Handle intruder messages from followers
void handle_follower_intruder(FT_MESSAGE* msg) {
    IntruderInfo intruder = msg->payload.intruder;

    if (intruder.speed > 0) {
        printf("[LEADER] Intruder detected by follower! Adjusting cruise speed to %d\n", intruder.speed);
        // Slow down to intruder speed
        leader.speed = intruder.speed;
        leader.state = CRUISE;  // force CRUISE state
    } else {
        printf("[LEADER] Intruder cleared by follower. Resuming normal cruise\n");
        // Resume normal leader cruise
        leader.state = CRUISE;
    }
}

int main(void) {
    srand(time(NULL));
    pthread_mutex_init(&mutex_client_fd_list, NULL);

    leader = (Truck){0,0,1,NORTH,CRUISE};

    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

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
        send(follower_fd, &initialMsg, sizeof(initialMsg), 0);

        // If there is a previous follower, update it so its rear points to this new follower 
        if (id > 0) {
            LD_MESSAGE update_rearInfo = {0};
            update_rearInfo.type = MSG_LDR_UPDATE_REAR;
            update_rearInfo.payload.rearInfo.rearTruck_Address = reg_msg.selfAddress;
            update_rearInfo.payload.rearInfo.has_rearTruck = 1; 
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
=======
#include "truckplatoon.h"

#include <stdio.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>


//MACROS
# define MAX_FOLLOWERS 3
# define GRID_LENGTH 600 // Vertical (Y axis)
#define GRID_WIDTH 600 // Horizontal (X axis)

//FUNC PROTOTYPES
void* accept_handler(void* arg); 

//SOCKET
int32_t leader_socket_fd;
struct sockaddr_in leader_addr; 
socklen_t leader_addr_len; 
 
int32_t result; 
int32_t follower_fd_list[MAX_FOLLOWERS]; 
int32_t follower_count = 0; 


//THREAD 
pthread_t tid_acceptor; 
pthread_mutex_t mutex_client_fd_list; 


//Leader
truck leader; 
uint64_t cmd_id= 0; 


int main (){
// STEP 1 : prepare states and variables
    // clear fd array
    for (int i = 0; i< MAX_FOLLOWERS; i++){ follower_fd_list[i] = 0;} 
    //init mutex 
    pthread_mutex_init(&mutex_client_fd_list, NULL); 
    // init leader 
    leader.positionX = 0; 
    leader.positionY = 0; 
    leader.currentSpeed = 0; 
    leader.heading = NORTH;  
    leader.state = IDLE; 


//STEP 2: Create server socket
    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0); 
    leader_addr.sin_family = AF_INET;  
    leader_addr.sin_port = htons(LEADER_PORT); 
    leader_addr.sin_addr.s_addr = INADDR_ANY; 
    leader_addr_len = sizeof(leader_addr); 

//STEP 3: Bind
    bind(leader_socket_fd, (struct sockaddr *)&leader_addr, leader_addr_len); 

// STEP 4: LISTEN
    listen(leader_socket_fd, MAX_FOLLOWERS); 


// STEP 5: ACCEPT and assign handler thread
    pthread_create(&tid_acceptor, NULL, &accept_handler, NULL); 

//STEP 6: LOOP 
    while (1){
        // drive
        driveForward(); 
        LeaderMessage leader_msg = {.leaderState = leader, 
                                    .command_id = cmd_id, 
                                    .cmd_timestamp = now_ms()};

        pthread_mutex_lock(&mutex_client_fd_list); 
        
        for (size_t i = 0; i < follower_count; i++){
            send(follower_fd_list[i], &leader_msg, sizeof(leader_msg), 0); 
        }

        pthread_mutex_unlock(&mutex_client_fd_list); 

        cmd_id++;
        usleep(LEADER_SLEEP); // 100ms
        // calculate time
        // send instruction to followers 
        
    }
    


    //main return
    return 0; 
}

void driveForward(){


    switch (leader.heading){
    case NORTH:
        if(leader.positionY < GRID_LENGTH){leader.positionY++; }
        break;
    case SOUTH: 
        if(leader.positionY > 0){leader.positionY--; }
        break;       
        case EAST: 
        if(leader.positionX < GRID_WIDTH){leader.positionX++; }
        break;  
        case WEST: 
        if(leader.positionX > 0){leader.positionX--; }
        break;
    default:
        break;
    }


}

void* accept_handler(void* arg){
    //uint8_t follower_fd_list = *(uint8_t*) cfd; 
    while (1){
        int32_t new_follower_fd =  accept(leader_socket_fd, (struct sockaddr*)&leader_addr, &leader_addr_len ); 
        if (new_follower_fd <0){
            if(new_follower_fd == -1){
                continue; 
            }
            else {
                perror("accept failed"); 
            }

        }
        else if(follower_count >= MAX_FOLLOWERS){
            close(new_follower_fd); 
        }
        else{
            pthread_mutex_lock(&mutex_client_fd_list); 
            follower_fd_list[follower_count] = new_follower_fd;     
            printf("follower %d joined platoon", follower_count); 
            
            if(follower_count < MAX_FOLLOWERS) {
                follower_count++; 
            }
            pthread_mutex_unlock(&mutex_client_fd_list); 

        }
    }
}
>>>>>>> initial leader and follower implementation

