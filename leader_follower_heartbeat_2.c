#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>     /* for sleep/usleep on POSIX systems */
#include <arpa/inet.h>   /* for inet_pton, htons */
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>


#define HEARTBEAT_INTERVAL_MS   100    /* Leader sends every 100 ms */
#define RECONNECT_DELAY_MS      500    /* Wait 500 ms before retry after failure */
#define MAX_RECONNECT_ATTEMPTS  10     /* Give up after 10 consecutive failures */
#define HEARTBEAT_TIMEOUT_MS    200    /* Follower declares leader dead after 200 ms */

/* Port number on which the follower listens for heartbeat messages. */
#define FOLLOWER_LISTEN_PORT     6000


static const char *follower_addresses[] = {
    "127.0.0.1", "127.0.0.2", "127.0.0.3", "127.0.0.4","127.0.0.5"   /* loopback follower */
    /* Additional follower IP strings can be added here */
};
/* Count how many follower addresses are in the array. */
static const size_t num_followers = sizeof(follower_addresses) / sizeof(follower_addresses[0]);


static int is_leader_active = 0;

static struct timespec last_heartbeat_time;

static pthread_mutex_t heartbeat_mutex = PTHREAD_MUTEX_INITIALIZER;


static void sleep_for_milliseconds(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000UL;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        /* If interrupted by a signal, nanosleep returns -1 and
         * updates ts with the remaining time.  Loop until it
         * completes successfully. */
    }
}

/*
 * Leader heartbeat thread.
 */
static void *leaderHeartbeatThread(void *arg)
{
    (void)arg;
    /* Define the heartbeat message and its length.  We send this
     * text to followers so they know the leader is alive. */
    const char heartbeat_message[] = "Leader Active";
    const size_t message_length = strlen(heartbeat_message);

    /* Track how many consecutive reconnection attempts have been made.
     * When this exceeds MAX_RECONNECT_ATTEMPTS the thread stops. */
    int reconnect_attempts = 0;
    /* Flag indicating whether we should continue running the loop. */
    int running = 1;

    /* Create the UDP socket for sending.  Using AF_INET and
     * SOCK_DGRAM selects an IPv4 UDP socket【804940706067835†L34-L87】. */
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("leader: socket");
        return NULL;
    }

    while (running) {
        if (socket_fd >= 0) {
            /* For each follower in our list, build the destination
             * address structure and send the heartbeat. */
            for (size_t i = 0; i < num_followers; i++) {
                struct sockaddr_in dest_addr;
                memset(&dest_addr, 0, sizeof(dest_addr));
                dest_addr.sin_family = AF_INET;
                dest_addr.sin_port = htons(FOLLOWER_LISTEN_PORT);
                if (inet_pton(AF_INET, follower_addresses[i], &dest_addr.sin_addr) <= 0) {
                    fprintf(stderr, "leader: invalid follower IP %s\n", follower_addresses[i]);
                    continue;
                }
                ssize_t bytes_sent = sendto(socket_fd, heartbeat_message, message_length, 0,
                                           (struct sockaddr *)&dest_addr, (socklen_t)sizeof(dest_addr));
                if (bytes_sent < 0) {
                    /* A send failure typically indicates a local error (e.g., no
                     * route) because UDP has no connection state.  Close the
                     * socket and trigger a reconnect sequence. */
                    perror("leader: sendto");
                    close(socket_fd);
                    socket_fd = -1;
                    break;
                } else {
                    printf("leader: sent heartbeat to %s\n", follower_addresses[i]);
                }
            }
            /* Pause before sending the next heartbeat. */
            sleep_for_milliseconds(HEARTBEAT_INTERVAL_MS);
        } else {
            /* Socket is closed due to an error – attempt to recreate it. */
            reconnect_attempts++;
            if (reconnect_attempts > MAX_RECONNECT_ATTEMPTS) {
                fprintf(stderr, "leader: unable to reconnect, giving up\n");
                running = 0;
                break;
            }
            fprintf(stderr, "leader: send failed; reconnecting attempt %d/%d\n",
                    reconnect_attempts, MAX_RECONNECT_ATTEMPTS);
            sleep_for_milliseconds(RECONNECT_DELAY_MS);
            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_fd < 0) {
                perror("leader: socket (reconnect)");
                continue;
            }
        }
    }

    if (socket_fd >= 0) {
        close(socket_fd);
    }
    printf("leader: heartbeat thread exiting\n");
    return NULL;
}

/*
 * Follower monitoring thread
 */
