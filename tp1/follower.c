//follower.c

#include "truckplatoon.h"

#include <stdio.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>

#define MAX_CMD 10

//FUNCTION PROTO
void* listen_to_leader(void* arg); 


//SOCKET
int32_t follower2leader_fd; 
int32_t result; 
struct sockaddr_in leader_addr; 
socklen_t leader_addr_len; 

//THREAD 
pthread_mutex_t mutex_commandCache; 
pthread_t tid_listen_to_leader; 


LeaderMessage cmd_queue[MAX_CMD]; 
uint8_t connectedToLeader = 0; 
uint8_t command_counter = 0; 

truck follower; 

int main(){
    // init 
    pthread_mutex_init(&mutex_commandCache, NULL); 
    // init follower 
    follower.positionX = 0; 
    follower.positionY = 0; 
    follower.currentSpeed = 0; 
    follower.heading = NORTH;  
    follower.state = IDLE; 



    //STEP 1: Create socket for follower to leader connection
    follower2leader_fd = socket(AF_INET, SOCK_STREAM, 0); 
    
    leader_addr.sin_family = AF_INET; 
    leader_addr.sin_port = htons(LEADER_PORT); 
    leader_addr_len = sizeof(leader_addr); 
    if (inet_pton(AF_INET, LEADER_IP, &leader_addr.sin_addr)<= 0){
        perror("invalid leader address"); 
        return -1; 
    }

    //STEP 2: Connect to leader 
    result = connect(follower2leader_fd, (struct sockaddr*)&leader_addr, leader_addr_len); 
    if( result < 0 ){
        perror("connection to leader failed"); 
    }
    else {
        connectedToLeader = 1; 
        printf("connected to leader"); 
    }

    //recieve leader msg. Better do it in a dedicated thread and include proper thread safety. 
     pthread_create(&tid_listen_to_leader, NULL, &listen_to_leader, NULL); 

    //## TODO:create the control thread for driver state control 

    while (1){
        //read first command from queue if alailable
        if(command_counter ==0){ 
            usleep(10000); 
            continue;
        }  

        pthread_mutex_lock(&mutex_commandCache);  
        LeaderMessage current_cmd = cmd_queue[0]; 
        pthread_mutex_unlock(&mutex_commandCache);  
        //execute command 

        //##TODO: Improve State Machine
        switch (current_cmd.leaderState.state){
        case EMERGENCY_BRAKE:
            follower.currentSpeed = 0; 
            follower.state = EMERGENCY_BRAKE; 
            printf("!!EMERGENCY STOP!!"); 
            break;
        
        case SOFT_BRAKE: 
            follower.currentSpeed--; 

        // **********************************************RESUME HERE********************
        default:
            break;
        }

        // sleep 100 ms

    }
    


    
  return 1; // main return  
}

//FUNC


void* listen_to_leader(void* arg){
    while(connectedToLeader){
        LeaderMessage incoming_leader_msg; 
        int32_t bytes_received = recv(follower2leader_fd, &incoming_leader_msg, sizeof(incoming_leader_msg),0 );
        uint64_t incoming_msg_ts = now_ms(); 

        if (bytes_received <= 0){
            if(bytes_received == 0){printf("connectioin lost with the leader!!!"); 
            //## TODO: try reconnection and switch to safe autnomous driving mode
            }
            else {perror("error receiving messages from the leader"); 
            }
        }

             //If the new message was sent more than 200ms(defined) ago, it should be discarded
        else if((incoming_msg_ts - incoming_leader_msg.cmd_timestamp) > MESSAGE_TIMEOUT_MS){ 
            continue;
        }

        else{    
            pthread_mutex_lock(&mutex_commandCache);      
             // if emergency brake flag is true, add the new message to the beginning of the queue
            if(incoming_leader_msg.leaderState.state == EMERGENCY_BRAKE){
                for(int i = command_counter; i > 0 ; i--){
                    //shift existing commands to the right by 1 cell
                    cmd_queue[i] =  cmd_queue[i-1]; 
                }
                cmd_queue[0] = incoming_leader_msg; 
                
                if(command_counter < MAX_CMD){command_counter++; }; 
            }
            else{                               
                // if the new message has an id smaller than any of the old messages, the new message shall have a smaller index 
                if(command_counter < MAX_CMD){
                    //find insertion index for the new command
                    uint8_t insert_index = 0; 
                    for(insert_index = 0; insert_index < command_counter ; insert_index++){                        
                        if(cmd_queue[insert_index].command_id < incoming_leader_msg.command_id ){
                            continue; 
                        }
                        else{                                
                            break;
                        }
                    }
                    //shift the existing commands accordingly and insert the new command. 
                    for (uint8_t i = command_counter; i > insert_index ; i-- ){
                            cmd_queue [i] = cmd_queue [i-1];
                        }
                        cmd_queue[insert_index] = incoming_leader_msg; 
                    
                    // increment command counter
                    command_counter++; 
                }            
            }
            pthread_mutex_unlock(&mutex_commandCache);  
        }
    }
}