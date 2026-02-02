#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <unistd.h>

//Meghana Includes
#include <termios.h>
#include <pthread.h>


#include "follower.h"
#include "intruder.h"
#include "event.h"
#include "matrix_clock.h"


#define INTRUDER_PROBABILITY 10  // %

/* follower_idx and follower_clock are defined in follower.c (extern in follower.h) */

int intruder_detected(void) {
    return (rand() % 100) < INTRUDER_PROBABILITY;
}

int intruder_speed(void) {
    return 30 + rand() % (120 - 30 + 1);
}

int intruder_length(void) {
    return 3 + rand() % (20 - 3 + 1);
}

uint32_t intruder_duration(void) {
    return 5000 + rand() % (10000 - 5000 + 1);
}

typedef struct {
    int32_t intruder_speed;
} IntruderReportMsg;




// intruder Time-out event generation 
void* intruder_timer_thread(void* arg) {
    uint32_t duration_ms = *(uint32_t*)arg;
    free(arg);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        perror("timerfd_create");
        return NULL;
    }

    struct itimerspec its = {0};
    its.it_value.tv_sec  = duration_ms / 1000;
    its.it_value.tv_nsec = (duration_ms % 1000) * 1000000;

    timerfd_settime(tfd, 0, &its, NULL);

    uint64_t expirations;
    read(tfd, &expirations, sizeof(expirations));

    Event e = {.type = EVT_INTRUDER_CLEAR};
    push_event(&truck_EventQ, &e);

    close(tfd);
    return NULL;
}


void start_intruder_timer(uint32_t duration_ms) {
    pthread_t tid;
    uint32_t* arg = malloc(sizeof(uint32_t));
    *arg = duration_ms;

    pthread_create(&tid, NULL, intruder_timer_thread, arg);
    pthread_detach(tid);
}


/*
Function: Randomly decide to generate intruder event. 
UNUSED currently as keyboard input is used instead 
*/

void maybe_intruder(void) {
    if (!intruder_detected())
        return;

    IntruderInfo intr = {
        .speed       = intruder_speed(),
        .length      = intruder_length(),
        .duration_ms = intruder_duration()
    };

    Event e = {
        .type = EVT_INTRUDER,
        .event_data.intruder = intr
    };

    push_event(&truck_EventQ, &e);
}





//FUNC: Entry action while still in intruder state to match intruder speed 
void update_intruder(IntruderInfo intruder) {
    pthread_mutex_lock(&mutex_follower);
    follower.speed = intruder.speed;
    pthread_mutex_unlock(&mutex_follower);
}

#define NOMINAL_FOLLOW_DISTANCE 10  // meters

void restore_nominal_distance(void) {
    // Placeholder logic — later replace with control law
    printf("Restoring nominal following distance: %d m\n",
           NOMINAL_FOLLOW_DISTANCE);
}





/*Meghana*/

int intruder_flag = 0;  // initially no intruder

/* Toggle intruder flag and return current state */
int toggle_intruder(void) {
    intruder_flag = !intruder_flag;
    return intruder_flag;
}

//intruder alert
void enter_intruder_follow(IntruderInfo intruder) {
    pthread_mutex_lock(&mutex_follower);
    follower.state = INTRUDER_FOLLOW;
    // Set target gap to maintain distance with intruder
    current_target_gap = TARGET_GAP + (float)intruder.length;
    current_intruder = intruder;
    // Initialize speed to intruder speed only if currently stopped
    if (follower.speed == 0) {
        follower.speed = (float)intruder.speed;
    }
    mc_local_event(&follower_clock, follower_idx); //mc: local intruder follow
    pthread_mutex_unlock(&mutex_follower);
    
    printf("[STATE] Follower entering INTRUDER_FOLLOW: speed=%d, length=%d, target_gap=%.1f\n",
           intruder.speed, intruder.length, current_target_gap);
}

//intruder exit alert
void exit_intruder_follow(void) {
    pthread_mutex_lock(&mutex_follower);
    follower.state = CRUISE;
    mc_local_event(&follower_clock, follower_idx); //mc: local intruder clear
    pthread_mutex_unlock(&mutex_follower);
    printf("[STATE] Intruder cleared → back to CRUISE\n");
}

