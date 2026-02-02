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
#include <signal.h>

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
int udp_sock = -1;
int32_t tcp2Leader = -1;

static volatile sig_atomic_t follower_shutdown_requested = 0;
static volatile sig_atomic_t follower_sig_received = 0;

static void follower_on_signal(int signo);

// Control Vars //bw
int follower_idx = 0;
int platoon_position = 0;  // Logical position in current platoon (1 = first after leader, 2 = second, etc.)
Truck front_ref;
float front_speed = 0;
float leader_base_speed = 0;

/* Join/Rejoin positioning:
 * On initial join (or process restart), follower used to snap to y=-10*id, which becomes
 * unrealistic when the leader has moved far. We now snap ONCE using the first received
 * leader position (MSG_LDR_CMD) as reference.
 */
static int needs_spawn_snap = 0;
static int have_front_position = 0;

// Intruder Context for Distance Control
IntruderInfo current_intruder = {0};  // Current active intruder
float current_target_gap = TARGET_GAP; // Dynamic target gap (10.0 normal, 50+length during intruder)

// Prototypes bw
void send_position_to_rear(void);
void move_truck(Truck *t, float dt, TurnQueue *q);
static void handle_cruise_cmd(Event *evnt);
static void handle_distance_update(Event *evnt);

MatrixClock follower_clock;  // mc

int follower_is_shutting_down(void) {
    return follower_shutdown_requested ? 1 : 0;
}

void follower_request_shutdown(const char* reason) {
    if (follower_shutdown_requested) return;
    follower_shutdown_requested = 1;
    simulation_running = 0;

    if (reason) {
        fprintf(stderr, "\n[FOLLOWER] Shutdown requested (%s)\n", reason);
    }

    /* Wake state machine */
    Event ev = {.type = EVT_SHUTDOWN};
    push_event(&truck_EventQ, &ev);

    /* Close sockets to unblock recv/recvfrom */
    pthread_mutex_lock(&mutex_sockets);
    if (tcp2Leader >= 0) {
        shutdown(tcp2Leader, SHUT_RDWR);
        close(tcp2Leader);
        tcp2Leader = -1;
    }
    if (udp_sock >= 0) {
        shutdown(udp_sock, SHUT_RDWR);
        close(udp_sock);
        udp_sock = -1;
    }
    pthread_mutex_unlock(&mutex_sockets);
}

static void follower_on_signal(int signo) {
    (void)signo;
    follower_sig_received = 1;
}

