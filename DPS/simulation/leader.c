//leader.c


#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

#include "truckplatoon.h"
#include "matrix_clock.h"
#include "event.h"
#include "tpnet.h"
#include "intruder.h"
#include "rt_wcet.h"

/* Leader truck state */
int leader_socket_fd = -1;


FollowerSession followers[MAX_FOLLOWERS];
pthread_mutex_t mutex_followers; 

#define MIN_FOLLOWERS 3
int formation_complete = 0;
int active_follower_count = 0;

pthread_mutex_t mutex_client_fd_list;
pthread_mutex_t mutex_leader_state;
pthread_t sender_tid;
pthread_t acceptor_tid;
pthread_t receiver_tid;  
pthread_t input_tid;
pthread_t state_tid; 

/* Leader Event Queue */
EventQueue leader_EventQ;

Truck leader;
uint64_t cmd_id = 0;
CommandQueue cmd_queue; 
int pending_turn = 0;
DIRECTION next_turn_dir;
int leader_intruder_length = 0;

/* If set, leader keeps connections but stops sending periodic cruise commands.
 * Used to simulate network/leader stalls so follower watchdog can trigger.
 */
static int leader_stale_mode = 0;

MatrixClock leader_clock; //matrix clock declaration

static volatile sig_atomic_t leader_shutdown_requested = 0;
static volatile sig_atomic_t leader_sig_received = 0;

static void leader_request_shutdown(const char* reason);
static void leader_close_all_sockets(void);
static void leader_on_signal(int signo);

void* accept_handler(void* arg);
void* send_handler(void* arg);
void* follower_message_receiver(void* arg);
void *input_handler(void *arg); //BW
void* leader_state_machine(void* arg); //MSR
void move_truck(Truck* t, float dt);
void queue_commands(LeaderCommand* ldr_cmd);
void broadcast_emergency_to_followers(void);

void register_new_follower(int fd, FollowerRegisterMsg* reg_msg);
void broadcast_to_followers(const void* msg_data, size_t msg_len);
static void compact_followers_locked(void);
static void send_spawn_to_follower(int fd, int assigned_id);

/* === WCET instrumentation (thread CPU time) === */
static RtWcetStat wcet_leader_tick_iter = RT_WCET_STAT_INIT("L1 tick-producer iter");
static RtWcetStat wcet_leader_fsm_event = RT_WCET_STAT_INIT("L2 FSM per-event");
static RtWcetStat wcet_leader_accept_iter = RT_WCET_STAT_INIT("L5 accept loop iter");
static RtWcetStat wcet_leader_rx_dispatch = RT_WCET_STAT_INIT("L4 rx msg->event");
static RtWcetStat wcet_leader_send_cmd = RT_WCET_STAT_INIT("L3 send one cmd");

static void leader_wcet_print_summary(void) {
    const RtWcetStat stats[] = {
        wcet_leader_tick_iter,
        wcet_leader_fsm_event,
        wcet_leader_accept_iter,
        wcet_leader_rx_dispatch,
        wcet_leader_send_cmd,
    };
    rt_wcet_stats_print_table(stderr, "Leader", stats, sizeof(stats) / sizeof(stats[0]));
}

#ifndef TEST_LEADER
int main(int argc, char** argv) {
    uint16_t leader_port = LEADER_PORT;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [LEADER_TCP_PORT]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        char* endp = NULL;
        long p = strtol(argv[1], &endp, 10);
        if (endp == argv[1] || *endp != '\0' || p <= 0 || p > 65535) {
            fprintf(stderr, "Invalid port: %s\nUsage: %s [LEADER_TCP_PORT]\n", argv[1], argv[0]);
            return 1;
        }
        leader_port = (uint16_t)p;
    }

    srand(time(NULL));

    struct sigaction sa = {0};
    sa.sa_handler = leader_on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    pthread_mutex_init(&mutex_client_fd_list, NULL);
    pthread_mutex_init(&mutex_leader_state, NULL);

    /* Initialize follower sessions */
    pthread_mutex_init(&mutex_followers, NULL);
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        followers[i].active = 0;
        followers[i].fd = -1;
        followers[i].id = i + 1; /* logical IDs start at 1 */
        followers[i].address.udp_port = 0;
        followers[i].address.ip[0] = '\0';
    }