// Helper: Notify leader about intruder
void notify_leader_intruder(IntruderInfo intruder) {

	mc_send_event(&follower_clock, follower_idx); //mc: send event
    FT_MESSAGE msg = {0};
    msg.type = MSG_FT_INTRUDER_REPORT;   // Already defined
    msg.payload.intruder = intruder;

    pthread_mutex_lock(&mutex_sockets);
    int temp_clock[NUM_TRUCKS][NUM_TRUCKS]; //mc
    memcpy(temp_clock, follower_clock.mc, sizeof(follower_clock.mc)); //mc
    //memcpy(msg.matrix_clock, follower_clock.mc, sizeof(follower_clock.mc)); //mc
    ssize_t ret = send(tcp2Leader, &msg, sizeof(msg), 0);
    pthread_mutex_unlock(&mutex_sockets);
    
    if (ret < 0) {
        perror("[INTRUDER] Failed to notify leader");
    } else {
        printf("[INTRUDER] Leader notified: speed=%d, length=%d\n",
               intruder.speed, intruder.length);
    }
}

// Thread function to create Intruder and Emergency Events based on keyboard inputs
// OPTIMIZED: Event-driven I/O with poll() - zero polling overhead, immediate responsiveness
void* keyboard_listener(void* arg) {
    (void)arg;
    
    /* 1. Setup Terminal Raw Mode ONCE at thread start */
    struct termios oldt, newt;
    if (tcgetattr(STDIN_FILENO, &oldt) < 0) {
        perror("tcgetattr");
        return NULL;
    }
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);      /* Disable canonical mode and echo */
    newt.c_cc[VMIN] = 0;                   /* Non-blocking mode */
    newt.c_cc[VTIME] = 0;
    
    /* Ensure stdout is line-buffered for immediate printing during raw mode */
    fflush(stdout);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt) < 0) {
        perror("tcsetattr");
        return NULL;
    }
    
    printf("\n[KEYBOARD] Ready: \n\t 'i' : toggle intruder,\n\t 'e' emergency, \n\t 'q' quit \n");
    fflush(stdout);
    
    /* 2. Setup poll() for event-driven input - blocks until data available */
    struct pollfd pfd = {
        .fd = STDIN_FILENO,
        .events = POLLIN  /* Wait for readable data */
    };
    
    while (1) {
        /* poll() blocks until stdin has data (no busy-waiting, no sleep) */
        int ret = poll(&pfd, 1, -1);  /* -1 timeout = block indefinitely */
        
        if (ret < 0) {
            perror("poll");
            break;
        }
        
        if (ret == 0) {
            /* Timeout (shouldn't happen with -1 timeout) */
            continue;
        }
        
        /* 3. Read available data when poll() indicates readiness */
        if (pfd.revents & POLLIN) {
            char c = 0;
            ssize_t nread = read(STDIN_FILENO, &c, 1);
            if (nread < 0) {
                perror("read");
                break;
            }
            if (nread == 0) {
                /* EOF */
                break;
            }
            
            if (c == 'i' || c == 'I') {
                int state = toggle_intruder();  /* toggle intruder flag */

                IntruderInfo intruder = {
                    .length = INTRUDER_LENGTH,
                    .speed  = INTRUDER_SPEED,
                    .duration_ms = 0  /* toggle mode - no timeout */
                };

                Event evt;
                if (state) {
                    printf("\n[KEYBOARD] Intruder detected - press 'i' again to clear\n");
                    evt.type = EVT_INTRUDER;
                } else {
                    printf("\n[KEYBOARD] Intruder cleared\n");
                    evt.type = EVT_INTRUDER_CLEAR;
                }
                evt.event_data.intruder = intruder;
                push_event(&truck_EventQ, &evt);
            }

            if (c == 'e' || c == 'E') {
                printf("\n[KEYBOARD] Emergency event triggered\n");
                Event evt = {.type = EVT_EMERGENCY};
                push_event(&truck_EventQ, &evt);
            }
            
            if (c == 'q' || c == 'Q') {
                printf("\n[KEYBOARD] Quit command received\n");
                break;
            }
        }
    }
    
    /* 4. Restore Terminal ONCE at thread exit */
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
    printf("[KEYBOARD] Terminal restored\n");
    fflush(stdout);
    return NULL;
}

