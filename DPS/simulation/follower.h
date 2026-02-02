#ifndef FOLLOWER_H
#define FOLLOWER_H

#define _POSIX_C_SOURCE 200809L

#include "truckplatoon.h"
#include "event.h"
#include "tpnet.h"
#include <pthread.h>
#include <time.h>

/* External Truck state */
extern Truck follower;
extern NetInfo rearTruck_Address;
extern int32_t has_rearTruck;
extern int8_t simulation_running;

/* Thread IDs */
extern pthread_t udp_tid, tcp_tid, sm_tid;

extern pthread_mutex_t mutex_follower;
extern pthread_mutex_t mutex_topology;  // FOR has_rearTruck, rearTruck_Address
extern pthread_mutex_t mutex_sockets;   // FOR udp_sock, tcp2Leader

/* Event Queue */
extern EventQueue truck_EventQ;

/* Sockets */
extern int udp_sock;
extern int32_t tcp2Leader;

#include "matrix_clock.h"
extern int follower_idx;
extern int platoon_position;  /* Logical position in current platoon (1 = first after leader) */
extern MatrixClock follower_clock;

/* Intruder Context (for dynamic gap control) */
extern IntruderInfo current_intruder;
extern float current_target_gap;

/* Thread Functions */
void* udp_listener(void* arg);
void* tcp_listener(void* arg);
void* truck_state_machine(void* arg);

/* Event Queue Functions */
void set_realtime_priority(pthread_t tid, int policy, int priority);
Event pop_event(EventQueue* queue);
void push_event(EventQueue* queue, Event* event);
void event_queue_init(EventQueue* queue);

/* Emergency Functions */
void propagate_emergency(void);
void enter_emergency(void);
void handle_timer(void);
void exit_emergency(void); 

/* Intruder Functions */
int intruder_detected(void);
int intruder_speed(void);
int intruder_length(void);
uint32_t intruder_duration(void);
void notify_leader_intruder(IntruderInfo intruder);
void start_intruder_timer(uint32_t duration_ms);
void start_emergency_timer(uint32_t duration_ms);
void maybe_intruder(void);
void enter_intruder_follow(IntruderInfo intruder);
void exit_intruder_follow(void);
void update_intruder(IntruderInfo intruder);
void restore_nominal_distance(void);
void adjust_distance_from_front(FT_POSITION front_pos);

/* Graceful shutdown (implemented in follower.c) */
void follower_request_shutdown(const char* reason);
int follower_is_shutting_down(void);

#endif