//Init leader
    leader = (Truck){.x = 0.0f, .y = 0.0f, .speed = 0.0f, .dir = NORTH, .state = STOPPED};
//Init Event queue
    event_queue_init(&leader_EventQ);
//Leader FSM
    if (pthread_create(&state_tid, NULL, leader_state_machine, NULL) != 0) {
        perror("pthread_create state");
        return 1;
    }   
    mc_init(&leader_clock); // MHK:  matrix clock

//Leader TCP Sock
    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(leader_port),
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

    /* Initialize command queue */
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
    if (pthread_create(&receiver_tid, NULL, follower_message_receiver, NULL) != 0) {
        perror("pthread_create receiver");
        return 1;
    }
    if (pthread_create(&input_tid, NULL, input_handler, NULL) != 0) {
    perror("pthread_create input");
    return 1;
    }

        printf("Leader started on TCP port %u.\nControls:\n\t[w/s] Speed\n\t [a/d] Turn \n\t [space] Brake \n\t [p] ToggleStale \n\t [q] Quit\n",
            (unsigned)leader_port);

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);


    while (!leader_shutdown_requested) {
    uint64_t t0 = rt_wcet_thread_time_ns();
                if (leader_sig_received) {
                        leader_request_shutdown("signal");
                        break;
                }
        next_tick.tv_nsec += (long)(LEADER_TICK_DT * 1e9);
    if (next_tick.tv_nsec >= 1e9) {
      next_tick.tv_sec += 1;
      next_tick.tv_nsec -= 1e9;
    }

    /* push  EVT_TICK_UPDATE event for the leader state machine*/
    Event tick_ev = {.type = EVT_TICK_UPDATE};
    push_event(&leader_EventQ, &tick_ev);

    int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
    if (rc == EINTR && leader_sig_received) {
        leader_request_shutdown("signal");
        break;
    }

        uint64_t t1 = rt_wcet_thread_time_ns();
        rt_wcet_stat_add(&wcet_leader_tick_iter, (t1 > t0) ? (t1 - t0) : 0);
  }

  leader_request_shutdown("main exit");

  pthread_join(input_tid, NULL);
  pthread_join(acceptor_tid, NULL);
  pthread_join(receiver_tid, NULL);
  pthread_join(sender_tid, NULL);
  pthread_join(state_tid, NULL);

  leader_close_all_sockets();
    leader_wcet_print_summary();
  return 0;
    
}
#endif

static void leader_on_signal(int signo) {
    (void)signo;
    leader_sig_received = 1;
}

static void leader_request_shutdown(const char* reason) {
    if (leader_shutdown_requested) return;
    leader_shutdown_requested = 1;

    if (reason) {
        fprintf(stderr, "\n[LEADER] Shutdown requested (%s)\n", reason);
    }

    /* Wake the FSM and sender thread */
    Event ev = {.type = EVT_SHUTDOWN};
    push_event(&leader_EventQ, &ev);

    pthread_mutex_lock(&cmd_queue.mutex);
    pthread_cond_broadcast(&cmd_queue.not_empty);
    pthread_mutex_unlock(&cmd_queue.mutex);

    /* Unblock accept() / recv() / select() by closing sockets */
    leader_close_all_sockets();
}

static void leader_close_all_sockets(void) {
    /* Close listening socket */
    if (leader_socket_fd >= 0) {
        shutdown(leader_socket_fd, SHUT_RDWR);
        close(leader_socket_fd);
        leader_socket_fd = -1;
    }

    /* Close all follower sockets */
    pthread_mutex_lock(&mutex_followers);
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!followers[i].active) continue;
        int fd = followers[i].fd;
        followers[i].active = 0;
        followers[i].fd = -1;
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
    }
    pthread_mutex_unlock(&mutex_followers);
}


