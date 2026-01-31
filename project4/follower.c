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

/* Heartbeat settings */ //Rajdeep
#define HEARTBEAT_INTERVAL_MS 100 //Rajdeep
#define HEARTBEAT_RECONNECT_MS 500 //Rajdeep
#define HEARTBEAT_MAX_ATTEMPTS 10 //Rajdeep

static uint64_t monotonic_ms(void); //Rajdeep
static void format_wall_time(time_t t, char *buf, size_t len); //Rajdeep
static const char* state_label(TRUCK_CONTROL_STATE state); //Rajdeep
static void sleep_ms(int ms); //Rajdeep
static void* leader_heartbeat_monitor(void* arg); //Rajdeep
static void request_stop_event(void); //Rajdeep





//TRUCK
Truck follower;
NetInfo rearTruck_Address;
int32_t has_rearTruck = 0;

int8_t simulation_running = 1; 

//THREAD RELATED 
pthread_t tid;
pthread_t udp_tid, tcp_tid, sm_tid;
pthread_t intruder_tid; //meghana
pthread_t heartbeat_tid; //Rajdeep
EventQueue truck_EventQ;
TurnQueue follower_turns; // bw

//MUTEX
pthread_mutex_t mutex_follower;
pthread_mutex_t mutex_topology;
pthread_mutex_t mutex_sockets;
pthread_mutex_t mutex_heartbeat; //Rajdeep

// SOCKET RELATED
int udp_sock;
int32_t tcp2Leader;

// Heartbeat state //Rajdeep
int is_leader_active = 0; //Rajdeep
uint64_t last_heartbeat_ms = 0; //Rajdeep
uint64_t last_reconnect_attempt_ms = 0; //Rajdeep
int reconnect_attempts = 0; //Rajdeep
int reconnect_gave_up = 0; //Rajdeep
int stop_requested = 0; //Rajdeep
int reconnect_notice_printed = 0; //Rajdeep
int heartbeat_printed = 0; //Rajdeep
time_t last_heartbeat_wall = 0; //Rajdeep

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
    pthread_mutex_init(&mutex_heartbeat, NULL); //Rajdeep
    last_heartbeat_ms = monotonic_ms(); //Rajdeep
    last_heartbeat_wall = time(NULL); //Rajdeep
    last_reconnect_attempt_ms = last_heartbeat_ms; //Rajdeep
    is_leader_active = 1; //Rajdeep

    // 3. Thread Creations 
    
    pthread_create(&udp_tid, NULL, udp_listener, NULL);
    pthread_create(&tcp_tid, NULL, tcp_listener, NULL);
    pthread_create(&sm_tid, NULL, truck_state_machine, NULL);
    pthread_create(&intruder_tid, NULL, keyboard_listener, NULL);//meghana
    pthread_create(&heartbeat_tid, NULL, leader_heartbeat_monitor, NULL); //Rajdeep
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
    (void)arg; //Rajdeep

    LD_MESSAGE msg;
    while (1) {
            /* TCP tag will be shown in the consolidated status line */ //Rajdeep
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
                    
                case MSG_LDR_HEARTBEAT: { //Rajdeep
                    time_t now_wall = time(NULL); //Rajdeep
                    int should_print = 0; //Rajdeep

                    pthread_mutex_lock(&mutex_heartbeat); //Rajdeep
                    last_heartbeat_ms = monotonic_ms(); //Rajdeep
                    last_reconnect_attempt_ms = last_heartbeat_ms; //Rajdeep
                    last_heartbeat_wall = now_wall; //Rajdeep
                    is_leader_active = 1; //Rajdeep
                    reconnect_attempts = 0; //Rajdeep
                    reconnect_gave_up = 0; //Rajdeep
                    reconnect_notice_printed = 0; //Rajdeep
                    if (!heartbeat_printed) { //Rajdeep
                        heartbeat_printed = 1; //Rajdeep
                        should_print = 1; //Rajdeep
                    }
                    pthread_mutex_unlock(&mutex_heartbeat); //Rajdeep

                    if (should_print) { //Rajdeep
                        char ts[32]; //Rajdeep
                        format_wall_time(now_wall, ts, sizeof(ts)); //Rajdeep
                        printf("leader active, leader heartbeat received at %s\n", ts); //Rajdeep
                    }
                    break;} //Rajdeep

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


static uint64_t monotonic_ms(void) { //Rajdeep
    struct timespec ts; //Rajdeep
    clock_gettime(CLOCK_MONOTONIC, &ts); //Rajdeep
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; //Rajdeep
} //Rajdeep

static void format_wall_time(time_t t, char *buf, size_t len) { //Rajdeep
    struct tm tm_local; //Rajdeep
    localtime_r(&t, &tm_local); //Rajdeep
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_local); //Rajdeep
} //Rajdeep

static void sleep_ms(int ms) { //Rajdeep
    struct timespec ts; //Rajdeep
    ts.tv_sec = ms / 1000; //Rajdeep
    ts.tv_nsec = (long)(ms % 1000) * 1000000L; //Rajdeep
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { //Rajdeep
        // retry
    }
} //Rajdeep

static const char* state_label(TRUCK_CONTROL_STATE state) { //Rajdeep
    switch (state) { //Rajdeep
        case CRUISE: return "CRUISE"; //Rajdeep
        case EMERGENCY_BRAKE: return "EMERGENCY_BRAKE"; //Rajdeep
        case STOPPED: return "STOPPED"; //Rajdeep
        case INTRUDER_FOLLOW: return "INTRUDER_FOLLOW"; //Rajdeep
        default: return "UNKNOWN"; //Rajdeep
    }
} //Rajdeep