int main(int argc, char* argv[]) {

    if (argc != 2) {
        printf("Usage: %s  <MY_UDP_PORT>\n", argv[0]);
        return 1;
    }

    const char* my_ip = LEADER_IP;
    uint16_t my_port = atoi(argv[1]);

    struct sigaction sa = {0};
    sa.sa_handler = follower_on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
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
    printf("[INIT] Connected to leader (tcp fd=%d)\n", tcp2Leader);
    udp_sock = createUDPServer(my_port); 

    // 2. Send Registration
    join_platoon(tcp2Leader, my_ip, my_port);
    printf("[INIT] Registration sent to leader (udp_port=%d)\n", my_port);

    //Mutex Init
    pthread_mutex_init(&mutex_follower, NULL);
    pthread_mutex_init(&mutex_topology, NULL);
    pthread_mutex_init(&mutex_sockets, NULL);

    // 3. Thread Creations 
    
    pthread_create(&udp_tid, NULL, udp_listener, NULL);
    pthread_create(&tcp_tid, NULL, tcp_listener, NULL);
    pthread_create(&sm_tid, NULL, truck_state_machine, NULL);
    pthread_create(&intruder_tid, NULL, keyboard_listener, NULL);//meghana
    printf("[INIT] Threads started: udp_listener, tcp_listener, state_machine, keyboard_listener\n");
   


    //Scheduling policy and prioroty 
            /*
            set_realtime_priority(sm_tid,  SCHED_FIFO, 80); // state machine
            set_realtime_priority(udp_tid, SCHED_FIFO, 70); // emergency RX
            set_realtime_priority(tcp_tid, SCHED_FIFO, 60); // cruise RX
            */


    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    unsigned long phys_tick_count = 0;

    // DECOUPLED PHYSICS TIMER: Runs independently of event processing
    // This ensures continuous motion even if event queue fills up
    while (simulation_running && !follower_shutdown_requested) {
        if (follower_sig_received) {
            follower_request_shutdown("signal");
            break;
        }
        phys_tick_count++;
        next_tick.tv_nsec += (long)(FOLLOWER_PHYS_DT * 1e9);
        if (next_tick.tv_nsec >= 1e9) {
            next_tick.tv_sec += 1;
            next_tick.tv_nsec -= 1e9;
        }
        
        // ===== PHYSICS-ONLY OPERATIONS (No event queue interaction) =====
        // Move truck based on current speed (read atomically)
        move_truck(&follower, FOLLOWER_PHYS_DT, &follower_turns);
        
        // Send position to rear truck (non-blocking UDP)
        send_position_to_rear();
        
        // Print status (no event queue blocking), but decimated for readability
        if (FOLLOWER_PRINT_EVERY_N <= 1 || (phys_tick_count % (unsigned long)FOLLOWER_PRINT_EVERY_N) == 0) {
            const char *state_str = "UNKNOWN";
            switch (follower.state) {
                case CRUISE: state_str = "CRUISE"; break;
                case INTRUDER_FOLLOW: state_str = "INTRUDER_FOLLOW"; break;
                case EMERGENCY_BRAKE: state_str = "EMERGENCY_BRAKE"; break;
                case STOPPED: state_str = "STOPPED"; break;
            }
            double gap = calculate_gap(follower.x, follower.y, front_ref.x, front_ref.y);
            printf("\r[STATE: %s] [POS: %.1f,%.1f] [SPD: %.1f] [GAP: %.1f]    ",
                   state_str, (double)follower.x, (double)follower.y, (double)follower.speed, gap);
            fflush(stdout);
        }
        // ================================================================
        
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
    }

    follower_request_shutdown("main exit");

    pthread_join(intruder_tid, NULL);
    pthread_join(udp_tid, NULL);
    pthread_join(tcp_tid, NULL);
    pthread_join(sm_tid, NULL);
    return 0;
}