void* accept_handler(void* arg) {
    (void)arg;

    while (!leader_shutdown_requested) {
        uint64_t t0 = rt_wcet_thread_time_ns();
        if (leader_socket_fd < 0) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(leader_socket_fd, &rfds);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};

        int ready = select(leader_socket_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            if (leader_shutdown_requested) break;
            perror("select accept");
            continue;
        }
        if (ready == 0) continue;
        if (!FD_ISSET(leader_socket_fd, &rfds)) continue;

        int follower_fd = accept(leader_socket_fd, NULL, NULL);
        if (follower_fd < 0) {
            if (leader_shutdown_requested) break;
            continue;
        }

        if (leader_shutdown_requested) {
            shutdown(follower_fd, SHUT_RDWR);
            close(follower_fd);
            break;
        }

        FollowerRegisterMsg reg_msg;
        recv(follower_fd, &reg_msg, sizeof(reg_msg), 0);

        /* Matrix clock local event */
        mc_local_event(&leader_clock,0); // 0 = leader ID
        mc_print(&leader_clock);

        // Register and handle topology 
        register_new_follower(follower_fd, &reg_msg);

        printf("Follower registered (socket=%d %s:%d)\n",
               follower_fd, reg_msg.selfAddress.ip, reg_msg.selfAddress.udp_port);

        uint64_t t1 = rt_wcet_thread_time_ns();
        rt_wcet_stat_add(&wcet_leader_accept_iter, (t1 > t0) ? (t1 - t0) : 0);
    }
    return NULL;
}

/* Function: Broadcast a raw message to all active followers (thread-safe) */
void broadcast_to_followers(const void* msg_data, size_t msg_len) {
    pthread_mutex_lock(&mutex_followers);
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!followers[i].active) continue;
        ssize_t sret = send(followers[i].fd, msg_data, msg_len, 0);
        if (sret < 0) {
            perror("broadcast send");
        }
    }
    pthread_mutex_unlock(&mutex_followers);
}

/* Function: Register a newly connected follower, send assigned ID and topology updates */
void register_new_follower(int fd, FollowerRegisterMsg* reg_msg) {
    if (leader_shutdown_requested) {
        close(fd);
        return;
    }
    pthread_mutex_lock(&mutex_followers);

    /* Keep active followers packed so new joiners become last. */
    compact_followers_locked();

    int idx = -1;
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!followers[i].active) { idx = i; break; }
    }
    if (idx == -1) {
        fprintf(stderr, "No free follower slots; rejecting connection\n");
        pthread_mutex_unlock(&mutex_followers);
        close(fd);
        return;
    }

    followers[idx].fd = fd;
    followers[idx].address = reg_msg->selfAddress;
    followers[idx].id = idx + 1;
    followers[idx].active = 1;

    int assigned_id = followers[idx].id;

    /* Send assigned ID */
    LD_MESSAGE idMsg = {0};
    idMsg.type = MSG_LDR_ASSIGN_ID;
    idMsg.payload.assigned_id = assigned_id;
    send(fd, &idMsg, sizeof(idMsg), 0);

    /* Send spawn pose for realistic join near current leader position */
    send_spawn_to_follower(fd, assigned_id);

    /* Increment active follower count and log formation progress */
    active_follower_count++;
    printf("[FORMATION] Active followers: %d/%d\n", active_follower_count, MIN_FOLLOWERS);

    int formed = 0;
    if (active_follower_count == MIN_FOLLOWERS) formed = 1;

    pthread_mutex_unlock(&mutex_followers);

    /* If this is the formation event (first time reaching MIN_FOLLOWERS) -> notify FSM */
    if (formed) {
        Event ev = {.type = EVT_PLATOON_FORMED};
        push_event(&leader_EventQ, &ev);
        return;
    }

    /* If formation is already complete and a new follower joins, re-finalize topology */
    if (formation_complete) {
        Event ev = {.type = EVT_PLATOON_FORMED};
        push_event(&leader_EventQ, &ev);
    }
}