static void request_stop_event(void) { //Rajdeep
    pthread_mutex_lock(&mutex_heartbeat); //Rajdeep
    if (stop_requested) { //Rajdeep
        pthread_mutex_unlock(&mutex_heartbeat); //Rajdeep
        return;
    }
    stop_requested = 1; //Rajdeep
    pthread_mutex_unlock(&mutex_heartbeat); //Rajdeep

    Event stop_evt = {.type = EVT_EMERGENCY_TIMER}; //Rajdeep
    push_event(&truck_EventQ, &stop_evt); //Rajdeep
} //Rajdeep

static void* leader_heartbeat_monitor(void* arg) { //Rajdeep
    (void)arg; //Rajdeep

    while (1) { //Rajdeep
        sleep_ms(HEARTBEAT_INTERVAL_MS); //Rajdeep
        uint64_t now = monotonic_ms(); //Rajdeep

        pthread_mutex_lock(&mutex_heartbeat); //Rajdeep
        uint64_t last_hb = last_heartbeat_ms; //Rajdeep
        uint64_t last_attempt = last_reconnect_attempt_ms; //Rajdeep
        int attempts = reconnect_attempts; //Rajdeep
        int gave_up = reconnect_gave_up; //Rajdeep
        int notice_printed = reconnect_notice_printed; //Rajdeep
        time_t last_wall = last_heartbeat_wall; //Rajdeep
        pthread_mutex_unlock(&mutex_heartbeat); //Rajdeep

        if (last_hb == 0) { //Rajdeep
            continue;
        }

        if (now - last_hb < HEARTBEAT_RECONNECT_MS) { //Rajdeep
            continue;
        }

        if (gave_up) { //Rajdeep
            continue;
        }

        if (now - last_attempt >= HEARTBEAT_RECONNECT_MS) { //Rajdeep
            if (!notice_printed) { //Rajdeep
                char ts[32]; //Rajdeep
                format_wall_time(last_wall, ts, sizeof(ts)); //Rajdeep
                printf("leader last heartbeat time - %s\n", ts); //Rajdeep
                printf("trying to reconnect with leader\n"); //Rajdeep
            }

            pthread_mutex_lock(&mutex_heartbeat); //Rajdeep
            reconnect_attempts++; //Rajdeep
            attempts = reconnect_attempts; //Rajdeep
            last_reconnect_attempt_ms = now; //Rajdeep
            is_leader_active = 0; //Rajdeep
            reconnect_notice_printed = 1; //Rajdeep
            heartbeat_printed = 0; //Rajdeep
            if (reconnect_attempts >= HEARTBEAT_MAX_ATTEMPTS) { //Rajdeep
                reconnect_gave_up = 1; //Rajdeep
            }
            pthread_mutex_unlock(&mutex_heartbeat); //Rajdeep

            if (attempts >= HEARTBEAT_MAX_ATTEMPTS) { //Rajdeep
                printf("Leader is disconnected.\n"); //Rajdeep
                pthread_mutex_lock(&mutex_sockets); //Rajdeep
                if (tcp2Leader >= 0) { //Rajdeep
                    close(tcp2Leader); //Rajdeep
                    tcp2Leader = -1; //Rajdeep
                }
                pthread_mutex_unlock(&mutex_sockets); //Rajdeep
                request_stop_event(); //Rajdeep
            }
        }
    }

    return NULL;
}


//FUNC: TRUCK State Machine Thread function
void* truck_state_machine(void* arg) {
    (void)arg; //Rajdeep
    while (1) {
        Event evnt = pop_event(&truck_EventQ);

        pthread_mutex_lock(&mutex_heartbeat); //Rajdeep
        int stop_now = stop_requested; //Rajdeep
        if (stop_requested) { //Rajdeep
            stop_requested = 0; //Rajdeep
        }
        pthread_mutex_unlock(&mutex_heartbeat); //Rajdeep

        if (stop_now) { //Rajdeep
            pthread_mutex_lock(&mutex_follower); //Rajdeep
            follower.state = STOPPED; //Rajdeep
            follower.speed = 0; //Rajdeep
            pthread_mutex_unlock(&mutex_follower); //Rajdeep
            continue;
        }

        switch (follower.state) {
        case CRUISE:
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

// FUNC: Shared simulation step for event-driven updates //Rajdeep
static void simulation_step(void) { //Rajdeep
  // 1. Move & Turn, UDP Status updates
  move_truck(&follower, SIM_DT, &follower_turns);
  broadcast_status();

  // 2. Print status line only when leader is active (avoid spam on disconnect) //Rajdeep
  pthread_mutex_lock(&mutex_heartbeat); //Rajdeep
  int leader_active = is_leader_active; //Rajdeep
  int reconnecting = reconnect_notice_printed || reconnect_gave_up; //Rajdeep
  pthread_mutex_unlock(&mutex_heartbeat); //Rajdeep

  pthread_mutex_lock(&mutex_follower); //Rajdeep
  TRUCK_CONTROL_STATE cur_state = follower.state; //Rajdeep
  int stopped = (cur_state == STOPPED); //Rajdeep
  pthread_mutex_unlock(&mutex_follower); //Rajdeep

  if (leader_active && !reconnecting && !stopped) { //Rajdeep
    printf( //Rajdeep
        "[TCP][STATE = %s] F%d: POS(%.1f,%.1f) SPD=%.1f FRONT_Y=%.1f GAP=%.1f", //Rajdeep
        state_label(cur_state), //Rajdeep
        follower_idx, //Rajdeep
        (double)follower.x, (double)follower.y, (double)follower.speed, //Rajdeep
        (double)front_ref.y, //Rajdeep
        (double)calculate_gap(follower.x, follower.y, front_ref.x, front_ref.y)); //Rajdeep
    printf("\n"); //Rajdeep
    fflush(stdout); //Rajdeep
  }
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
