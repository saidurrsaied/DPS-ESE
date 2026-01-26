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
#include <math.h>

#include "truckplatoon.h"
#include "event.h"
#include "follower.h"
#include "tpnet.h"
#include "cruise_control.h"
#include "turning.h"

#define _DEFAULT_SOURCE

//TRUCK
Truck follower;
NetInfo rearTruck_Address;
int32_t has_rearTruck = 0;

int8_t simulation_running = 1; 

//THREAD RELATED 
pthread_t tid;
pthread_t udp_tid, tcp_tid, sm_tid;
EventQueue truck_EventQ;
TurnQueue follower_turns; //bw

// Control Vars //bw
int follower_idx = 0;
Truck front_ref;
float front_speed = 0;
float leader_base_speed = 0;


// SOCKET RELATED
int udp_sock;
int32_t tcp2Leader;


// Prototypes
void broadcast_status(void);
void move_truck(Truck *t, float dt, TurnQueue *q);

int main(int argc, char* argv[]) {

    if (argc != 3) {
        printf("Usage: %s <LEADER_IP> <LEADER_TCP_PORT> <MY_IP> <MY_UDP_PORT>\n",
        argv[0]);
    return 1;
    }

    const char* leader_ip = LEADER_IP;
    uint16_t leader_tcp_port = LEADER_PORT;
    const char* my_ip = LEADER_IP;
    uint16_t my_port = atoi(argv[1]);
    follower_idx = atoi(argv[2]);

    /* Seed */
    srand(time(NULL) ^ getpid());

    //EVENT Queue
    event_queue_init(&truck_EventQ); 
    turn_queue_init(&follower_turns);//bw

  // Initial position for distance maintenance testing (standardized start)
  follower = (Truck){.x = 0,
                     .y = -10.0f * (float)follower_idx,
                     .speed = 0,
                     .dir = NORTH,
                     .state = STOPPED};

    //1. Create TCP + UDP Sockets and Connect
    tcp2Leader = connect2Leader(); 
    udp_sock = createUDPServer(my_port); 

    // 2. Send Registration
    join_platoon(tcp2Leader, my_ip, my_port);
    
    // 3. Thread Creations 
    pthread_create(&udp_tid, NULL, udp_listener, NULL);
    pthread_create(&tcp_tid, NULL, tcp_listener, NULL);
    pthread_create(&sm_tid, NULL, truck_state_machine, NULL);
   


    //Scheduling policy and prioroty 
            /*
            set_realtime_priority(sm_tid,  SCHED_FIFO, 80); // state machine
            set_realtime_priority(udp_tid, SCHED_FIFO, 70); // emergency RX
            set_realtime_priority(tcp_tid, SCHED_FIFO, 60); // cruise RX
            */


  struct timespec next_tick;
  clock_gettime(CLOCK_MONOTONIC, &next_tick);

  while (simulation_running) {
    next_tick.tv_nsec += (long)(SIM_DT * 1e9);
    if (next_tick.tv_nsec >= 1e9) {
      next_tick.tv_sec += 1;
      next_tick.tv_nsec -= 1e9;
    }

    // 1. Move & Turn (at 10Hz)
    move_truck(&follower, SIM_DT, &follower_turns);

    // 2. Status updates and other logic
    broadcast_status();
    //handle_timer(); // recover from emergency if needed

    printf("\rF%d: POS(%.1f,%.1f) SPD=%.1f FRONT_Y=%.1f GAP=%.1f    ",
           follower_idx, (double)follower.x, (double)follower.y,
           (double)follower.speed, (double)front_ref.y,
           (double)calculate_gap(follower.x, follower.y, front_ref.x,
                                 front_ref.y));
    fflush(stdout);

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
  }
    close(tcp2Leader);
    close(udp_sock);
    return 0;
}



// Dedicated thread for listening to UDP emergency messages from other trucks
void* udp_listener(void* arg) {
  (void)arg;
  FT_MESSAGE msg;
  while (1) {
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
  return NULL;
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
                      if (msg.payload.cmd.is_turning_event) {
        turn_queue_push(&follower_turns, msg.payload.cmd.turn_point_x,
                        msg.payload.cmd.turn_point_y, msg.payload.cmd.turn_dir);
      }
      leader_base_speed = msg.payload.cmd.leader.speed;
      // DEBUG: printf("\n[TCP] Received CMD %lu, leader_speed=%.1f\n",
      // msg.payload.cmd.command_id, leader_base_speed);
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
    case ACCELERATE:
    case DECELERATE:
    case TURNING:
    case STOPPED:
      switch (evnt.type) {
      case EVT_CRUISE_CMD:
        // Update information from leader
        leader_base_speed = evnt.event_data.leader_cmd.leader.speed;
        if (follower_idx == 1) {
          front_ref = evnt.event_data.leader_cmd.leader;
          front_speed = front_ref.speed;
        }
        // Calculate speed based on new data
        follower.speed = cruise_control_calculate_speed(
            follower.speed, front_ref.x, front_ref.y, front_speed,
            leader_base_speed, follower.x, follower.y);
        break;

      case EVT_DISTANCE:
        // Update information from truck ahead
        if (follower_idx > 1) {
          front_ref.x = evnt.event_data.ft_pos.x;
          front_ref.y = evnt.event_data.ft_pos.y;
          front_speed = evnt.event_data.ft_pos.speed;
        }
        // Calculate speed based on new data
        follower.speed = cruise_control_calculate_speed(
            follower.speed, front_ref.x, front_ref.y, front_speed,
            leader_base_speed, follower.x, follower.y);
        break;
        
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
                break;

            case EVT_INTRUDER_CLEAR:
                exit_intruder_follow();
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


// FUNC: Move Truck
void move_truck(Truck *t, float dt, TurnQueue *q) {
  // A. Physical Movement
  float dx = 0, dy = 0;
  switch (t->dir) {
  case NORTH:
    dy = t->speed * dt;
    break;
  case SOUTH:
    dy = -t->speed * dt;
    break;
  case EAST:
    dx = t->speed * dt;
    break;
  case WEST:
    dx = -t->speed * dt;
    break;
  }
  t->x += dx;
  t->y += dy;

  // B. Turning Logic
  DIRECTION next_dir;
  float snapped_x, snapped_y;
  if (turning_check_and_update(q, t->x, t->y, t->dir, &next_dir, &snapped_x,
                               &snapped_y)) {
    t->x = snapped_x;
    t->y = snapped_y;
    t->dir = next_dir;
    printf("\n[TURN] Executed turn to %d at (%.2f, %.2f)\n", next_dir, t->x,
           t->y);
  }
}

// FUNC: Broadcast status to rear truck
void broadcast_status(void) {
  if (has_rearTruck) {
    struct sockaddr_in dst = {.sin_family = AF_INET,
                              .sin_port = htons(rearTruck_Address.udp_port)};
    inet_pton(AF_INET, rearTruck_Address.ip, &dst.sin_addr);

    FT_MESSAGE msg = {.type = MSG_FT_POSITION,
                      .payload.position = {.x = follower.x,
                                           .y = follower.y,
                                           .speed = follower.speed}};
    sendto(udp_sock, &msg, sizeof(msg), 0, (struct sockaddr *)&dst,
           sizeof(dst));
  }
}
