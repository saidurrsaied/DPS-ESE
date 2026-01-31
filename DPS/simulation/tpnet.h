#ifndef TPNET_H
#define TPNET_H

#include <stdint.h>

/* Network utility functions for truck communication */

/**
 * join_platoon - Register follower with leader
 * @leader_FD: TCP socket connected to leader
 * @self_ip: Follower's IP address (null-terminated string)
 * @self_port: Follower's UDP listening port
 * 
 * Returns: Status code from send() operation
 */
int32_t join_platoon(int32_t leader_FD, const char *self_ip, uint16_t self_port);

/**
 * connect2Leader - Establish TCP connection to leader
 * 
 * Returns: Socket file descriptor on success, negative on error
 */
int32_t connect2Leader(void);

/**
 * createUDPServer - Create and bind UDP socket for receiving messages
 * @udp_port: UDP port number to listen on
 * 
 * Returns: Socket file descriptor on success, negative on error
 */
int32_t createUDPServer(uint16_t udp_port);

#endif
