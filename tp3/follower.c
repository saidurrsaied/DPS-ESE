<<<<<<< HEAD
// follower.c


//TODOs: 

/*
    1. Add mechanism to warn leader about intruder. Send intruder info so that the leader can maintain appropriate speed and consequently 
        the whole platoon runs at the same speed (intruder's speed) 
        **Detals: Define intruder warning message struct, message type and payload. Use it on both the leader and the follower.  
                        
                Message Type: MSG_FT_INTRUDER_REPORT

*/

#define _POSIX_C_SOURCE 200809L

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
#include "follower.h"
#include "tpnet.h"
#include "intruder.h"

#define INTRUDER_PROB 10   // %

//TRUCK
Truck follower;
NetInfo rearTruck_Address;
int32_t has_rearTruck = 0;

int8_t simulation_running = 1; 

//THREAD RELATED 
pthread_t tid;
pthread_t udp_tid, tcp_tid, sm_tid;
EventQueue truck_EventQ;
pthread_t intruder_tid;
pthread_create(&intruder_tid, NULL, intruder_keyboard_listener, NULL);


// SOCKET RELATED
int udp_sock;
int32_t tcp2Leader;

// Timer to track when braking started
time_t brake_start_time = 0; 

int main(int argc, char* argv[]) {

    if (argc != 2) {
        printf("Usage: %s <LEADER_IP> <LEADER_TCP_PORT> <MY_IP> <MY_UDP_PORT>\n", argv[0]);
        return 1;
    }

    const char* leader_ip = LEADER_IP;
    uint16_t leader_tcp_port = LEADER_PORT;
    const char* my_ip = LEADER_IP;
    uint16_t my_port = atoi(argv[1]);

    /* Seed */
    srand(time(NULL) ^ getpid());

    //EVENT Queue
    event_queue_init(&truck_EventQ); 

    //1. Create TCP + UDP Sockets and Connect
    tcp2Leader = connect2Leader(); 
    udp_sock = createUDPServer(my_port); 

    // 2. Send Registration
    join_platoon(tcp2Leader, my_ip, my_port);
    
    // 3. Thread Creations 
    pthread_create(&udp_tid, NULL, udp_listener, NULL);
    pthread_create(&tcp_tid, NULL, tcp_listener, NULL);
    pthread_create(&sm_tid, NULL, truck_state_machine, NULL);
    pthread_create(&intruder_tid, NULL, intruder_keyboard_listener, NULL);
   


    //Scheduling policy and prioroty 
            /*
            set_realtime_priority(sm_tid,  SCHED_FIFO, 80); // state machine
            set_realtime_priority(udp_tid, SCHED_FIFO, 70); // emergency RX
            set_realtime_priority(tcp_tid, SCHED_FIFO, 60); // cruise RX
            */


    while(simulation_running){
        maybe_intruder(); 
        sleep(2); 
    }
    close(tcp2Leader);
    close(udp_sock);
    return 0;
}



// Dedicated thread for listening to UDP emergency messages from other trucks
void* udp_listener(void* arg) {
    FT_MESSAGE msg;
    recvfrom(udp_sock, &msg, sizeof(msg), 0, NULL, NULL);

    switch (msg.type) {
        case MSG_FT_EMERGENCY_BRAKE:
            Event emergency_evt = {.type = EVT_EMERGENCY};
            push_event(&truck_EventQ, &emergency_evt);
            break;

        case MSG_FT_POSITION:
            Event distance_evt = {.type = EVT_DISTANCE};
            distance_evt.event_data.ft_pos = msg.payload.position;
            push_event(&truck_EventQ, &distance_evt);
            break;
    }
}