static void send_spawn_to_follower(int fd, int assigned_id) {
    Truck leader_snapshot;
    int intr_len;

    pthread_mutex_lock(&mutex_leader_state);
    leader_snapshot = leader;
    intr_len = leader_intruder_length;
    pthread_mutex_unlock(&mutex_leader_state);

    float offset = ((float)assigned_id * TARGET_GAP) + (float)INTRUDER_LENGTH + (float)intr_len;

    LD_MESSAGE spawnMsg = {0};
    spawnMsg.type = MSG_LDR_SPAWN;
    spawnMsg.payload.spawn.assigned_id = assigned_id;
    spawnMsg.payload.spawn.spawn_dir = leader_snapshot.dir;

    switch (leader_snapshot.dir) {
    case NORTH:
        spawnMsg.payload.spawn.spawn_x = leader_snapshot.x;
        spawnMsg.payload.spawn.spawn_y = leader_snapshot.y - offset;
        break;
    case SOUTH:
        spawnMsg.payload.spawn.spawn_x = leader_snapshot.x;
        spawnMsg.payload.spawn.spawn_y = leader_snapshot.y + offset;
        break;
    case EAST:
        spawnMsg.payload.spawn.spawn_x = leader_snapshot.x - offset;
        spawnMsg.payload.spawn.spawn_y = leader_snapshot.y;
        break;
    case WEST:
        spawnMsg.payload.spawn.spawn_x = leader_snapshot.x + offset;
        spawnMsg.payload.spawn.spawn_y = leader_snapshot.y;
        break;
    }

    mc_send_event(&leader_clock, 0);
    memcpy(spawnMsg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));

    ssize_t sret = send(fd, &spawnMsg, sizeof(spawnMsg), 0);
    if (sret < 0) perror("send spawn");
}

/* Finalize topology once minimum followers have joined */
void finalize_topology(void) {
    pthread_mutex_lock(&mutex_followers);
    /*
     * Reformation must do two things atomically:
     *  1) Compact active followers so joiners become last
     *  2) Reassign sequential platoon positions (IDs) and broadcast both ID + rear topology
     */
    compact_followers_locked();

    int active_count = 0;
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (followers[i].active) active_count++;
    }

    /* Reassign IDs as contiguous positions (1..N) */
    int pos = 0;
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!followers[i].active) continue;
        followers[i].id = ++pos;
    }

    /* Broadcast updated assigned IDs first so followers switch control source immediately */
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!followers[i].active) continue;

        LD_MESSAGE idMsg = {0};
        idMsg.type = MSG_LDR_ASSIGN_ID;
        idMsg.payload.assigned_id = followers[i].id;

        mc_send_event(&leader_clock, 0);
        memcpy(idMsg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));

        ssize_t sret = send(followers[i].fd, &idMsg, sizeof(idMsg), 0);
        if (sret < 0) perror("send assign_id");
    }

    /* Then broadcast rear pointers based on new ordering (i -> i+1) */
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!followers[i].active) continue;

        LD_MESSAGE update = {0};
        update.type = MSG_LDR_UPDATE_REAR;

        int found = 0;
        NetInfo rearAddr = {0};
        for (int j = i + 1; j < MAX_FOLLOWERS; j++) {
            if (!followers[j].active) continue;
            found = 1;
            rearAddr = followers[j].address;
            break;
        }

        update.payload.rearInfo.has_rearTruck = found ? 1 : 0;
        if (found) update.payload.rearInfo.rearTruck_Address = rearAddr;

        mc_send_event(&leader_clock, 0);
        memcpy(update.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));

        ssize_t sret = send(followers[i].fd, &update, sizeof(update), 0);
        if (sret < 0) perror("send topology");
    }

    /* Formation remains complete as long as at least one follower exists */
    formation_complete = (active_count > 0) ? 1 : 0;
    pthread_mutex_unlock(&mutex_followers);
}

/* Keep active followers packed at the front of the array (stable order). Call with mutex_followers held. */
static void compact_followers_locked(void) {
    int write_idx = 0;
    for (int read_idx = 0; read_idx < MAX_FOLLOWERS; read_idx++) {
        if (!followers[read_idx].active) continue;
        if (write_idx != read_idx) {
            followers[write_idx] = followers[read_idx];
        }
        write_idx++;
    }

    for (int i = write_idx; i < MAX_FOLLOWERS; i++) {
        followers[i].active = 0;
        followers[i].fd = -1;
        followers[i].id = i + 1;
        followers[i].address.udp_port = 0;
        followers[i].address.ip[0] = '\0';
    }
}

