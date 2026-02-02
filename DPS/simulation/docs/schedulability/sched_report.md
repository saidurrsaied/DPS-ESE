# Schedulability report (Leader + Follower)

Date: Feb 2, 2026

This report evaluates schedulability for the truck platooning simulator using:
- **EDF utilization** (implicit-deadline test)
- **Fixed-priority (RMS-style) response-time analysis (RTA)**

## Assumptions
- Leader and each follower are analyzed **separately**, each on a **single dedicated CPU core**.
- WCET inputs are **CPU demand** measured using `CLOCK_THREAD_CPUTIME_ID` and then inflated by a safety margin (see wcet_bounds.md).
- Task periods are taken from `truckplatoon.h` values documented in task_model.md.
- For fixed-priority analysis we assign higher priority to safety-critical tasks (emergency, watchdog/timeout).

References:
- docs/schedulability/task_model.md
- docs/schedulability/wcet_bounds.md

## Task sets used for analysis

### Leader (single-core)
Periods/deadlines:
- Control tick: $T=D=250\,\text{ms}$

WCET bounds-with-margin (ms):
- L_fsm: 2.634
- L_send: 2.163
- L_rx: 0.400 (rare path, already margin-inflated)
- L_tick: 0.852

Utilization:
- $U = 0.024196$ (2.42% CPU)

RTA results (all deadlines 250 ms):
- Worst-case response time max: 6.049 ms
- Schedulable: **YES**

EDF utilization test:
- Implicit deadlines (D=T) → EDF schedulable iff $U\le 1$
- $U = 0.024196$ → **PASS**

### Follower (single-core)
Periods/deadlines:
- Emergency reaction: $T=D=50\,\text{ms}$ (project-defined safety deadline)
- Watchdog/timeout: $T=D=100\,\text{ms}$
- Cruise/distance/physics: $T=D=250\,\text{ms}$

To avoid double-counting interference across separate RX/FSM threads, we conservatively **combine** decode+enqueue and FSM handling into one per-message-class task:
- Emergency: `F4 UDP msg->event` + `F5 EVT_EMERGENCY`
- Cruise: `F3 TCP msg->event` + `F5 EVT_CRUISE_CMD`
- Distance: `F4 UDP msg->event` + `F5 EVT_DISTANCE`

WCET bounds-with-margin (ms):
- F_emergency = 1.104 + 2.553 = 3.657
- F_watchdog = 0.700
- F_timeout_fsm = 0.866
- F_cruise = 1.175 + 0.419 = 1.594
- F_distance = 1.104 + 0.465 = 1.569
- F_physics = 1.467

Utilization:
- $U = 0.107320$ (10.73% CPU)

RTA results:
- Emergency: R = 3.657 ms ≤ 50 ms
- Worst-case response time max: 9.853 ms ≤ 250 ms
- Schedulable: **YES**

EDF utilization test:
- Implicit deadlines (D=T) → EDF schedulable iff $U\le 1$
- $U = 0.107320$ → **PASS**

## How to reproduce

Run the calculator:

```bash
python3 docs/schedulability/compute_sched.py
```

## Notes / limitations
- These proofs are for **CPU schedulability** (demand) and do not model unbounded blocking in I/O.
- Arrival rates for aperiodic events are modeled conservatively via minimum inter-arrival times (e.g., emergency assumed at 50 ms).
- Measured WCET includes current logging/printing overhead; if you later change logging volume, re-measure.
