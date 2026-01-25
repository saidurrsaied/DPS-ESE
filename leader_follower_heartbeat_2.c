#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>

#include "truckplatoon.h"

#define main leader_main
#include "leader.c"
#undef main

/* ------------------- SETTINGS ------------------- */
#define HEARTBEAT_INTERVAL_MS   100
#define RECONNECT_DELAY_MS      500
#define MAX_RECONNECT_ATTEMPTS  10

#define HEARTBEAT_TIMEOUT_MS    5000
#define FOLLOWER_DEFAULT_LISTEN_PORT 6000

/* ------------------- SHARED STATE ------------------- */
static int is_leader_active = 0;
static struct timespec last_heartbeat_time;
static pthread_mutex_t heartbeat_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint16_t follower_listen_port = FOLLOWER_DEFAULT_LISTEN_PORT;
static int leader_tcp_fd = -1;
static char follower_leader_ip[64];
static char follower_self_ip[64];
static uint16_t follower_leader_port = LEADER_PORT;

typedef enum {
    MODE_LEADER,
    MODE_FOLLOWER,
    MODE_BOTH
} RunMode;

static pthread_mutex_t leader_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t leader_ready_cond = PTHREAD_COND_INITIALIZER;
static int leader_ready = 0;

/* ------------------- HELPERS ------------------- */
static void sleep_for_milliseconds(int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        // retry
    }
}

static double ms_since(struct timespec now, struct timespec then)
{
    double sec  = (double)(now.tv_sec - then.tv_sec) * 1000.0;
    double nsec = (double)(now.tv_nsec - then.tv_nsec) / 1000000.0;
    return sec + nsec;
}

static void signal_leader_ready(int status)
{
    pthread_mutex_lock(&leader_ready_mutex);
    leader_ready = status;
    pthread_cond_broadcast(&leader_ready_cond);
    pthread_mutex_unlock(&leader_ready_mutex);
}

static int wait_for_leader_ready(void)
{
    pthread_mutex_lock(&leader_ready_mutex);
    while (leader_ready == 0) {
        pthread_cond_wait(&leader_ready_cond, &leader_ready_mutex);
    }
    int status = leader_ready;
    pthread_mutex_unlock(&leader_ready_mutex);
    return status;
}

static int parse_port(const char *s, uint16_t *out)
{
    char *end = NULL;
    long val = strtol(s, &end, 10);
    if (s == end || *end != '\0' || val <= 0 || val > 65535) {
        return -1;
    }
    *out = (uint16_t)val;
    return 0;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [--leader|--follower|--both] [options]\n", prog);
    printf("Options:\n");
    printf("  --leader             Leader + heartbeat sender (default: both)\n");
    printf("  --follower           Follower heartbeat monitor only\n");
    printf("  --both               Run leader + one follower monitor in one process\n");
    printf("  --listen-port <port> UDP port for follower heartbeat monitor (default %d)\n",
           FOLLOWER_DEFAULT_LISTEN_PORT);
    printf("  --leader-ip <ip>      Leader IP for follower registration (default %s)\n",
           LEADER_IP);
    printf("  --leader-port <port>  Leader TCP port for registration (default %d)\n",
           LEADER_PORT);
    printf("  --self-ip <ip>        Self IP for registration (default %s)\n",
           LEADER_IP);
    printf("  --help                Show this help\n");
}

