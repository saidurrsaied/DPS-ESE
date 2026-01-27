// leader.c

// TODO:
/*
    1. Add follower intruder notification receive capability and handle logic
           **Details: If the follower send an intruder warning to the leader,
   the leader shall cruise at the speed of the intruder (sent by follower in the
   intruder notification ). The leader shall exit upon intruder timeout/ clear
   event.
*/

#define _POSIX_C_SOURCE 200809L

#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <termios.h>

#include "truckplatoon.h"

int leader_socket_fd;
int follower_fd_list[MAX_FOLLOWERS];
NetInfo follower_Addresses[MAX_FOLLOWERS];
int follower_count = 0;

pthread_mutex_t mutex_client_fd_list;
pthread_t sender_tid;
pthread_t acceptor_tid;
pthread_t input_tid;

Truck leader;
uint64_t cmd_id = 0;
CommandQueue cmd_queue;

/* Turn Event Tracking */
int pending_turn = 0;
DIRECTION next_turn_dir;

void *accept_handler(void *arg);
void *send_handler(void *arg);
void move_truck(Truck *t);
void queue_commands(LeaderCommand *ldr_cmd);
void *input_handler(void *arg);

int main(void) {
  srand(time(NULL));
  pthread_mutex_init(&mutex_client_fd_list, NULL);

  // leader = (Truck){0,0,1,NORTH,CRUISE};
  leader = (Truck){
      .x = 0.0f, .y = 0.0f, .speed = 0.0f, .dir = NORTH, .state = STOPPED};

  leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

  int opt = 1;
  setsockopt(leader_socket_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(LEADER_PORT),
                             .sin_addr.s_addr = INADDR_ANY};

  if (bind(leader_socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
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
  if (pthread_create(&input_tid, NULL, input_handler, NULL) != 0) {
    perror("pthread_create input");
    return 1;
  }

  printf("Leader started. Controls: [w/s] Speed, [a/d] Turn, [space] Brake, "
         "[q] Quit\n");

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

void *accept_handler(void *arg) {
  (void)arg;

  while (1) {
    int follower_fd = accept(leader_socket_fd, NULL, NULL);
    if (follower_fd < 0)
      continue;

    FollowerRegisterMsg reg_msg;
    recv(follower_fd, &reg_msg, sizeof(reg_msg), 0);

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
    // The first follower has no rear truck.but it expects a message
    LD_MESSAGE initialMsg = {0};
    initialMsg.type = MSG_LDR_UPDATE_REAR;
    initialMsg.payload.rearInfo.has_rearTruck =
        0; // First connection has no rear
    send(follower_fd, &initialMsg, sizeof(initialMsg), 0);

    // If there is a previous follower, update it so its rear points to this new
    // follower
    if (id > 0) {
      LD_MESSAGE update_rearInfo = {0};
      update_rearInfo.type = MSG_LDR_UPDATE_REAR;
      update_rearInfo.payload.rearInfo.rearTruck_Address = reg_msg.selfAddress;
      update_rearInfo.payload.rearInfo.has_rearTruck = 1;
      send(follower_fd_list[id - 1], &update_rearInfo, sizeof(update_rearInfo),
           0);
    }

    printf("Follower %d registered (%s:%d)\n", id, reg_msg.selfAddress.ip,
           reg_msg.selfAddress.udp_port);

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

// Helper function for queuing commands
void queue_commands(LeaderCommand *ldr_cmd) {
  pthread_mutex_lock(&cmd_queue.mutex);

  int next_tail = (cmd_queue.tail + 1) % CMD_QUEUE_SIZE;
  if (next_tail == cmd_queue.head) {
    /* Queue full; drop the message and log */
    fprintf(stderr, "cmd_queue full, dropping command %lu\n",
            ldr_cmd->command_id);
    pthread_mutex_unlock(&cmd_queue.mutex);
    return;
  }

  cmd_queue.queue[cmd_queue.tail] = *ldr_cmd;
  cmd_queue.tail = next_tail;

  pthread_cond_signal(&cmd_queue.not_empty);
  pthread_mutex_unlock(&cmd_queue.mutex);
}

// Dedicated thread function to handle sending cruise commands
void *send_handler(void *arg) {
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
      ssize_t sret =
          send(follower_fd_list[i], &ldr_cmd_msg, sizeof(ldr_cmd_msg), 0);
      if (sret < 0) {
        perror("send to follower");
      }
    }
    pthread_mutex_unlock(&mutex_client_fd_list);
  }
}
