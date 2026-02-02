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
extern Truck leader;
extern pthread_mutex_t mutex_leader_state;

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

    /* Initialize leader state for spawn computations */
    leader = (Truck){.x = 0.0f, .y = 0.0f, .speed = 0.0f, .dir = NORTH, .state = STOPPED};
    pthread_mutex_init(&mutex_leader_state, NULL);

    /* We'll create 4 socketpairs to simulate followers (test reconfiguration on disconnect) */
    const int N = 4;
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

    /* Spawn message follows */
    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_SPAWN);
    assert(msg0.payload.spawn.assigned_id == 1);

    /* Register second follower */
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
    assert(msg1.type == MSG_LDR_SPAWN);
    assert(msg1.payload.spawn.assigned_id == 2);

    /* Register third follower and expect EVT_PLATOON_FORMED */
    FollowerRegisterMsg reg2 = {0};
    strncpy(reg2.selfAddress.ip, "127.0.0.1", sizeof(reg2.selfAddress.ip));
    reg2.selfAddress.udp_port = 5003;

    register_new_follower(sv[2][0], &reg2);

    LD_MESSAGE msg2;
    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_ASSIGN_ID);
    assert(msg2.payload.assigned_id == 3);

    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_SPAWN);
    assert(msg2.payload.spawn.assigned_id == 3);

    /* Pop the formation event */
    Event formed_ev = pop_event(&leader_EventQ);
    assert(formed_ev.type == EVT_PLATOON_FORMED);

    /* Finalize topology and verify each follower receives update */
    extern void finalize_topology(void);
    finalize_topology();

    /* finalize_topology broadcasts fresh IDs first */
    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_ASSIGN_ID);
    assert(msg0.payload.assigned_id == 1);

    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_UPDATE_REAR);
    assert(msg0.payload.rearInfo.has_rearTruck == 1);
    assert(msg0.payload.rearInfo.rearTruck_Address.udp_port == 5002);

    r = read_ld_message(sv[1][1], &msg1);
    assert(r == sizeof(msg1));
    assert(msg1.type == MSG_LDR_ASSIGN_ID);
    assert(msg1.payload.assigned_id == 2);

    r = read_ld_message(sv[1][1], &msg1);
    assert(r == sizeof(msg1));
    assert(msg1.type == MSG_LDR_UPDATE_REAR);
    assert(msg1.payload.rearInfo.has_rearTruck == 1);
    assert(msg1.payload.rearInfo.rearTruck_Address.udp_port == 5003);

    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_ASSIGN_ID);
    assert(msg2.payload.assigned_id == 3);

    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_UPDATE_REAR);
    assert(msg2.payload.rearInfo.has_rearTruck == 0);

    /* Register a 4th follower and expect topology re-finalization */
    FollowerRegisterMsg reg3 = {0};
    strncpy(reg3.selfAddress.ip, "127.0.0.1", sizeof(reg3.selfAddress.ip));
    reg3.selfAddress.udp_port = 5004;

    register_new_follower(sv[3][0], &reg3);
    LD_MESSAGE msg3;
    r = read_ld_message(sv[3][1], &msg3);
    assert(r == sizeof(msg3));
    assert(msg3.type == MSG_LDR_ASSIGN_ID);
    assert(msg3.payload.assigned_id == 4);

    r = read_ld_message(sv[3][1], &msg3);
    assert(r == sizeof(msg3));
    assert(msg3.type == MSG_LDR_SPAWN);
    assert(msg3.payload.spawn.assigned_id == 4);

    /* Since formation is already complete, register_new_follower should push EVT_PLATOON_FORMED -- pop it */
    Event reformed = pop_event(&leader_EventQ);
    assert(reformed.type == EVT_PLATOON_FORMED);

    /* Re-finalize topology to include the 4th follower */
    finalize_topology();

    /* IDs first */
    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_ASSIGN_ID);
    assert(msg0.payload.assigned_id == 1);

    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_UPDATE_REAR);
    assert(msg0.payload.rearInfo.has_rearTruck == 1);
    assert(msg0.payload.rearInfo.rearTruck_Address.udp_port == 5002);

    r = read_ld_message(sv[1][1], &msg1);
    assert(r == sizeof(msg1));
    assert(msg1.type == MSG_LDR_ASSIGN_ID);
    assert(msg1.payload.assigned_id == 2);

    r = read_ld_message(sv[1][1], &msg1);
    assert(r == sizeof(msg1));
    assert(msg1.type == MSG_LDR_UPDATE_REAR);
    assert(msg1.payload.rearInfo.has_rearTruck == 1);
    assert(msg1.payload.rearInfo.rearTruck_Address.udp_port == 5003);

    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_ASSIGN_ID);
    assert(msg2.payload.assigned_id == 3);

    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_UPDATE_REAR);
    assert(msg2.payload.rearInfo.has_rearTruck == 1);
    assert(msg2.payload.rearInfo.rearTruck_Address.udp_port == 5004);

    r = read_ld_message(sv[3][1], &msg3);
    assert(r == sizeof(msg3));
    assert(msg3.type == MSG_LDR_ASSIGN_ID);
    assert(msg3.payload.assigned_id == 4);

    r = read_ld_message(sv[3][1], &msg3);
    assert(r == sizeof(msg3));
    assert(msg3.type == MSG_LDR_UPDATE_REAR);
    assert(msg3.payload.rearInfo.has_rearTruck == 0);

    /* Start the receiver thread */
    pthread_t recv_tid;
    pthread_create(&recv_tid, NULL, follower_message_receiver, NULL);

    /* Now simulate a disconnect of follower 2 (middle one) and expect re-finalization */
    close(sv[1][1]); /* follower side closes */

    /* Pop the EVT_PLATOON_FORMED event emitted by the receiver after handling disconnect */
    Event disconnect_ev = pop_event(&leader_EventQ);
    assert(disconnect_ev.type == EVT_PLATOON_FORMED);

    /* Re-finalize topology */
    finalize_topology();

    /* IDs first: follower 3 becomes position 2, follower 4 becomes position 3 */
    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_ASSIGN_ID);
    assert(msg0.payload.assigned_id == 1);

    /* Verify updated topology for remaining followers (1 -> 3, 3 -> 4, 4 -> none) */
    r = read_ld_message(sv[0][1], &msg0);
    assert(r == sizeof(msg0));
    assert(msg0.type == MSG_LDR_UPDATE_REAR);
    assert(msg0.payload.rearInfo.has_rearTruck == 1);
    assert(msg0.payload.rearInfo.rearTruck_Address.udp_port == 5003);

    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_ASSIGN_ID);
    assert(msg2.payload.assigned_id == 2);

    r = read_ld_message(sv[2][1], &msg2);
    assert(r == sizeof(msg2));
    assert(msg2.type == MSG_LDR_UPDATE_REAR);
    assert(msg2.payload.rearInfo.has_rearTruck == 1);
    assert(msg2.payload.rearInfo.rearTruck_Address.udp_port == 5004);

    r = read_ld_message(sv[3][1], &msg3);
    assert(r == sizeof(msg3));
    assert(msg3.type == MSG_LDR_ASSIGN_ID);
    assert(msg3.payload.assigned_id == 3);

    r = read_ld_message(sv[3][1], &msg3);
    assert(r == sizeof(msg3));
    assert(msg3.type == MSG_LDR_UPDATE_REAR);
    assert(msg3.payload.rearInfo.has_rearTruck == 0);

    /* Receiver is already running; send an intruder report from follower 1 (sv[0][1]) */
    FT_MESSAGE fmsg = {0};
    fmsg.type = MSG_FT_INTRUDER_REPORT;
    fmsg.payload.intruder.speed = 42;
    fmsg.payload.intruder.length = 5;

    send(sv[0][1], &fmsg, sizeof(fmsg), 0);

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

    printf("Leader integration test passed \n");
    return 0;
}
