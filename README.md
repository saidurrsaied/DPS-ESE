# DPS-ESE_Heartbeat_monitoring_System

## Overview

This contains a simple **Leader–Follower heartbeat mechanism** written in **C** using **UDP sockets** and **POSIX threads**.

The system shows how a leader periodically sends heartbeat messages and how followers detect leader failure using a timeout.  

---

## How It Works

The program runs **two threads in parallel**:

### 1. Leader (Heartbeat Sender)
- Sends a heartbeat message at a fixed interval
- Sends the message to one or more follower IP addresses
- Retries sending if a failure occurs
- Stops after repeated failures and reports a connection failure

### 2. Follower (Heartbeat Monitor)
- Listens on a UDP port for heartbeat messages
- Updates the last received heartbeat time
- Detects leader disconnection if no heartbeat is received within a timeout
- Prints status changes only when the state changes

---

## Key Behavior (Configurable)

- Heartbeat interval: short periodic send (e.g. 100 ms)
- Timeout detection: follower declares failure after a few seconds
- Retry mechanism: leader retries before giving up
- Supports multiple follower IPs

(All values are defined as constants and can be changed easily.)

---

## Build Instructions

Compile using `gcc` with pthread support:

```bash
gcc -pthread leader_follower_heartbeat_2.c -o heartbeat
