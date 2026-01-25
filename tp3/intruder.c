//intruder.c

#include "intruder.h"
#include "follower.h"
#include "event.h"
#include "follower.h"  // for truck_EventQ
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

int intruder_flag = 0;  // initially no intruder

/* Toggle intruder flag and return current state */
int toggle_intruder(void) {
    intruder_flag = !intruder_flag;
    return intruder_flag;
}

//intruder alert
void enter_intruder_follow(IntruderInfo intruder) {
    follower.state = INTRUDER_FOLLOW;
    follower.speed = intruder.speed;  // slow down to intruder's speed
    printf("[STATE] Follower entering INTRUDER_FOLLOW: speed=%d, length=%d\n",
           intruder.speed, intruder.length);
}

//intruder exit alert
void exit_intruder_follow(void) {
    follower.state = CRUISE;
    printf("[STATE] Intruder cleared → back to CRUISE\n");
}

// read a single char from console without Enter key
char getch(void) {
    char buf = 0;
    struct termios old = {0};
    if (tcgetattr(0, &old) < 0) perror("tcgetattr()");
    old.c_lflag &= ~ICANON;
    old.c_lflag &= ~ECHO;
    if (tcsetattr(0, TCSANOW, &old) < 0) perror("tcsetattr ICANON");
    if (read(0, &buf, 1) < 0) perror("read()");
    old.c_lflag |= ICANON;
    old.c_lflag |= ECHO;
    if (tcsetattr(0, TCSADRAIN, &old) < 0) perror("tcsetattr ~ICANON");
    return buf;
}


// Helper: Notify leader about intruder
void notify_leader_intruder(IntruderInfo intruder) {
    FT_MESSAGE msg = {0};
    msg.type = MSG_FT_INTRUDER_REPORT;   // Already defined
    msg.payload.intruder = intruder;

    ssize_t ret = send(tcp2Leader, &msg, sizeof(msg), 0);
    if (ret < 0) {
        perror("[INTRUDER] Failed to notify leader");
    } else {
        printf("[INTRUDER] Leader notified: speed=%d, length=%d\n",
               intruder.speed, intruder.length);
    }
}

// Keyboard monitoring thread
void* intruder_keyboard_listener(void* arg) {
    (void)arg;
    while (1) {
        char c = getch();
        if (c == 'i' || c == 'I') {
            int state = toggle_intruder();  // toggle intruder flag

            IntruderInfo intruder = {
                .length = INTRUDER_LENGTH,
                .speed  = INTRUDER_SPEED,
                .duration_ms = 0  // can be 0 for now
            };

            Event evt;
            if (state) {
                printf("[INTRUDER] Intruder detected!\n");
                evt.type = EVT_INTRUDER;
            } else {
                printf("[INTRUDER] Intruder cleared!\n");
                evt.type = EVT_INTRUDER_CLEAR;
            }
            evt.event_data.intruder = intruder;
            push_event(&truck_EventQ, &evt);
        }
    }
    return NULL;
}
