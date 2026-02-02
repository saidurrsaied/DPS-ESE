# WCET bounds (from WSL measurements)

This file converts **max observed** CPU-time measurements into **analysis WCET bounds** by applying a conservative margin.

Measurement type:
- All values are from **`CLOCK_THREAD_CPUTIME_ID`** (thread CPU time).
- This measures **CPU demand** (good for EDF/RMS math), not end-to-end latency.

## Input data (max observed)
Provided measurement snapshot (Feb 2, 2026):

### Leader maxima
- L1 tick-producer iter: 426.200 µs (count 1152)
- L2 FSM per-event: 1316.800 µs (count 1182)
- L3 send one cmd: 1081.600 µs (count 1067)
- L4 rx msg->event: 133.400 µs (count 2)
- L5 accept loop iter: 479.200 µs (count 3)

### Follower maxima (worst across followers)
Taking the **worst max** across your 3 followers:
- F1 physics iter: max 733.700 µs
- F2 watchdog iter: max 350.100 µs
- F3 TCP msg->event: max 587.600 µs
- F4 UDP msg->event: max 551.900 µs
- F5 FSM per-event: max 851.100 µs

FSM per-event-type maxima (worst across followers):
- EVT_EMERGENCY: max 851.100 µs
- EVT_LEADER_TIMEOUT: max 288.800 µs
- EVT_CRUISE_CMD: max 209.300 µs
- EVT_DISTANCE: max 232.400 µs
- EVT_INTRUDER: max 387.500 µs

## Margin policy
A single run is usually enough for a **first** schedulability proof if we add conservative margins.

I recommend:
- **2× margin** for well-sampled paths (counts ~1000+)
- **3× margin** for rare paths (low counts, e.g., accept/rx, emergency/intruder)

You can change the factors later when you collect more data.

## Recommended analysis WCET bounds

### Leader (analysis WCET)
| Task | Max observed (µs) | Count | Margin | WCET bound (µs) | WCET bound (ms) |
|---|---:|---:|---:|---:|---:|
| L1 tick-producer iter | 426.200 | 1152 | 2× | 852.400 | 0.852 |
| L2 FSM per-event | 1316.800 | 1182 | 2× | 2633.600 | 2.634 |
| L3 send one cmd | 1081.600 | 1067 | 2× | 2163.200 | 2.163 |
| L4 rx msg->event | 133.400 | 2 | 3× | 400.200 | 0.400 |
| L5 accept loop iter | 479.200 | 3 | 3× | 1437.600 | 1.438 |

### Follower (analysis WCET)
Thread/task-level bounds (worst follower):

| Task | Max observed (µs) | Margin | WCET bound (µs) | WCET bound (ms) |
|---|---:|---:|---:|---:|
| F1 physics iter | 733.700 | 2× | 1467.400 | 1.467 |
| F2 watchdog iter | 350.100 | 2× | 700.200 | 0.700 |
| F3 TCP msg->event | 587.600 | 2× | 1175.200 | 1.175 |
| F4 UDP msg->event | 551.900 | 2× | 1103.800 | 1.104 |
| F5 FSM per-event | 851.100 | 2× | 1702.200 | 1.702 |

FSM per-event-type bounds (use these for event-class deadlines/priorities):

| Event type | Max observed (µs) | Count (approx) | Margin | WCET bound (µs) | WCET bound (ms) |
|---|---:|---:|---:|---:|---:|
| EVT_EMERGENCY | 851.100 | 2–3 | 3× | 2553.300 | 2.553 |
| EVT_LEADER_TIMEOUT | 288.800 | 2 | 3× | 866.400 | 0.866 |
| EVT_CRUISE_CMD | 209.300 | ~1067 | 2× | 418.600 | 0.419 |
| EVT_DISTANCE | 232.400 | ~1119 | 2× | 464.800 | 0.465 |
| EVT_INTRUDER | 387.500 | 1 | 3× | 1162.500 | 1.163 |

## Do we need more samples?

Not strictly required to proceed to EDF/RMS **with these conservative margins**, but more samples would increase confidence and/or allow smaller margins.

Where you already have strong coverage:
- Anything with counts ~1000+: L1/L2/L3, F1/F2/F3/F4 (for follower 2/3), the cruise+distance FSM events.

Where you should collect more samples (rare paths):
- Leader: L4 (count 2), L5 (count 3)
- Follower: EVT_EMERGENCY (count 2–3), EVT_INTRUDER (count 1), EVT_LEADER_TIMEOUT (count 2)

Suggested “stress scenarios” to exercise rare paths:
- Trigger emergency braking many times (press space repeatedly during a run).
- Trigger intruder events multiple times.
- Connect/disconnect followers repeatedly (to drive accept/join paths).
- Optionally run under CPU load (e.g., `stress-ng --cpu 1`) and keep the 2×/3× factors.

If you want, I can add a scripted scenario mode to `scripts/measure_wcet.sh` that injects emergency/intruder/timeout events repeatedly so those rare counters get hundreds of samples.
