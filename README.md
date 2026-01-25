# DPS-ESE_Heartbeat_monitoring_System

## Overview

This contains a simple **Leader–Follower heartbeat mechanism** written in **C** using **UDP sockets** and **POSIX threads**.

The system shows how a leader periodically sends heartbeat messages and how followers detect leader failure using a timeout.  

---

This repo is a small networking demo for a **truck platooning** setup.

- A **Leader** runs the main simulation (moves and sends commands).
- Followers **register** to the leader using **TCP**.
- After registration, the leader sends a **UDP heartbeat** (“Leader Active”) to each follower every few milliseconds.
- Each follower **listens on UDP** and checks if the leader is still alive.
- If heartbeats stop, the follower tries to **reconnect and register again** (with retry limits).

---

## What this project does (in simple words)

###  Leader side
1. Starts a TCP server (the normal leader program).
2. Accepts follower registrations (followers send their IP + UDP port).
3. Starts a heartbeat thread that:
   - takes the registered follower list
   - sends `Leader Active` via UDP repeatedly

###  Follower side
1. Connects to leader over TCP.
2. Registers itself (its IP + UDP listen port).
3. Listens for UDP heartbeats.
4. If heartbeat is missing for too long:
   - marks leader as inactive
   - tries to reconnect and register again (max attempts)

---

## How it works (Networking)

### TCP (registration)
Follower → Leader:
- “Hi, I am here. My UDP address is `X.X.X.X:PORT`.”

Leader stores these addresses so it knows where to send UDP heartbeats.

### UDP (heartbeat)
Leader → Follower:
- sends message: `Leader Active`
- repeated every `HEARTBEAT_INTERVAL_MS`

Follower:
- if no heartbeat for `HEARTBEAT_TIMEOUT_MS`, it assumes leader is down/disconnected.

---

## Threads used

This file runs multiple threads:

### Leader threads
- **Leader runtime thread**: runs the actual leader simulation and sends commands (from `leader.c`)
- **Leader heartbeat thread**: sends UDP heartbeat messages

### Follower thread
- **Follower monitor thread**: receives heartbeat + checks timeout + reconnect logic

---

## Settings (you can tweak these)

Inside the code:

- `HEARTBEAT_INTERVAL_MS = 100`  
  Leader sends heartbeat every 100 ms

- `HEARTBEAT_TIMEOUT_MS = 5000`  
  Follower waits 5 seconds before saying “leader is dead”

- `RECONNECT_DELAY_MS = 500`  
  Wait 500 ms between reconnect tries

- `MAX_RECONNECT_ATTEMPTS = 10`  
  Stop trying after 10 failed reconnects

- Default follower UDP listen port:
  - `FOLLOWER_DEFAULT_LISTEN_PORT = 6000`

---

## Build

This project uses pthreads + sockets (Linux / WSL recommended).

Example compile command (adjust filenames if needed):


```bash
gcc -pthread leader_follower_heartbeat_2.c -o heartbeat