//FUNC: TCP Listener 
void* tcp_listener(void* arg) {
    (void)arg; 
    LD_MESSAGE msg;
    while (1) {
            if (recv(tcp2Leader, &msg, sizeof(msg), 0) <= 0)
                break;

            switch (msg.type){
                case MSG_LDR_CMD:
                    Event cmd_evt = {.type = EVT_CRUISE_CMD, .event_data.leader_cmd = msg.payload.cmd};
                    push_event(&truck_EventQ, &cmd_evt);
                    break;
                case MSG_LDR_UPDATE_REAR: 
                    has_rearTruck = msg.payload.rearInfo.has_rearTruck;
                    if (has_rearTruck){ rearTruck_Address = msg.payload.rearInfo.rearTruck_Address;}
                    printf("[TOPOLOGY] Rear truck updated\n");
                    break; 

                case MSG_LDR_EMERGENCY_BRAKE: 
                    Event emergency_evt = {.type = EVT_EMERGENCY};
                    push_event(&truck_EventQ, &emergency_evt);
            
                default:
                    break;
            }
        }
        return NULL;
    }
    
    
//FUNC: TRUCK State Machine Thread function
void* truck_state_machine(void* arg) {
    while (1) {
        Event evnt = pop_event(&truck_EventQ);

        switch (follower.state) {

        case CRUISE:
            switch (evnt.type) {

            case EVT_CRUISE_CMD:
                follower = evnt.event_data.leader_cmd.leader;
                //TODO: implement follower speed and position change logic 
                // func: cruiseAsLeader (evnt.event_data.leader_cmd.leader)
                break;

            case EVT_DISTANCE : 
                //TODO: implement fnc to adjust speed to maintain distance with the front truck

            case EVT_INTRUDER:
                enter_intruder_follow(evnt.event_data.intruder);
                break;

            case EVT_EMERGENCY:
                enter_emergency();
                break;

            case EVT_EMERGENCY_TIMER: 
                // Timer for now,  event is not relevent during cruise 
                break; 
            default: 
                break; 
            }
            break;

        case INTRUDER_FOLLOW:
            switch (evnt.type) {

            case EVT_INTRUDER:
                // if intruder is detected again during intruder state, what to do ?
                update_intruder(evnt.event_data.intruder);
                notify_leader_intruder(evnt.event_data.intruder);
                
                if(has_rearTruck) {
                	FT_MESSAGE rear_msg = {0};
                	rear_msg.type = MSG_FT_INTRUDER_REPORT;
                	rear_msg.payload.intruder= evnt.event_data.intruder;
                	
                	sendto(udp_sock, &rear_msg, sizeof(rear_msg), 0, (struct sockaddr*)&rearTruck_Address.addr, sizeof(rearTruck_Address.addr));
                }
                
                break;

            case EVT_INTRUDER_CLEAR:
                exit_intruder_follow();
                
                IntruderInfo intruder_clear = {0};
                notify_leader_intruder(intruder_clear);
                
                if(has_rearTruck) {
                	FT_MESSAGE rear_msg = {0};
                	rear_msg.type = MSG_FT_INTRUDER_REPORT;
                	rear_msg.payload.intruder= intruder_clear;
                	
                	sendto(udp_sock, &rear_msg, sizeof(rear_msg), 0, (struct sockaddr*)&rearTruck_Address.addr, sizeof(rearTruck_Address.addr));
                }
                
                break;

            case EVT_EMERGENCY:
                enter_emergency();
                break;
            }
            break;

        case EMERGENCY_BRAKE:
            switch (evnt.type) {
            case EVT_EMERGENCY_TIMER:
                    // exit emergency 
                    // TODO:  implement exit_emergency() with proper transition action
                break;

            case EVT_EMERGENCY:
                break; // remain in emergency and do nothing. wait for timeout 
            }
            break;
        }
    }
}





//FUNC: Functioon to recover from emergenxy state 

void handle_timer(void){
    if (follower.state == EMERGENCY_BRAKE) {
        double elapsed = difftime(time(NULL), brake_start_time);
        if (elapsed >= 5.0) {
            follower.state = CRUISE;
            printf("[STATE] Emergency cleared → CRUISE\n");
        }
    }
}



// FUNC: SCHEDULING Priority and policy set 

void set_realtime_priority(pthread_t tid, int policy, int priority) {
    struct sched_param sp;
    sp.sched_priority = priority;

    int ret = pthread_setschedparam(tid, policy, &sp);
    if (ret != 0) {
        fprintf(stderr,
            "pthread_setschedparam failed: %s\n",
            strerror(ret));
    }
}
=======
//follower.c

#include "truckplatoon.h"

#include <stdio.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
>>>>>>> initial leader and follower implementation