/* Wrapper with logging/tracing to make finalization observable and atomic from the FSM perspective */
void finalize_topology_atomic(void) {
    printf("[FORMATION] Finalization START (active=%d)\n", active_follower_count);
    finalize_topology();
    printf("[FORMATION] Finalization DONE\n");
}


/* Thread: Receive and process messages from followers */
void* follower_message_receiver(void* arg) {
    (void)arg;

    printf("[RECEIVER] Follower message receiver thread started\n");

    while (!leader_shutdown_requested) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;

        pthread_mutex_lock(&mutex_followers);
        for (int i = 0; i < MAX_FOLLOWERS; i++) {
            if (!followers[i].active) continue;
            FD_SET(followers[i].fd, &readfds);
            if (followers[i].fd > maxfd) maxfd = followers[i].fd;
        }
        pthread_mutex_unlock(&mutex_followers);

        if (maxfd == -1) {
            /* No connected followers - sleep briefly */
            struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
            select(0, NULL, NULL, NULL, &tv);
            continue;
        }

        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000}; // 100ms timeout
        int ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            if (leader_shutdown_requested) break;
            continue;
        }
        if (ready == 0) continue;

        pthread_mutex_lock(&mutex_followers);
        for (int i = 0; i < MAX_FOLLOWERS; i++) {
            if (!followers[i].active) continue;
            int fd = followers[i].fd;
            if (!FD_ISSET(fd, &readfds)) continue;

            FT_MESSAGE msg = {0};  /* FIXED: Initialize to zero to avoid garbage in padding */
            uint64_t t0 = rt_wcet_thread_time_ns();
            ssize_t r = recv(fd, &msg, sizeof(msg), 0);
            if (r <= 0) {
                if (r == 0) printf("\n[RECEIVER] Follower %d disconnected\n", followers[i].id);
                else perror("recv");
                shutdown(fd, SHUT_RDWR);
                close(fd);

                /* Update session state and active count */
                int disconnected_id = followers[i].id;
                followers[i].active = 0;
                active_follower_count--;
                printf("[FORMATION] Follower %d disconnected -> active=%d/%d\n", disconnected_id, active_follower_count, MIN_FOLLOWERS);

                /* Re-finalize topology for any remaining follower(s) so platoon_position updates (1..N) */
                if (formation_complete && active_follower_count >= 1) {
                    Event ev = {.type = EVT_PLATOON_FORMED};
                    push_event(&leader_EventQ, &ev);
                } else if (active_follower_count < 1) {
                    formation_complete = 0;
                    printf("[FORMATION] Not enough followers, waiting for more to join\n");
                }

                continue;
            }

            int fid = followers[i].id;
            pthread_mutex_unlock(&mutex_followers);

            Event ev = {0};
            ev.type = EVT_FOLLOWER_MSG;
            ev.event_data.follower_msg.follower_id = fid;
            ev.event_data.follower_msg.msg = msg;
            push_event(&leader_EventQ, &ev);

            uint64_t t1 = rt_wcet_thread_time_ns();
            rt_wcet_stat_add(&wcet_leader_rx_dispatch, (t1 > t0) ? (t1 - t0) : 0);

            pthread_mutex_lock(&mutex_followers);
        }
        pthread_mutex_unlock(&mutex_followers);
    }

    return NULL;
}





