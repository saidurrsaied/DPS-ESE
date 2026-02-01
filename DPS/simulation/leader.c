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

#include "truckplatoon.h"
#include "matrix_clock.h"
#include "event.h"

/* Leader truck state */
int leader_socket_fd;

/* Follower session encapsulation */
typedef struct {
    int id;           /* logical ID starting at 1 */
    int fd;           /* socket FD */
    NetInfo address;  /* IP and UDP port */
    int active;       /* 1 = connected, 0 = empty/disconnected */
} FollowerSession;

FollowerSession followers[MAX_FOLLOWERS];
pthread_mutex_t mutex_followers; /* protect followers[] */

/* Helper prototypes */
void register_new_follower(int fd, FollowerRegisterMsg* reg_msg);
void broadcast_to_followers(const void* msg_data, size_t msg_len);

pthread_mutex_t mutex_client_fd_list;
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

MatrixClock leader_clock; //matrix clock declaration

void* accept_handler(void* arg);
void* send_handler(void* arg);
void* follower_message_receiver(void* arg);
void *input_handler(void *arg); //BW
void* leader_state_machine(void* arg); //MSR
void move_truck(Truck* t);
void queue_commands(LeaderCommand* ldr_cmd);
void broadcast_emergency_to_followers(void);

#ifndef TEST_LEADER
int main(void) {
    srand(time(NULL));
    pthread_mutex_init(&mutex_client_fd_list, NULL);

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

    printf("Leader started. Controls: [w/s] Speed, [a/d] Turn, [space] Brake, [q] Quit\n");

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);




  while (1) {
    next_tick.tv_nsec += (long)(SIM_DT * 1e9);
    if (next_tick.tv_nsec >= 1e9) {
      next_tick.tv_sec += 1;
      next_tick.tv_nsec -= 1e9;
    }

    /* push  EVT_TICK_UPDATE event for the leader state machine*/
    Event tick_ev = {.type = EVT_TICK_UPDATE};
    push_event(&leader_EventQ, &tick_ev);

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
  }
    
}
#endif


void* accept_handler(void* arg) {
    (void)arg;

    while (1) {
        int follower_fd = accept(leader_socket_fd, NULL, NULL);
        if (follower_fd < 0) continue;

        FollowerRegisterMsg reg_msg;
        recv(follower_fd, &reg_msg, sizeof(reg_msg), 0);

        /* Matrix clock local event */
        mc_local_event(&leader_clock,0); // 0 = leader ID
        mc_print(&leader_clock);

        /* Register and handle topology in a helper (locks followers array internally) */
        register_new_follower(follower_fd, &reg_msg);

        printf("Follower registered (socket=%d %s:%d)\n",
               follower_fd, reg_msg.selfAddress.ip, reg_msg.selfAddress.udp_port);
    }
}

/* Helper: Broadcast a raw message to all active followers (thread-safe) */
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

