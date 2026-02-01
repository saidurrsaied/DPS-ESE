#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <pthread.h>

#include "../event.h"

/* Externs from leader.c */
extern EventQueue leader_EventQ;
extern void register_new_follower(int fd, FollowerRegisterMsg* reg_msg);
extern void* follower_message_receiver(void* arg);
extern MatrixClock leader_clock;

/* We need to include the actual definitions used in messages */
#include "../truckplatoon.h"

/* helper to read LD_MESSAGE */
ssize_t read_ld_message(int fd, LD_MESSAGE* out) {
    return recv(fd, out, sizeof(*out), MSG_WAITALL);
}

int main(void) {
    printf("Starting leader integration test...\n");

    /* Initialize event queue */
    event_queue_init(&leader_EventQ);

    /* Initialize leader matrix clock (opaque) */
    mc_init(&leader_clock);

    /* We'll create 3 socketpairs to simulate followers */
    const int N = 3;
    int sv[N][2];
    for (int i = 0; i < N; i++) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) < 0) {
            perror("socketpair");
            return 1;
        }
    }

    /* Register first follower */
    FollowerRegisterMsg reg0 = {0};
    strncpy(reg0.selfAddress.ip, "127.0.0.1", sizeof(reg0.selfAddress.ip));
    reg0.selfAddress.udp_port = 5001;

    register_new_follower(sv[0][0], &reg0);

    LD_MESSAGE msg0;
    ssize_t r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_ASSIGN_ID);
    assert(msg0.payload.assigned_id == 1);

    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_UPDATE_REAR);
    assert(msg0.payload.rearInfo.has_rearTruck == 0);

    /* Register second follower (should notify previous) */
    FollowerRegisterMsg reg1 = {0};
    strncpy(reg1.selfAddress.ip, "127.0.0.1", sizeof(reg1.selfAddress.ip));
    reg1.selfAddress.udp_port = 5002;

    register_new_follower(sv[1][0], &reg1);

    LD_MESSAGE msg1;
    r = read_ld_message(sv[1][1], &msg1);
    assert(r == sizeof(msg1));
    assert(msg1.type == MSG_LDR_ASSIGN_ID);
    assert(msg1.payload.assigned_id == 2);

    r = read_ld_message(sv[1][1], &msg1);
    assert(r == sizeof(msg1));
    assert(msg1.type == MSG_LDR_UPDATE_REAR);
    assert(msg1.payload.rearInfo.has_rearTruck == 0);

    /* The previous follower (sv[0][1]) should receive an update_rearInfo */
    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_UPDATE_REAR);
    assert(msg0.payload.rearInfo.has_rearTruck == 1);
    assert(msg0.payload.rearInfo.rearTruck_Address.udp_port == 5002);

    /* Start the receiver thread */
    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, follower_message_receiver, NULL);

    /* Send an intruder report from follower 1 (sv[1][1]) */
    FT_MESSAGE fmsg = {0};
    fmsg.type = MSG_FT_INTRUDER_REPORT;
    fmsg.payload.intruder.speed = 42;
    fmsg.payload.intruder.length = 5;

    send(sv[1][1], &fmsg, sizeof(fmsg), 0);

    /* Pop event from leader event queue */
    Event ev = pop_event(&leader_EventQ);
    assert(ev.type == EVT_FOLLOWER_MSG);
    assert(ev.event_data.follower_msg.msg.type == MSG_FT_INTRUDER_REPORT);
    assert(ev.event_data.follower_msg.msg.payload.intruder.speed == 42);

    /* Clean up
       Cancel receiver thread (select is a cancellation point) */
    pthread_cancel(recv_tid);
    pthread_join(recv_tid, NULL);

    for (int i = 0; i < N; i++) {
        close(sv[i][0]);
        close(sv[i][1]);
    }

    printf("Leader integration test passed ✅\n");
    return 0;
}