/* Broadcast emergency brake to all followers */
void broadcast_emergency_to_followers(void) {
    printf("[LEADER] Broadcasting emergency brake to all followers\n");
    
    LD_MESSAGE emergency_msg = {0};
    emergency_msg.type = MSG_LDR_EMERGENCY_BRAKE;
    
    mc_send_event(&leader_clock, 0);
    memcpy(emergency_msg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
    broadcast_to_followers(&emergency_msg, sizeof(emergency_msg));
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

    while (!leader_shutdown_requested) {
        pthread_mutex_lock(&cmd_queue.mutex);

        while (cmd_queue.head == cmd_queue.tail && !leader_shutdown_requested) {
            pthread_cond_wait(&cmd_queue.not_empty, &cmd_queue.mutex);
        }

        if (leader_shutdown_requested) {
            pthread_mutex_unlock(&cmd_queue.mutex);
            break;
        }

        LeaderCommand ldr_cmd = cmd_queue.queue[cmd_queue.head];
        cmd_queue.head = (cmd_queue.head + 1) % CMD_QUEUE_SIZE;

        LD_MESSAGE ldr_cmd_msg;
        ldr_cmd_msg.type = MSG_LDR_CMD;
        ldr_cmd_msg.payload.cmd = ldr_cmd;

        pthread_mutex_unlock(&cmd_queue.mutex);

        /* Prepare matrix clock and broadcast to active followers */
        uint64_t t0 = rt_wcet_thread_time_ns();
        mc_send_event(&leader_clock, 0);  // 0 = leader ID
        memcpy(ldr_cmd_msg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
        broadcast_to_followers(&ldr_cmd_msg, sizeof(ldr_cmd_msg));
        uint64_t t1 = rt_wcet_thread_time_ns();
        rt_wcet_stat_add(&wcet_leader_send_cmd, (t1 > t0) ? (t1 - t0) : 0);
    }

    return NULL;
}

/* Centralized leader state machine: single writer for leader state */
void* leader_state_machine(void* arg) {
    (void)arg;
    unsigned long tick_count = 0;
    while (!leader_shutdown_requested) {
        Event ev = pop_event(&leader_EventQ);
        uint64_t t0 = rt_wcet_thread_time_ns();
        switch (ev.type) {
            case EVT_SHUTDOWN:
                return NULL;
            case EVT_PLATOON_FORMED: {
                printf("\n[FORMATION] EVT_PLATOON_FORMED received - scheduling finalization\n");
                finalize_topology_atomic();
                printf("[FORMATION] Topology finalized. Controls unlocked.\n");
                break;
            }

            case EVT_TICK_UPDATE: {
                if (!formation_complete) {
                    /* Stall physics until formation completes */
                    break;
                }

                if (leader_stale_mode) {
                    /* Simulate leader/control plane stall: do not move and do not send commands. */
                    break;
                }

                tick_count++;

                LeaderCommand ldr_cmd = {.command_id = ++cmd_id, .is_turning_event = 0};
                if (pending_turn) {
                    ldr_cmd.is_turning_event = 1;
                    ldr_cmd.turn_point_x = leader.x;
                    ldr_cmd.turn_point_y = leader.y;
                    ldr_cmd.turn_dir = next_turn_dir;

                    leader.dir = next_turn_dir;
                    pending_turn = 0;
                    printf("\n[TURN] Leader at (%.2f, %.2f) to %d\n", ldr_cmd.turn_point_x,
                           ldr_cmd.turn_point_y, next_turn_dir);
                }

                pthread_mutex_lock(&mutex_leader_state);
                move_truck(&leader, LEADER_TICK_DT);
                ldr_cmd.leader = leader;
                pthread_mutex_unlock(&mutex_leader_state);

                mc_local_event(&leader_clock, 0);
                queue_commands(&ldr_cmd);

                  if (LEADER_PRINT_EVERY_N <= 1 || (tick_count % (unsigned long)LEADER_PRINT_EVERY_N) == 0) {
                      printf("\rLeader: POS(%.1f,%.1f) SPD=%.1f DIR=%d STATE=%d    ", leader.x,
                          leader.y, leader.speed, leader.dir, leader.state);
                      //mc_print(&leader_clock);
                      fflush(stdout);
                  }
                break;
            }

            case EVT_USER_INPUT: {
                if (!formation_complete) {
                    printf("Waiting for followers...\n");
                    break;
                }

                char c = ev.event_data.input.key;
                if (c == 'w') {
                    leader.speed += 0.5f;
                    leader.state = CRUISE;
                } else if (c == 's') {
                    leader.speed -= 0.5f;
                    leader.state = CRUISE;
                    if (leader.speed <= 0) {
                        leader.speed = 0;
                        leader.state = STOPPED;
                    }
                } else if (c == 'a') {
                    next_turn_dir = (leader.dir + 3) % 4; // Left
                    pending_turn = 1;
                    leader.state = CRUISE;
                } else if (c == 'd') {
                    next_turn_dir = (leader.dir + 1) % 4; // Right
                    pending_turn = 1;
                    leader.state = CRUISE;
                } else if (c == ' ') {
                    leader.speed = 0;
                    leader.state = EMERGENCY_BRAKE;
                    /* Also broadcast emergency to followers */
                    broadcast_emergency_to_followers();
                } else if (c == 'p' || c == 'P') {
                    leader_stale_mode = !leader_stale_mode;
                    if (leader_stale_mode) {
                        printf("\n[LEADER] Stale mode ON: pausing cruise commands\n");
                    } else {
                        printf("\n[LEADER] Stale mode OFF: resuming cruise commands\n");
                    }
                } else if (c == 'q') {
                    leader_request_shutdown("user");
                }
                break;
            }

            case EVT_FOLLOWER_MSG: {
                int fid = ev.event_data.follower_msg.follower_id;
                FT_MESSAGE msg = ev.event_data.follower_msg.msg;
                /* Merge matrix clocks on receive */
                mc_receive_event(&leader_clock, &msg.matrix_clock, 0);

                switch (msg.type) {
                    case MSG_FT_INTRUDER_REPORT:
                        if (msg.payload.intruder.speed == 0) {
                            leader.state = CRUISE;
                            pthread_mutex_lock(&mutex_leader_state);
                            leader_intruder_length = 0;
                            pthread_mutex_unlock(&mutex_leader_state);
                            printf("\n[LEADER] Intruder cleared by follower %d\n", fid);
                        } else {
                            leader.state = INTRUDER_FOLLOW;
                            leader.speed = msg.payload.intruder.speed;
                            pthread_mutex_lock(&mutex_leader_state);
                            leader_intruder_length = msg.payload.intruder.length;
                            pthread_mutex_unlock(&mutex_leader_state);
                            printf("\n[LEADER] Intruder reported by follower %d: speed=%d length=%d\n",
                                   fid, msg.payload.intruder.speed, msg.payload.intruder.length);
                        }
                        break;

                    case MSG_FT_POSITION:
                        printf("\n[LEADER] Follower %d position: x=%.1f, y=%.1f\n",
                               fid, msg.payload.position.x, msg.payload.position.y);
                        break;

                    case MSG_FT_EMERGENCY_BRAKE:
                        printf("\n[LEADER] Emergency brake from follower %d\n", fid);
                        broadcast_emergency_to_followers();
                        break;

                    default:
                        break;
                }
                break;
            }

            default:
                //Unhandled event types relevant to follower truck
                break;
        }
        uint64_t t1 = rt_wcet_thread_time_ns();
        rt_wcet_stat_add(&wcet_leader_fsm_event, (t1 > t0) ? (t1 - t0) : 0);
    }
    return NULL;
}

/* Keyboard Input Handler */
void *input_handler(void *arg) {
  (void)arg;
  struct termios oldt, newt;
  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    while (!leader_shutdown_requested) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(STDIN_FILENO, &rfds);
                struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};

                int ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
                if (ready < 0) {
                        if (errno == EINTR) continue;
                        break;
                }
                if (ready == 0) continue;
                if (!FD_ISSET(STDIN_FILENO, &rfds)) continue;

                char c = 0;
                ssize_t n = read(STDIN_FILENO, &c, 1);
                if (n <= 0) continue;

    Event evt = {0};
    evt.type = EVT_USER_INPUT;
    evt.event_data.input.key = c;
    push_event(&leader_EventQ, &evt);
  }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  return NULL;
}

/*Function: Move Truck*/
void move_truck(Truck *t, float dt) {
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
}

