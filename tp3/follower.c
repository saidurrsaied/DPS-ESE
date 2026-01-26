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


//TRUCK
Truck follower;
NetInfo rearTruck_Address;
int32_t has_rearTruck = 0;

int8_t simulation_running = 1; 

//THREAD RELATED 
pthread_t tid;
pthread_t udp_tid, tcp_tid, sm_tid;
pthread_t intruder_tid; //meghana
EventQueue truck_EventQ;

// SOCKET RELATED
int udp_sock;
int32_t tcp2Leader;


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
    pthread_create(&intruder_tid, NULL, keyboard_listener, NULL);//meghana
    printf("Created Threads...\n");
   


    //Scheduling policy and prioroty 
            /*
            set_realtime_priority(sm_tid,  SCHED_FIFO, 80); // state machine
            set_realtime_priority(udp_tid, SCHED_FIFO, 70); // emergency RX
            set_realtime_priority(tcp_tid, SCHED_FIFO, 60); // cruise RX
            */


    while(1){
        
        //YOU WAIT HERE 
    }
    close(tcp2Leader);
    close(udp_sock);
    return 0;
}



// Dedicated thread for listening to UDP emergency messages from other trucks
void* udp_listener(void* arg) {
    FT_MESSAGE msg;
    while(1){
        printf("[UDP]"); 
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
            case MSG_FT_INTRUDER_REPORT: 
                // These type of message is not expected here
                break;
        }
    }
}


//FUNC: TCP Listener 
void* tcp_listener(void* arg) {
    (void)arg; 
    LD_MESSAGE msg;
    while (1) {
            printf("[TCP]"); 
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
        printf("[FSM]"); 
        Event evnt = pop_event(&truck_EventQ);

        switch (follower.state) {
        case CRUISE:
            printf("[STATE = CRUISE]"); 
            switch (evnt.type) {

            case EVT_CRUISE_CMD:
                follower = evnt.event_data.leader_cmd.leader;
                //TODO: implement follower speed and position change logic 
                // func: cruiseAsLeader (evnt.event_data.leader_cmd.leader)
                break;

            case EVT_DISTANCE : 
                //TODO: implement fnc to adjust speed to maintain distance with the front truck

            case EVT_INTRUDER:
                notify_leader_intruder(evnt.event_data.intruder);
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
            printf("[STATE = INTRUDER_FOLLOW]"); 
            switch (evnt.type) { 
                case EVT_CRUISE_CMD: 
                    push_event(&truck_EventQ,&evnt); //defere
                    break;
                case EVT_DISTANCE:
                    push_event(&truck_EventQ,&evnt); //defere
                    break;
                case EVT_INTRUDER:
                    // if intruder is detected again during intruder state, what to do ?
                    update_intruder(evnt.event_data.intruder);               
                    break;

                case EVT_INTRUDER_CLEAR:
                    exit_intruder_follow();
                    IntruderInfo intruder_clear = {0};
                    notify_leader_intruder(intruder_clear);
                    break;

                case EVT_EMERGENCY:
                    enter_emergency();
                    break;
                case EVT_EMERGENCY_TIMER: 
                    break; 
                }
                break;

        case EMERGENCY_BRAKE:
            printf("[STATE = EMERGENCY_BRAKE]"); 
            switch (evnt.type) {
                case EVT_CRUISE_CMD: 
                    push_event(&truck_EventQ,&evnt); //defere
                    break;
                case EVT_DISTANCE:
                    push_event(&truck_EventQ,&evnt); //defere
                    break;
                case EVT_INTRUDER: 
                    push_event(&truck_EventQ,&evnt); //defere
                    break;
                case EVT_INTRUDER_CLEAR: 
                    push_event(&truck_EventQ,&evnt); //defere
                    break;
                case EVT_EMERGENCY_TIMER:
                        exit_emergency(); 
                    break;

                case EVT_EMERGENCY:
                    break; // remain in emergency and do nothing. wait for timeout 
            }
            break;
        case STOPPED: 
            printf("[STATE = EMERGENCY_BRAKE]"); 
            
        }
    }
    pthread_exit(pthread_self); // Check if correct
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
