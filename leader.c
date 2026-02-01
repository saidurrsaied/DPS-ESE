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

/* Heartbeat settings */ //Rajdeep
#define HEARTBEAT_INTERVAL_MS 100 //Rajdeep

/* Leader truck state */
int leader_socket_fd;
int follower_fd_list[MAX_FOLLOWERS];
NetInfo follower_Addresses[MAX_FOLLOWERS];
int follower_count = 0;

/* Leader intruder tracking */
int leader_in_intruder_mode = 0;
int32_t intruder_speed = 0;
pthread_mutex_t mutex_intruder_state;

pthread_mutex_t mutex_client_fd_list;
pthread_t sender_tid;
pthread_t acceptor_tid;
pthread_t receiver_tid;  // New receiver thread for follower messages
pthread_t input_tid;
pthread_t heartbeat_tid; //Rajdeep

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
void* leader_heartbeat_sender(void* arg); //Rajdeep
static void sleep_ms(int ms); //Rajdeep
void leader_decide_next_state(Truck* t);
void move_truck(Truck* t);
void queue_commands(LeaderCommand* ldr_cmd);
void handle_follower_intruder_report(int follower_id, IntruderInfo intruder);
void handle_follower_position_report(int follower_id, FT_POSITION position);
void broadcast_emergency_to_followers(void);

int main(void) {
    //Rajdeep
    srand(time(NULL));
    pthread_mutex_init(&mutex_client_fd_list, NULL);
    pthread_mutex_init(&mutex_intruder_state, NULL);

    leader = (Truck){.x = 0.0f, .y = 0.0f, .speed = 0.0f, .dir = NORTH, .state = STOPPED};

    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    mc_init(&leader_clock); // matrix clock

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

    /* Initialize command queue before starting threads */
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
    if (pthread_create(&heartbeat_tid, NULL, leader_heartbeat_sender, NULL) != 0) { //Rajdeep
        perror("pthread_create heartbeat");
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

    pthread_mutex_lock(&mutex_client_fd_list);

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

    queue_commands(&ldr_cmd);

    printf("\rLeader: POS(%.1f,%.1f) SPD=%.1f DIR=%d STATE=%d    ", leader.x,
           leader.y, leader.speed, leader.dir, leader.state);
    fflush(stdout);

    pthread_mutex_unlock(&mutex_client_fd_list);

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
  }
    
}


