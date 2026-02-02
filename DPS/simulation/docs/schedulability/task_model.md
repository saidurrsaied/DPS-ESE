# Task Model (Leader + Follower)

Assumptions (as agreed):
- Analyze **leader** and **follower** separately.
- Each runs on a **single dedicated CPU core** (no CPU sharing between leader and follower).
- Preemptive scheduling is assumed (Linux-like). We will show **EDF** and **fixed-priority** (RMS/RTA) feasibility.
- No professor-provided deadlines: we define reasonable deadlines based on the simulator’s own control rates.

## Source of periods
- Leader tick period: `LEADER_TICK_DT` in `truckplatoon.h` (currently 0.25 s).
- Follower physics period: `FOLLOWER_PHYS_DT` in `truckplatoon.h` (currently 0.25 s).
- Watchdog check period: `LEADER_WATCHDOG_PERIOD_MS` in `truckplatoon.h` (currently 100 ms).

---

## Follower task set (per-process)

Threads/tasks observed in `follower.c`:
- Main thread: periodic physics loop
- `udp_listener`: blocking `recvfrom()` + enqueue event
- `tcp_listener`: blocking `recv()` + enqueue event(s)
- `truck_state_machine`: event consumer / control logic
- `leader_rx_watchdog`: periodic liveness check + enqueue timeout event
- `keyboard_listener`: operator input (ignored for schedulability unless required)

We model each long-running thread as a schedulable task.

### F1 — Physics + Status + Rear broadcast (periodic)
- **Implementation**: follower main loop
- **Type**: periodic
- **Period**: $T=\texttt{FOLLOWER_PHYS_DT}$ (250 ms)
- **Deadline**: $D=T$ (implicit deadline)
- **Work (what consumes CPU)**:
  - integrate kinematics (`move_truck`)
  - turn-queue check
  - UDP send to rear (`send_position_to_rear`)
  - status print (optional, but include in WCET if enabled)

### F2 — Watchdog (periodic)
- **Implementation**: `leader_rx_watchdog`
- **Type**: periodic
- **Period**: $T=\texttt{LEADER_WATCHDOG_PERIOD_MS}$ (100 ms)
- **Deadline**: $D=T$
- **Work**:
  - reads last leader-rx timestamp
  - may enqueue `EVT_LEADER_TIMEOUT`

### F3 — TCP RX (sporadic)
- **Implementation**: `tcp_listener`
- **Type**: sporadic
- **Min inter-arrival** (assumption):
  - cruise commands are produced by leader at most once per leader tick
  - so we assume $T=\texttt{LEADER_TICK_DT}$ (250 ms) for `MSG_LDR_CMD` arrivals
  - plus low-rate topology/id/spawn messages (bounded by join events)
- **Deadline**: $D=T$ for cruise messages
- **Work**:
  - decode message
  - update liveness timestamp
  - enqueue event(s)

### F4 — UDP RX (sporadic)
- **Implementation**: `udp_listener`
- **Type**: sporadic
- **Min inter-arrival** (assumptions):
  - position updates from front truck: at most once per `FOLLOWER_PHYS_DT` (250 ms)
  - emergency messages: rare; assume a conservative minimum inter-arrival $T=50$ ms for analysis
- **Deadlines**:
  - position updates: $D=250$ ms
  - emergency brake: **define** $D=50$ ms (safety-critical, must react faster than control tick)

### F5 — FSM / Control (event-driven)
- **Implementation**: `truck_state_machine`
- **Type**: sporadic server (runs when events are available)
- **Arrival rate**: sum of event sources (TCP/UDP/watchdog/keyboard)
- **Deadlines (by event class)**:
  - cruise cmd (`EVT_CRUISE_CMD`): $D=250$ ms
  - distance update (`EVT_DISTANCE`): $D=250$ ms
  - emergency (`EVT_EMERGENCY`): $D=50$ ms
  - leader timeout (`EVT_LEADER_TIMEOUT`): $D=100$ ms

Notes:
- For conservative schedulability, we will measure and bound **WCET per event type** in the FSM.
- For fixed-priority analysis, emergency-related work should be assigned the highest priority.

---

## Leader task set (per-process)

Threads/tasks observed in `leader.c`:
- Main thread: periodic tick producer
- `leader_state_machine`: event consumer (single writer)
- `accept_handler`: accept TCP connections
- `send_handler`: broadcast commands to followers
- `follower_message_receiver`: receive follower messages (intruder reports, etc.)
- `input_handler`: keyboard input

### L1 — Tick producer (periodic)
- **Implementation**: leader main loop pushes `EVT_TICK_UPDATE`
- **Type**: periodic
- **Period**: $T=\texttt{LEADER_TICK_DT}$ (250 ms)
- **Deadline**: $D=T$
- **Work**:
  - enqueue tick event

### L2 — Leader FSM (event-driven)
- **Implementation**: `leader_state_machine`
- **Type**: sporadic server
- **Arrival sources**:
  - ticks (periodic)
  - input events (sporadic)
  - follower messages (sporadic)
- **Deadlines**:
  - tick processing: $D=250$ ms
  - emergency reaction (space key / forwarded emergency): $D=50$ ms

### L3 — Sender (bounded periodic)
- **Implementation**: `send_handler`
- **Type**: periodic-ish, driven by queued commands
- **Rate assumption**:
  - leader generates at most one cruise command per tick
  - so model as periodic $T=250$ ms
- **Deadline**: $D=250$ ms
- **Work**:
  - serialize/broadcast to up to `MAX_FOLLOWERS`

### L4 — Receiver (sporadic)
- **Implementation**: `follower_message_receiver`
- **Type**: sporadic
- **Min inter-arrival**: assume $T=250$ ms per follower (conservative)
- **Deadline**: $D=250$ ms

### L5 — Acceptor (sporadic)
- **Implementation**: `accept_handler`
- **Type**: sporadic / aperiodic
- **Not typically in steady-state schedulability**; include for “formation phase” analysis if required.

---

## What we will measure next (WCET candidates)

We will measure CPU-time (not wall-time) of:
- F1: one physics iteration
- F2: one watchdog iteration
- F3/F4: per-message decode+enqueue
- F5: per-event handling time (by event type)
- L1: tick enqueue
- L2: per-event handling time (tick, input, follower msg)
- L3: per-broadcast send (cost grows with number of followers)

We’ll then apply a conservative margin factor (e.g., 2×) to turn “max observed” into an analysis WCET.
