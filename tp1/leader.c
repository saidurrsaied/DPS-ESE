//leader.c
#include "truckplatoon.h"

#include <stdio.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>



//MACROS
# define MAX_FOLLOWERS 3
# define GRID_LENGTH 600 // Vertical (Y axis)
#define GRID_WIDTH 600 // Horizontal (X axis)

//FUNC PROTOTYPES
void* accept_handler(void* arg); 
void driveForward(); 

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