void* accept_handler(void* arg) {
    //Rajdeep
    (void)arg; //Rajdeep

    while (1) {
        int follower_fd = accept(leader_socket_fd, NULL, NULL);
        if (follower_fd < 0) continue;

        FollowerRegisterMsg reg_msg;
        recv(follower_fd, &reg_msg, sizeof(reg_msg), 0);
        
        /* Matrix clock*/
        mc_local_event(&leader_clock,0); // 0 = leader ID
		mc_print(&leader_clock); 
		/**/

        pthread_mutex_lock(&mutex_client_fd_list);
        int id = follower_count;
        follower_fd_list[id] = follower_fd;
        follower_Addresses[id] = reg_msg.selfAddress;
        follower_count++;

        // Send assigned ID to follower
        LD_MESSAGE idMsg = {0};
        idMsg.type = MSG_LDR_ASSIGN_ID;
        idMsg.payload.assigned_id = follower_count;
        send(follower_fd, &idMsg, sizeof(idMsg), 0);

        // Send rear-truck info to the newly connected follower. 
        //The first follower has no rear truck.but it expects a message
        LD_MESSAGE initialMsg = {0};
        initialMsg.type = MSG_LDR_UPDATE_REAR;
        initialMsg.payload.rearInfo.has_rearTruck = 0; // First connection has no rear
        
        /* Matrix clock*/
        mc_send_event(&leader_clock, 0); // leader sends a message
        memcpy(initialMsg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
        /**/
        
        send(follower_fd, &initialMsg, sizeof(initialMsg), 0);

        // If there is a previous follower, update it so its rear points to this new follower 
        if (id > 0) {
            LD_MESSAGE update_rearInfo = {0};
            update_rearInfo.type = MSG_LDR_UPDATE_REAR;
            update_rearInfo.payload.rearInfo.rearTruck_Address = reg_msg.selfAddress;
            update_rearInfo.payload.rearInfo.has_rearTruck = 1; 
            
            /* Matrix clock*/
        mc_send_event(&leader_clock, 0); // leader sends a message
		memcpy(initialMsg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
        /**/
        
        mc_send_event(&leader_clock, 0);
			memcpy(update_rearInfo.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
            
            send(follower_fd_list[id-1], &update_rearInfo, sizeof(update_rearInfo), 0);
        }

        printf("Follower %d registered (%s:%d)\n",
               id, reg_msg.selfAddress.ip, reg_msg.selfAddress.udp_port);

        pthread_mutex_unlock(&mutex_client_fd_list);
    }
}


/* Thread: Receive and process messages from followers */
void* follower_message_receiver(void* arg) {
    (void)arg;
    FT_MESSAGE msg;
    
    printf("[RECEIVER] Follower message receiver thread started\n");

    while (1) {
        pthread_mutex_lock(&mutex_client_fd_list);
        
        // Poll all follower sockets for messages
        for (int i = 0; i < follower_count; i++) {
            int fd = follower_fd_list[i];
            
            // Use non-blocking recv to check for data
            ssize_t recv_len = recv(fd, &msg, sizeof(msg), MSG_DONTWAIT);
            
            if (recv_len < 0) {
                // No data available (EAGAIN/EWOULDBLOCK) - continue to next follower
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                // Socket error - skip this follower
                perror("recv");
                continue;
            }
            
            if (recv_len == 0) {
                // Connection closed by follower
                printf("[RECEIVER] Follower %d disconnected\n", i);
                close(fd);
                // Note: Could implement proper cleanup here
                continue;
            }

            // Message received - process based on type
            pthread_mutex_unlock(&mutex_client_fd_list);
            
            switch (msg.type) {
                case MSG_FT_INTRUDER_REPORT:
                    printf("[RECEIVER] Intruder report from follower %d: speed=%d, length=%d\n",
                           i, msg.payload.intruder.speed, msg.payload.intruder.length);
                    handle_follower_intruder_report(i, msg.payload.intruder);
                    break;

                case MSG_FT_POSITION:
                    printf("[RECEIVER] Position report from follower %d: (%f,%f)\n",
                           i, msg.payload.position.x, msg.payload.position.y);
                    handle_follower_position_report(i, msg.payload.position);
                    break;

                case MSG_FT_EMERGENCY_BRAKE:
                    printf("[RECEIVER] Emergency brake from follower %d\n", i);
                    broadcast_emergency_to_followers();
                    break;

                default:
                    printf("[RECEIVER] Unknown message type %d from follower %d\n", 
                           msg.type, i);
                    break;
            }
            
            pthread_mutex_lock(&mutex_client_fd_list);
        }
        
        pthread_mutex_unlock(&mutex_client_fd_list);
        
        // Small sleep to prevent busy-wait using select timeout
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms
        select(0, NULL, NULL, NULL, &tv);
    }
    
    return NULL;
}


/* Handle intruder report from follower */
void handle_follower_intruder_report(int follower_id, IntruderInfo intruder) {
    // If intruder speed is 0, it's an intruder clear event
    if (intruder.speed == 0) {
        printf("[LEADER] Intruder cleared by follower %d\n", follower_id);
        
        pthread_mutex_lock(&mutex_intruder_state);
        leader_in_intruder_mode = 0;
        intruder_speed = 0;
        pthread_mutex_unlock(&mutex_intruder_state);
        
        return;
    }
    
    // Intruder detected - leader matches intruder speed
    printf("[LEADER] Matching intruder speed: %d m/s\n", intruder.speed);
    
    pthread_mutex_lock(&mutex_intruder_state);
    leader_in_intruder_mode = 1;
    intruder_speed = intruder.speed;
    pthread_mutex_unlock(&mutex_intruder_state);
}


/* Handle position report from follower (for distance monitoring) */
void handle_follower_position_report(int follower_id, FT_POSITION position) {
    // Placeholder for future distance-based control
    printf("[LEADER] Follower %d position: x=%.1f, y=%.1f\n",
           follower_id, position.x, position.y);
}


/* Broadcast emergency brake to all followers */
void broadcast_emergency_to_followers(void) {
    printf("[LEADER] Broadcasting emergency brake to all followers\n");
    
    LD_MESSAGE emergency_msg = {0};
    emergency_msg.type = MSG_LDR_EMERGENCY_BRAKE;
    
    pthread_mutex_lock(&mutex_client_fd_list);
    for (int i = 0; i < follower_count; i++) {
        ssize_t sret = send(follower_fd_list[i], &emergency_msg, sizeof(emergency_msg), 0);
        if (sret < 0) {
            perror("send emergency to follower");
        }
    }
    pthread_mutex_unlock(&mutex_client_fd_list);
}

/* Heartbeat sender thread (TCP) */ //Rajdeep
void* leader_heartbeat_sender(void* arg) { //Rajdeep
    //Rajdeep
    (void)arg;

    LD_MESSAGE hb_msg = {0}; //Rajdeep
    hb_msg.type = MSG_LDR_HEARTBEAT; //Rajdeep

    while (1) { //Rajdeep
        pthread_mutex_lock(&mutex_client_fd_list); //Rajdeep
        for (int i = 0; i < follower_count; i++) { //Rajdeep
            // Matrix clock update before sending heartbeat //Rajdeep
            mc_send_event(&leader_clock, 0); //Rajdeep
            memcpy(hb_msg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc)); //Rajdeep

            ssize_t sret = send(follower_fd_list[i], &hb_msg, sizeof(hb_msg), 0); //Rajdeep
            if (sret < 0) { //Rajdeep
                perror("leader heartbeat send"); //Rajdeep
            } //Rajdeep
        } //Rajdeep
        pthread_mutex_unlock(&mutex_client_fd_list); //Rajdeep

        sleep_ms(HEARTBEAT_INTERVAL_MS); //Rajdeep
    }

    return NULL; //Rajdeep
} //Rajdeep

static void sleep_ms(int ms) { //Rajdeep
    //Rajdeep
    struct timespec ts; //Rajdeep
    ts.tv_sec = ms / 1000; //Rajdeep
    ts.tv_nsec = (long)(ms % 1000) * 1000000L; //Rajdeep
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { //Rajdeep
        // retry //Rajdeep
    } //Rajdeep
} //Rajdeep


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

        pthread_mutex_lock(&mutex_client_fd_list);
        for (int i = 0; i < follower_count; i++) {
        
        	// Matrix clock update before sending
            mc_send_event(&leader_clock, 0);  // 0 = leader ID
            memcpy(ldr_cmd_msg.matrix_clock.mc, leader_clock.mc, sizeof(leader_clock.mc));
        
            ssize_t sret = send(follower_fd_list[i], &ldr_cmd_msg, sizeof(ldr_cmd_msg), 0);
            if (sret < 0) {
                perror("send to follower");
            }
        }
        pthread_mutex_unlock(&mutex_client_fd_list);
    }
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
    pthread_mutex_lock(&mutex_client_fd_list);
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
    }
    // else if (c == ' ') {
    //   leader.speed = 0;
    //   leader.state = EMERGENCY_BRAKE;
    // }
    else if (c == 'q') {
      tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
      exit(0);
    }
    pthread_mutex_unlock(&mutex_client_fd_list);
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