static int connect_to_leader(const char *leader_ip, uint16_t leader_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("follower: socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(leader_port);

    if (inet_pton(AF_INET, leader_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "follower: invalid leader IP: %s\n", leader_ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("follower: connect");
        close(fd);
        return -1;
    }

    return fd;
}

static int register_with_leader(int fd, const char *self_ip, uint16_t self_port)
{
    FollowerRegisterMsg reg;
    memset(&reg, 0, sizeof(reg));
    strncpy(reg.selfAddress.ip, self_ip, sizeof(reg.selfAddress.ip) - 1);
    reg.selfAddress.udp_port = self_port;

    ssize_t sent = send(fd, &reg, sizeof(reg), 0);
    if (sent < 0) {
        perror("follower: register");
        return -1;
    }

    printf("[FOLLOWER] registered with leader as %s:%d\n", self_ip, self_port);
    return 0;
}

static int follower_reconnect(void)
{
    int fd = connect_to_leader(follower_leader_ip, follower_leader_port);
    if (fd < 0) {
        return -1;
    }

    if (register_with_leader(fd, follower_self_ip, follower_listen_port) != 0) {
        close(fd);
        return -1;
    }

    if (leader_tcp_fd >= 0) {
        close(leader_tcp_fd);
    }
    leader_tcp_fd = fd;
    return 0;
}

static int snapshot_followers(NetInfo *out, int max)
{
    int count = 0;
    pthread_mutex_lock(&mutex_client_fd_list);
    count = follower_count;
    if (count > max) {
        count = max;
    }
    for (int i = 0; i < count; i++) {
        out[i] = follower_Addresses[i];
    }
    pthread_mutex_unlock(&mutex_client_fd_list);
    return count;
}

/* ------------------- LEADER RUNTIME (FROM leader.c) ------------------- */
static void *leaderRuntimeThread(void *arg)
{
    (void)arg;

    srand(time(NULL));
    pthread_mutex_init(&mutex_client_fd_list, NULL);

    leader = (Truck){0,0,1,NORTH,CRUISE};

    leader_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (leader_socket_fd < 0) {
        perror("leader: socket");
        signal_leader_ready(-1);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(LEADER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(leader_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("leader: bind");
        signal_leader_ready(-1);
        return NULL;
    }
    if (listen(leader_socket_fd, MAX_FOLLOWERS) < 0) {
        perror("leader: listen");
        signal_leader_ready(-1);
        return NULL;
    }

    cmd_queue.head = 0;
    cmd_queue.tail = 0;
    pthread_mutex_init(&cmd_queue.mutex, NULL);
    pthread_cond_init(&cmd_queue.not_empty, NULL);

    if (pthread_create(&acceptor_tid, NULL, accept_handler, NULL) != 0) {
        perror("pthread_create acceptor");
        signal_leader_ready(-1);
        return NULL;
    }
    if (pthread_create(&sender_tid, NULL, send_handler, NULL) != 0) {
        perror("pthread_create sender");
        signal_leader_ready(-1);
        return NULL;
    }

    printf("Leader started\n");
    signal_leader_ready(1);

    while (1) {
        leader_decide_next_state(&leader);
        move_truck(&leader);

        LeaderCommand ldr_cmd = {
            .command_id = cmd_id++,
            .leader = leader
        };

        queue_commands(&ldr_cmd);

        printf("Leader pos (%d,%d), sent command: %lu \n", leader.x, leader.y, cmd_id);
        fflush(stdout);

        sleep(LEADER_SLEEP);
    }

    return NULL;
}

/* ------------------- THREAD 1: LEADER SENDS HEARTBEAT ------------------- */
static void *leaderHeartbeatThread(void *arg)
{
    (void)arg;

    const char *heartbeat_msg = "Leader Active";
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("leader: socket");
        return NULL;
    }

    int reconnect_attempts = 0;
    int running = 1;
    int waiting_logged = 0;

    while (running) {
        NetInfo followers[MAX_FOLLOWERS];
        int follower_count_snapshot = snapshot_followers(followers, MAX_FOLLOWERS);

        if (follower_count_snapshot == 0) {
            if (!waiting_logged) {
                printf("[LEADER] waiting for followers to register...\n");
                waiting_logged = 1;
            }
            sleep_for_milliseconds(HEARTBEAT_INTERVAL_MS);
            continue;
        }
        waiting_logged = 0;

        int send_failed = 0;

        for (int i = 0; i < follower_count_snapshot; i++) {
            if (followers[i].udp_port == 0 || followers[i].ip[0] == '\0') {
                fprintf(stderr, "leader: follower %d has empty address\n", i);
                continue;
            }

            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family = AF_INET;
            dest.sin_port   = htons(followers[i].udp_port);

            if (inet_pton(AF_INET, followers[i].ip, &dest.sin_addr) <= 0) {
                fprintf(stderr, "leader: invalid follower IP: %s\n", followers[i].ip);
                continue;
            }

            ssize_t sent = sendto(sock,
                                  heartbeat_msg,
                                  strlen(heartbeat_msg),
                                  0,
                                  (struct sockaddr *)&dest,
                                  sizeof(dest));

            if (sent < 0) {
                perror("leader: sendto");
                send_failed = 1;
                break;
            } else {
                printf("[LEADER] sent heartbeat to %s:%d\n",
                       followers[i].ip, followers[i].udp_port);
            }
        }

        if (!send_failed) {
            reconnect_attempts = 0;
            sleep_for_milliseconds(HEARTBEAT_INTERVAL_MS);
            continue;
        }

        reconnect_attempts++;
        if (reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
            printf("[LEADER] connection with leader failed (gave up after %d tries)\n",
                   MAX_RECONNECT_ATTEMPTS);
            running = 0;
            break;
        }

        printf("[LEADER] send failed -> retry %d/%d after %d ms\n",
               reconnect_attempts, MAX_RECONNECT_ATTEMPTS, RECONNECT_DELAY_MS);

        sleep_for_milliseconds(RECONNECT_DELAY_MS);

        close(sock);
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            perror("leader: socket (reconnect)");
        }
    }

    if (sock >= 0) close(sock);
    printf("[LEADER] sender thread stopped\n");
    return NULL;
}

/* ------------------- THREAD 2: FOLLOWER MONITORS HEARTBEAT ------------------- */
static void *followerMonitorThread(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("follower: socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(follower_listen_port);

    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("follower: bind");
        close(sock);
        return NULL;
    }

    printf("[FOLLOWER] listening for heartbeats on UDP port %d\n", follower_listen_port);

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 100000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[256];
    int last_reported = -1;
    int reconnect_attempts = 0;
    int gave_up = 0;
    int reconnect_succeeded = 0;
    struct timespec last_reconnect_time;

    clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
    last_reconnect_time = last_heartbeat_time;

    while (1) {
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (n > 0) {
            buf[n] = '\0';

            if (strstr(buf, "Leader Active") != NULL) {
                pthread_mutex_lock(&heartbeat_mutex);
                clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
                is_leader_active = 1;
                pthread_mutex_unlock(&heartbeat_mutex);

                printf("[FOLLOWER] received heartbeat: \"%s\"\n", buf);
                reconnect_attempts = 0;
                gave_up = 0;
                reconnect_succeeded = 0;
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        pthread_mutex_lock(&heartbeat_mutex);
        double elapsed = ms_since(now, last_heartbeat_time);
        if (elapsed > HEARTBEAT_TIMEOUT_MS) {
            is_leader_active = 0;
        } else {
            is_leader_active = 1;
        }
        int current = is_leader_active;
        pthread_mutex_unlock(&heartbeat_mutex);

        if (current != last_reported) {
            if (current) {
                printf("[FOLLOWER] leader is active\n");
            }
            last_reported = current;
        }

        if (!current && !gave_up && !reconnect_succeeded) {
            double since_reconnect = ms_since(now, last_reconnect_time);
            if (since_reconnect >= RECONNECT_DELAY_MS) {
                last_reconnect_time = now;

                if (follower_reconnect() == 0) {
                    printf("[FOLLOWER] reconnect successful\n");
                    reconnect_attempts = 0;
                    reconnect_succeeded = 1;
                } else {
                    reconnect_attempts++;
                    printf("[FOLLOWER] reconnect attempt %d/%d failed\n",
                           reconnect_attempts, MAX_RECONNECT_ATTEMPTS);
                    if (reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
                        printf("[FOLLOWER] leader is disconnected from followers (timeout > %d ms)\n",
                               HEARTBEAT_TIMEOUT_MS);
                        gave_up = 1;
                    }
                }
            }
        }

        sleep_for_milliseconds(50);
    }

    close(sock);
    return NULL;
}

/* ------------------- MAIN ------------------- */
int main(int argc, char **argv)
{
    RunMode mode = MODE_BOTH;
    const char *leader_ip = LEADER_IP;
    uint16_t leader_port = LEADER_PORT;
    const char *self_ip = LEADER_IP;
    uint16_t listen_port = FOLLOWER_DEFAULT_LISTEN_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--leader") == 0) {
            mode = MODE_LEADER;
        } else if (strcmp(argv[i], "--follower") == 0) {
            mode = MODE_FOLLOWER;
        } else if (strcmp(argv[i], "--both") == 0) {
            mode = MODE_BOTH;
        } else if (strcmp(argv[i], "--listen-port") == 0 && i + 1 < argc) {
            if (parse_port(argv[++i], &listen_port) != 0) {
                fprintf(stderr, "invalid listen port\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--leader-ip") == 0 && i + 1 < argc) {
            leader_ip = argv[++i];
        } else if (strcmp(argv[i], "--leader-port") == 0 && i + 1 < argc) {
            if (parse_port(argv[++i], &leader_port) != 0) {
                fprintf(stderr, "invalid leader port\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--self-ip") == 0 && i + 1 < argc) {
            self_ip = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    strncpy(follower_leader_ip, leader_ip, sizeof(follower_leader_ip) - 1);
    follower_leader_ip[sizeof(follower_leader_ip) - 1] = '\0';
    strncpy(follower_self_ip, self_ip, sizeof(follower_self_ip) - 1);
    follower_self_ip[sizeof(follower_self_ip) - 1] = '\0';
    follower_leader_port = leader_port;

    pthread_t leader_thread;
    pthread_t follower_thread;
    pthread_t leader_runtime_thread;
    int start_leader = (mode == MODE_LEADER || mode == MODE_BOTH);
    int start_follower = (mode == MODE_FOLLOWER || mode == MODE_BOTH);

    if (start_leader) {
        if (pthread_create(&leader_runtime_thread, NULL, leaderRuntimeThread, NULL) != 0) {
            perror("pthread_create leader runtime");
            return 1;
        }

        if (wait_for_leader_ready() < 0) {
            fprintf(stderr, "leader failed to start\n");
            return 1;
        }

        if (pthread_create(&leader_thread, NULL, leaderHeartbeatThread, NULL) != 0) {
            perror("pthread_create leader heartbeat");
            return 1;
        }
    }

    if (start_follower) {
        follower_listen_port = listen_port;

        leader_tcp_fd = connect_to_leader(leader_ip, leader_port);
        if (leader_tcp_fd < 0) {
            return 1;
        }

        if (register_with_leader(leader_tcp_fd, self_ip, follower_listen_port) != 0) {
            close(leader_tcp_fd);
            return 1;
        }

        if (pthread_create(&follower_thread, NULL, followerMonitorThread, NULL) != 0) {
            perror("pthread_create follower heartbeat");
            close(leader_tcp_fd);
            return 1;
        }
    }

    if (start_leader) {
        pthread_join(leader_thread, NULL);
        pthread_join(leader_runtime_thread, NULL);
    }

    if (start_follower) {
        pthread_join(follower_thread, NULL);
    }

    if (leader_tcp_fd >= 0) {
        close(leader_tcp_fd);
    }

    return 0;
}
