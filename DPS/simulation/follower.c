// follower.c


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
#include <errno.h>

#include "truckplatoon.h"
#include "event.h"
#include "follower.h"
#include "tpnet.h"
#include "intruder.h"
#include "cruise_control.h"
#include "matrix_clock.h"





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
TurnQueue follower_turns; // bw

//MUTEX
pthread_mutex_t mutex_follower;
pthread_mutex_t mutex_topology;
pthread_mutex_t mutex_sockets;

// SOCKET RELATED
int udp_sock;
int32_t tcp2Leader;

// Control Vars //bw
int follower_idx = 0;
Truck front_ref;
float front_speed = 0;
float leader_base_speed = 0;

// Prototypes bw
void broadcast_status(void);
void move_truck(Truck *t, float dt, TurnQueue *q);
static void simulation_step(void);
static void handle_cruise_cmd(Event *evnt);
static void handle_distance_update(Event *evnt);


MatrixClock follower_clock;  // mc

int main(int argc, char* argv[]) {

    if (argc != 2) {
        printf("Usage: %s  <MY_UDP_PORT>\n", argv[0]);
        return 1;
    }

    const char* my_ip = LEADER_IP;
    uint16_t my_port = atoi(argv[1]);
    
    mc_init(&follower_clock); //matrix clock initialization

    /* Seed */
    srand(time(NULL) ^ getpid());

      // Initial position (placeholder, will be snapped by TCP listener)
    follower = (Truck) {.x = 0.0f, .y = -10.0f, .speed = 0, .dir = NORTH, .state = CRUISE};

    //EVENT Queue
    event_queue_init(&truck_EventQ); 
    turn_queue_init(&follower_turns); // bw

    //1. Create TCP + UDP Sockets and Connect
    tcp2Leader = connect2Leader(); 
    udp_sock = createUDPServer(my_port); 

    // 2. Send Registration
    join_platoon(tcp2Leader, my_ip, my_port);

    //Mutex Init
    pthread_mutex_init(&mutex_follower, NULL);
    pthread_mutex_init(&mutex_topology, NULL);
    pthread_mutex_init(&mutex_sockets, NULL);

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


    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    while (simulation_running) {
        next_tick.tv_nsec += (long)(SIM_DT * 1e9);
        if (next_tick.tv_nsec >= 1e9) {
        next_tick.tv_sec += 1;
        next_tick.tv_nsec -= 1e9;
        }
        simulation_step();
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
        ssize_t recv_len = recvfrom(udp_sock, &msg, sizeof(msg), 0, NULL, NULL);
        if (recv_len < 0) {
            perror("recvfrom");
            break;
        }

        switch (msg.type) {
            case MSG_FT_EMERGENCY_BRAKE:{
                Event emergency_evt = {.type = EVT_EMERGENCY};
                push_event(&truck_EventQ, &emergency_evt);
                break;}

            case MSG_FT_POSITION:{
                Event distance_evt = {.type = EVT_DISTANCE};
                distance_evt.event_data.ft_pos = msg.payload.position;
                push_event(&truck_EventQ, &distance_evt);
                break;}
                
            case MSG_FT_INTRUDER_REPORT:
                // Potential future use
                break;
                
            default:
                break;
        }
    }
    return NULL;
}