// Dedicated thread for listening to UDP emergency messages from other trucks
void* udp_listener(void* arg) {
    (void)arg;
    FT_MESSAGE msg;
    
    while (!follower_shutdown_requested) {
        int local_udp;
        pthread_mutex_lock(&mutex_sockets);
        local_udp = udp_sock;
        pthread_mutex_unlock(&mutex_sockets);

        if (local_udp < 0) break;

        ssize_t recv_len = recvfrom(local_udp, &msg, sizeof(msg), 0, NULL, NULL);
        if (recv_len < 0) {
            if (errno == EINTR) continue;
            if (follower_shutdown_requested) break;
            perror("recvfrom");
            follower_request_shutdown("udp recvfrom error");
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
    while (!follower_shutdown_requested) {
            int local_tcp;
            pthread_mutex_lock(&mutex_sockets);
            local_tcp = tcp2Leader;
            pthread_mutex_unlock(&mutex_sockets);

            if (local_tcp < 0) break;

            ssize_t rr = recv(local_tcp, &msg, sizeof(msg), 0);
            if (rr <= 0) {
                if (!follower_shutdown_requested) {
                    follower_request_shutdown("tcp recv closed");
                }
                break;
            }

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
                    printf("\n[TOPOLOGY] Rear updated: has_rear=%d rear_port=%d\n", has_rearTruck, rearTruck_Address.udp_port);
                    break; 

                case MSG_LDR_EMERGENCY_BRAKE: {
                    Event emergency_evt = {.type = EVT_EMERGENCY};
                    push_event(&truck_EventQ, &emergency_evt);
                    break;}

                case MSG_LDR_SPAWN:
                    /* Leader-supplied spawn pose for realistic join near current platoon */
                    pthread_mutex_lock(&mutex_follower);
                    if (needs_spawn_snap) {
                        follower.x = msg.payload.spawn.spawn_x;
                        follower.y = msg.payload.spawn.spawn_y;
                        follower.dir = msg.payload.spawn.spawn_dir;
                        needs_spawn_snap = 0;
                        have_front_position = 0;
                        printf("\n[SPAWN] Leader spawn: (%.1f,%.1f) dir=%d pos=%d\n",
                               follower.x, follower.y, follower.dir, msg.payload.spawn.assigned_id);
                    }
                    pthread_mutex_unlock(&mutex_follower);
                    break;
                    
                case MSG_LDR_ASSIGN_ID:
                    /*
                     * Leader may resend MSG_LDR_ASSIGN_ID during topology reformation.
                     * We must NOT reset/snap our physical position on reassign, otherwise
                     * trucks "jump" back to their initial start slots.
                     */
                    if (follower_idx == 0) {
                        follower_idx = msg.payload.assigned_id;
                        platoon_position = msg.payload.assigned_id;
                           /* Defer physical spawn/snap until first cruise command arrives (we need leader position). */
                           needs_spawn_snap = 1;
                           have_front_position = 0;
                           printf("\n[ID] Initial ID: %d (Platoon pos: %d)\n",
                               follower_idx, platoon_position);
                    } else {
                        follower_idx = msg.payload.assigned_id;
                        platoon_position = msg.payload.assigned_id;
                        printf("\n[ID] Updated platoon position: %d \n",
                               platoon_position);
                    }
                    break;
                default:
                    break;
            }
        }
        return NULL;
    }
    
    
//FUNC: TRUCK State Machine Thread function
void* truck_state_machine(void* arg) {
    (void)arg;
    while (!follower_shutdown_requested) {
        Event evnt = pop_event(&truck_EventQ);

        if (evnt.type == EVT_SHUTDOWN) {
            break;
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
                pthread_mutex_lock(&mutex_follower);
                handle_distance_update(&evnt);
                pthread_mutex_unlock(&mutex_follower);
                break;
            case EVT_INTRUDER:
                // INLINE state change - consistent lock pattern
                pthread_mutex_lock(&mutex_follower);
                current_intruder = evnt.event_data.intruder;
                current_target_gap = TARGET_GAP + (float)current_intruder.length;
                follower.state = INTRUDER_FOLLOW;
                if (follower.speed == 0) {
                    follower.speed = (float)current_intruder.speed;
                }
                mc_local_event(&follower_clock, follower_idx);
                pthread_mutex_unlock(&mutex_follower);
                
                notify_leader_intruder(evnt.event_data.intruder);
                printf("[STATE] Follower entering INTRUDER_FOLLOW: speed=%d, length=%d, target_gap=%.1f\n",
                       current_intruder.speed, current_intruder.length, current_target_gap);
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
                    // FIX: PROCESS cruise commands with intruder-adjusted gap!
                    pthread_mutex_lock(&mutex_follower);
                    handle_cruise_cmd(&evnt);
                    pthread_mutex_unlock(&mutex_follower);
                    break;
                case EVT_DISTANCE:
                    // FIX: PROCESS distance updates with intruder-adjusted gap!
                    pthread_mutex_lock(&mutex_follower);
                    handle_distance_update(&evnt);
                    pthread_mutex_unlock(&mutex_follower);
                    break;
                case EVT_INTRUDER:
                    // Update intruder info and recalculate target gap
                    pthread_mutex_lock(&mutex_follower);
                    current_intruder = evnt.event_data.intruder;
                    // Target gap = base gap + intruder length
                    current_target_gap = TARGET_GAP + (float)current_intruder.length;
                    // IMPORTANT: Start at intruder speed, but DON'T LOCK IT
                    // Let cruise control adjust speed to maintain the intruder gap
                    if (follower.speed == 0) {
                        follower.speed = (float)current_intruder.speed;  // Initialize only if stopped
                    }
                    pthread_mutex_unlock(&mutex_follower);
                    mc_local_event(&follower_clock, follower_idx);
                    break;

                case EVT_INTRUDER_CLEAR:
                    // Clear intruder and restore normal gap target
                    pthread_mutex_lock(&mutex_follower);
                    current_intruder = (IntruderInfo){0};
                    current_target_gap = TARGET_GAP;  // Restore normal gap
                    // INLINE state change - avoid calling exit_intruder_follow() which re-locks!
                    follower.state = CRUISE;
                    mc_local_event(&follower_clock, follower_idx);
                    pthread_mutex_unlock(&mutex_follower);
                    
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
            switch (evnt.type) {
                case EVT_CRUISE_CMD: 
                    printf("\r[EMERGENCY] Ignoring cruise cmd, in emergency mode");
                    break;
                case EVT_DISTANCE:
                    printf("\r[EMERGENCY] Ignoring distance update, in emergency mode");
                    break;
                case EVT_INTRUDER: 
                    printf("\r[EMERGENCY] Ignoring intruder event, in emergency mode");
                    break;
                case EVT_INTRUDER_CLEAR: 
                    printf("\r[EMERGENCY] Ignoring intruder clear, in emergency mode");
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
    return NULL;
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



// Helper to handle cruise command from leader
static void handle_cruise_cmd(Event *evnt) {
  leader_base_speed = evnt->event_data.leader_cmd.leader.speed;

    /*
     * Spawn/snap on initial join using current leader position.
     * Offset behind leader: platoon_position*TARGET_GAP + INTRUDER_LENGTH (join-safe margin).
     */
    if (needs_spawn_snap && platoon_position > 0) {
        float offset = ((float)platoon_position * TARGET_GAP) + (float)INTRUDER_LENGTH;
        Truck l = evnt->event_data.leader_cmd.leader;

        follower.dir = l.dir;
        switch (l.dir) {
        case NORTH:
            follower.x = l.x;
            follower.y = l.y - offset;
            break;
        case SOUTH:
            follower.x = l.x;
            follower.y = l.y + offset;
            break;
        case EAST:
            follower.x = l.x - offset;
            follower.y = l.y;
            break;
        case WEST:
            follower.x = l.x + offset;
            follower.y = l.y;
            break;
        }

        needs_spawn_snap = 0;
        printf("\n[SPAWN] Snapped near leader at (%.1f,%.1f) pos=%d offset=%.1f\n",
                     follower.x, follower.y, platoon_position, offset);
    }

    /*
     * Control source selection:
     * - platoon_position == 1: always follow leader directly.
     * - platoon_position > 1: normally follow UDP front truck.
     *   But right after join, UDP front updates may not have started yet, so temporarily
     *   follow leader until we receive at least one EVT_DISTANCE.
     */
    if (platoon_position == 1 || (platoon_position > 1 && !have_front_position)) {
        front_ref = evnt->event_data.leader_cmd.leader;
        front_speed = front_ref.speed;
        follower.speed = cruise_control_calculate_speed_with_gap(
                follower.speed, front_ref.x, front_ref.y, front_speed,
                leader_base_speed, follower.x, follower.y, current_target_gap);
    }
}

// Helper to handle distance update from truck ahead
static void handle_distance_update(Event *evnt) {
  // Use platoon_position (not follower_idx) to determine if we receive UDP updates
  // platoon_position > 1 means we follow another follower truck
  if (platoon_position > 1) {
        have_front_position = 1;
    front_ref.x = evnt->event_data.ft_pos.x;
    front_ref.y = evnt->event_data.ft_pos.y;
    front_speed = evnt->event_data.ft_pos.speed;
    // Use dynamic gap control (handles both normal and intruder cases)
    follower.speed = cruise_control_calculate_speed_with_gap(
        follower.speed, front_ref.x, front_ref.y, front_speed,
        leader_base_speed, follower.x, follower.y, current_target_gap);
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
void send_position_to_rear(void) {
    pthread_mutex_lock(&mutex_topology);
    int local_has_rear = has_rearTruck;
    NetInfo local_rear = rearTruck_Address;
    pthread_mutex_unlock(&mutex_topology);

    if (local_has_rear) {
    struct sockaddr_in dst = {.sin_family = AF_INET,
                                                            .sin_port = htons(local_rear.udp_port)};
        inet_pton(AF_INET, local_rear.ip, &dst.sin_addr);

    FT_MESSAGE msg = {.type = MSG_FT_POSITION,
                      .payload.position = {.x = follower.x,
                                           .y = follower.y,
                                           .speed = follower.speed}};
    sendto(udp_sock, &msg, sizeof(msg), 0, (struct sockaddr *)&dst,
           sizeof(dst));
  }
}