/* Helper: Register a newly connected follower, send assigned ID and topology updates */
void register_new_follower(int fd, FollowerRegisterMsg* reg_msg) {
    pthread_mutex_lock(&mutex_followers);

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

    /* Send initial rear update (no rear) */
    LD_MESSAGE initialMsg = {0};
    initialMsg.type = MSG_LDR_UPDATE_REAR;
    initialMsg.payload.rearInfo.has_rearTruck = 0;
    mc_send_event(&leader_clock, 0);
    memcpy(initialMsg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
    send(fd, &initialMsg, sizeof(initialMsg), 0);

    /* If there is a previous active follower, update its rear to point to the new follower */
    int prev_idx = -1;
    for (int j = idx - 1; j >= 0; j--) {
        if (followers[j].active) { prev_idx = j; break; }
    }
    if (prev_idx >= 0) {
        LD_MESSAGE update_rearInfo = {0};
        update_rearInfo.type = MSG_LDR_UPDATE_REAR;
        update_rearInfo.payload.rearInfo.rearTruck_Address = reg_msg->selfAddress;
        update_rearInfo.payload.rearInfo.has_rearTruck = 1;
        mc_send_event(&leader_clock, 0);
        memcpy(update_rearInfo.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
        send(followers[prev_idx].fd, &update_rearInfo, sizeof(update_rearInfo), 0);
    }

    pthread_mutex_unlock(&mutex_followers);
}

/* Thread: Receive and process messages from followers */
void* follower_message_receiver(void* arg) {
    (void)arg;

    printf("[RECEIVER] Follower message receiver thread started\n");

    while (1) {
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

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready <= 0) continue;

        pthread_mutex_lock(&mutex_followers);
        for (int i = 0; i < MAX_FOLLOWERS; i++) {
            if (!followers[i].active) continue;
            int fd = followers[i].fd;
            if (!FD_ISSET(fd, &readfds)) continue;

            FT_MESSAGE msg;
            ssize_t r = recv(fd, &msg, sizeof(msg), 0);
            if (r <= 0) {
                if (r == 0) printf("[RECEIVER] Follower %d disconnected\n", followers[i].id);
                else perror("recv");
                close(fd);
                followers[i].active = 0;
                continue;
            }

            int fid = followers[i].id;
            pthread_mutex_unlock(&mutex_followers);

            Event ev = {0};
            ev.type = EVT_FOLLOWER_MSG;
            ev.event_data.follower_msg.follower_id = fid;
            ev.event_data.follower_msg.msg = msg;
            push_event(&leader_EventQ, &ev);

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


/*  Leader logic 
void leader_decide_next_state(Truck* t) {
    // Check if in intruder mode
    pthread_mutex_lock(&mutex_intruder_state);
    if (leader_in_intruder_mode) {
        // Match intruder speed while in intruder mode
        t->state = CRUISE;
        t->speed = intruder_speed;
        pthread_mutex_unlock(&mutex_intruder_state);
        printf("[LEADER] In intruder mode: speed=%d\n", t->speed);
        return;
    }
    pthread_mutex_unlock(&mutex_intruder_state);
    
    // Normal leader decision logic
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
*/


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

        /* Prepare matrix clock and broadcast to active followers */
        mc_send_event(&leader_clock, 0);  // 0 = leader ID
        memcpy(ldr_cmd_msg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
        broadcast_to_followers(&ldr_cmd_msg, sizeof(ldr_cmd_msg));
    }
}

/* Centralized leader state machine: single writer for leader state */
void* leader_state_machine(void* arg) {
    (void)arg;
    while (1) {
        Event ev = pop_event(&leader_EventQ);
        switch (ev.type) {
            case EVT_TICK_UPDATE: {
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

                move_truck(&leader);
                ldr_cmd.leader = leader;

                mc_local_event(&leader_clock, 0);
                queue_commands(&ldr_cmd);

                printf("\rLeader: POS(%.1f,%.1f) SPD=%.1f DIR=%d STATE=%d    ", leader.x,
                       leader.y, leader.speed, leader.dir, leader.state);
                fflush(stdout);
                break;
            }

            case EVT_USER_INPUT: {
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
                } else if (c == 'q') {
                    exit(0); /* terminate cleanly */
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
                            printf("[LEADER] Intruder cleared by follower %d\n", fid);
                        } else {
                            leader.state = INTRUDER_FOLLOW;
                            leader.speed = msg.payload.intruder.speed;
                            printf("[LEADER] Intruder reported by follower %d: speed=%d length=%d\n",
                                   fid, msg.payload.intruder.speed, msg.payload.intruder.length);
                        }
                        break;

                    case MSG_FT_POSITION:
                        printf("[LEADER] Follower %d position: x=%.1f, y=%.1f\n",
                               fid, msg.payload.position.x, msg.payload.position.y);
                        break;

                    case MSG_FT_EMERGENCY_BRAKE:
                        printf("[LEADER] Emergency brake from follower %d\n", fid);
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

  while (1) {
    char c = getchar();
    Event evt = {0};
    evt.type = EVT_USER_INPUT;
    evt.event_data.input.key = c;
    push_event(&leader_EventQ, &evt);

    if (c == 'q') {
      /* Restore terminal before exiting input thread; state-machine handles actual shutdown */
      tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
      return NULL;
    }
  }
  return NULL;
}

/*Function: Move Truck*/
void move_truck(Truck *t) {
    float dx = 0, dy = 0;
    switch (t->dir) {
    case NORTH:
    dy = t->speed * SIM_DT;
    break;
    case SOUTH:
    dy = -t->speed * SIM_DT;
    break;
    case EAST:
    dx = t->speed * SIM_DT;
    break;
    case WEST:
    dx = -t->speed * SIM_DT;
    break;
    }
    t->x += dx;
    t->y += dy;
}