//FUNC: TCP Listener 
void* tcp_listener(void* arg) {

    LD_MESSAGE msg;
    while (1) {
            printf("[TCP]"); 
            if (recv(tcp2Leader, &msg, sizeof(msg), 0) <= 0)
                break;

            switch (msg.type){
                case MSG_LDR_CMD:
                    if (msg.payload.cmd.is_turning_event) {
                    turn_queue_push(&follower_turns, msg.payload.cmd.turn_point_x, msg.payload.cmd.turn_point_y, msg.payload.cmd.turn_dir);
                    }
                    leader_base_speed = msg.payload.cmd.leader.speed;
                    Event cmd_evt = {.type = EVT_CRUISE_CMD, .event_data.leader_cmd = msg.payload.cmd};
                    push_event(&truck_EventQ, &cmd_evt);
                    break;
                case MSG_LDR_UPDATE_REAR: 
                    pthread_mutex_lock(&mutex_topology);
                    has_rearTruck = msg.payload.rearInfo.has_rearTruck;
                    if (has_rearTruck){ rearTruck_Address = msg.payload.rearInfo.rearTruck_Address;}
                    pthread_mutex_unlock(&mutex_topology);
                    printf("[TOPOLOGY] Rear truck updated\n");
                    break; 

                case MSG_LDR_EMERGENCY_BRAKE: {
                    Event emergency_evt = {.type = EVT_EMERGENCY};
                    push_event(&truck_EventQ, &emergency_evt);
                    break;}
                    
                case MSG_LDR_ASSIGN_ID:
                    follower_idx = msg.payload.assigned_id;
                    follower.y = -10.0f * (float)follower_idx; // Snap to position
                    printf("\n[ID] Assigned ID: %d (Starting Y: %.1f)\n", follower_idx, follower.y);
                    break;
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
                pthread_mutex_lock(&mutex_follower);
                handle_cruise_cmd(&evnt);
                pthread_mutex_unlock(&mutex_follower);
                break;

            case EVT_DISTANCE : 
                //adjust_distance_from_front(evnt.event_data.ft_pos);
                handle_distance_update(&evnt);
                break;
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
                    printf("[INTRUDER] Ignoring cruise cmd while handling intruder\n");
                    break;
                case EVT_DISTANCE:
                    printf("[INTRUDER] Ignoring distance update while handling intruder\n");
                    break;
                case EVT_INTRUDER:
                    // if intruder is detected again during intruder state, what to do ?
                    update_intruder(evnt.event_data.intruder);      
                    mc_local_event(&follower_clock, follower_idx); //mc: increment for local intruder follow         
                    break;

                case EVT_INTRUDER_CLEAR:
                    exit_intruder_follow();
                    IntruderInfo intruder_clear = {0};
                    notify_leader_intruder(intruder_clear);
                    mc_local_event(&follower_clock, follower_idx); //mc: increment for local intruder clear
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
                    printf("[EMERGENCY] Ignoring cruise cmd, in emergency mode\n");
                    break;
                case EVT_DISTANCE:
                    printf("[EMERGENCY] Ignoring distance update, in emergency mode\n");
                    break;
                case EVT_INTRUDER: 
                    printf("[EMERGENCY] Ignoring intruder event, in emergency mode\n");
                    break;
                case EVT_INTRUDER_CLEAR: 
                    printf("[EMERGENCY] Ignoring intruder clear, in emergency mode\n");
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




/*Function for maintainoing distance*/

void adjust_distance_from_front(FT_POSITION front_pos) {
    pthread_mutex_lock(&mutex_follower);

    // Simple distance calculation
    int32_t dx = front_pos.x - follower.x;
    int32_t dy = front_pos.y - follower.y;
    int32_t dist_squared = dx*dx + dy*dy;
    int32_t current_dist = (int32_t)sqrt(dist_squared);

    // Adjust speed based on distance
    if (current_dist < SAFE_DISTANCE) {
    // Too close - reduce speed
    if (follower.speed > 1) follower.speed--;
    printf("[DISTANCE] Too close (%d m), reducing speed to %.1f\n", 
            current_dist, follower.speed);
    } else if (current_dist > SAFE_DISTANCE + 5) {
    // Too far - increase speed
    if (follower.speed < 5) follower.speed++;
    printf("[DISTANCE] Too far (%d m), increasing speed to %.1f\n", 
            current_dist, follower.speed);
    }

    pthread_mutex_unlock(&mutex_follower);
}


/*BW*/

// FUNC: Shared simulation step for event-driven updates
static void simulation_step(void) {
  // 1. Move & Turn, UDP Status updates
  move_truck(&follower, SIM_DT, &follower_turns);
  broadcast_status();

  // 2. Print status line
  printf(
      "\rF%d: POS(%.1f,%.1f) SPD=%.1f FRONT_Y=%.1f GAP=%.1f    ", follower_idx,
      (double)follower.x, (double)follower.y, (double)follower.speed,
      (double)front_ref.y,
      (double)calculate_gap(follower.x, follower.y, front_ref.x, front_ref.y));
  fflush(stdout);
}

// Helper to handle cruise command from leader
static void handle_cruise_cmd(Event *evnt) {
  leader_base_speed = evnt->event_data.leader_cmd.leader.speed;
  if (follower_idx == 1) {
    front_ref = evnt->event_data.leader_cmd.leader;
    front_speed = front_ref.speed;
    follower.speed = cruise_control_calculate_speed(
        follower.speed, front_ref.x, front_ref.y, front_speed,
        leader_base_speed, follower.x, follower.y);
  }
}

// Helper to handle distance update from truck ahead
static void handle_distance_update(Event *evnt) {
  if (follower_idx > 1) {
    front_ref.x = evnt->event_data.ft_pos.x;
    front_ref.y = evnt->event_data.ft_pos.y;
    front_speed = evnt->event_data.ft_pos.speed;
    follower.speed = cruise_control_calculate_speed(
        follower.speed, front_ref.x, front_ref.y, front_speed,
        leader_base_speed, follower.x, follower.y);
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
                               &snapped_y, follower_idx)) {
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