static void *followerMonitorThread(void *arg)
{
    (void)arg;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("follower: socket");
        return NULL;
    }

    /* Bind the socket to all local interfaces on FOLLOWER_LISTEN_PORT【502487870642587†L48-L51】. */
    struct sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    localAddr.sin_port = htons(FOLLOWER_LISTEN_PORT);
    if (bind(socket_fd, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
        perror("follower: bind");
        close(socket_fd);
        return NULL;
    }

    /* Set a small receive timeout so that recvfrom() does not block
     * indefinitely when there is no traffic.  This allows the thread
     * to periodically check the elapsed time. */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  /* 100 ms */
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("follower: setsockopt");
        /* Continue anyway – the socket will block but the heartbeat
         * monitoring logic still works, albeit with coarser granularity. */
    }

    /* Buffer to hold incoming UDP data. */
    char receive_buffer[256];
    /* Keep track of the last status we reported so we only print
     * status changes.  -1 means no status has been printed yet. */
    int last_reported_status = -1;
    /* Initialize last_heartbeat_time to the current time so that
     * initial timeouts are measured relative to program start. */
    clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);

    while (1) {
        struct sockaddr_in srcAddr;
        socklen_t srcLen = sizeof(srcAddr);
        ssize_t bytes_received = recvfrom(socket_fd, receive_buffer, sizeof(receive_buffer) - 1, 0,
                                         (struct sockaddr *)&srcAddr, &srcLen);
        if (bytes_received > 0) {
            /* Null‑terminate the received data so strstr() works on it */
            receive_buffer[bytes_received] = '\0';
            /* If the message contains the leader heartbeat string, update the timestamp. */
            if (strstr(receive_buffer, "Leader Active") != NULL) {
                /* Record the arrival time of this heartbeat and mark
                 * the leader as alive.  Protect the shared state with
                 * the heartbeat mutex so both variables are updated
                 * atomically. */
                pthread_mutex_lock(&heartbeat_mutex);
                clock_gettime(CLOCK_MONOTONIC, &last_heartbeat_time);
                is_leader_active = 1;
                pthread_mutex_unlock(&heartbeat_mutex);
                /* Print once per message for debugging. */
                printf("follower: received heartbeat from leader\n");
            }
        }
        /* Compute the time since the last heartbeat. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        pthread_mutex_lock(&heartbeat_mutex);
        double elapsed_ms = (now.tv_sec - last_heartbeat_time.tv_sec) * 1000.0 +
                            (now.tv_nsec - last_heartbeat_time.tv_nsec) / 1e6;
        /* If no heartbeat has been seen within the timeout window,
         * mark the leader as inactive; otherwise mark it active. */
        if (elapsed_ms > HEARTBEAT_TIMEOUT_MS) {
            is_leader_active = 0;
        } else {
            is_leader_active = 1;
        }
        int current_status = is_leader_active;
        pthread_mutex_unlock(&heartbeat_mutex);
        /* Report status transitions only. */
        if (current_status != last_reported_status) {
            if (current_status) {
                printf("follower: leader is active\n");
            } else {
                printf("follower: leader is disconnected from followers\n");
            }
            last_reported_status = current_status;
        }
        /* Small sleep to avoid busy waiting. */
        sleep_for_milliseconds(50);
    }

    /* Unreachable: the loop above is infinite.  If a shutdown condition
     * is needed, add logic to break out of the loop and close the socket. */
    close(socket_fd);
    return NULL;
}

int main(void)
{
    /* Create the leader and follower threads.  The POSIX function
     * pthread_create() starts a new thread and invokes the given
     * start_routine(), passing arg as its argument【660345157720278†L30-L36】.
     * The function returns 0 on success or an error number on failure. */
    /* Thread identifiers for the leader (sender) and follower (monitor) threads */
    pthread_t sender_thread;
    pthread_t monitor_thread;

    int ret;
    /* Create the leader heartbeat thread.  This thread will send
     * heartbeats until it either encounters repeated errors or the
     * program is terminated. */
    ret = pthread_create(&sender_thread, NULL, leaderHeartbeatThread, NULL);
    if (ret != 0) {
        fprintf(stderr, "main: failed to create sender thread: %s\n", strerror(ret));
        return EXIT_FAILURE;
    }

    /* Create the follower monitoring thread.  This thread runs
     * indefinitely and updates the is_leader_active flag based on
     * incoming heartbeats. */
    ret = pthread_create(&monitor_thread, NULL, followerMonitorThread, NULL);
    if (ret != 0) {
        fprintf(stderr, "main: failed to create monitor thread: %s\n", strerror(ret));
        return EXIT_FAILURE;
    }

    /* Wait for the sender thread to finish.  In this demonstration the
     * sender thread will terminate only if repeated send failures occur.
     * The monitor thread runs indefinitely.  In a real application you
     * would coordinate thread termination using additional flags or
     * signals. */
    pthread_join(sender_thread, NULL);
    /* Optionally wait on monitor_thread.  This program will never
     * reach here because monitor_thread has an infinite loop. */
    pthread_join(monitor_thread, NULL);
    return EXIT_SUCCESS;
}