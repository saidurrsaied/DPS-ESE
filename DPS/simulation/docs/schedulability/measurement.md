# WCET Measurement (WSL)

This repo includes per-thread CPU-time WCET counters (see `rt_wcet.c`).
The script below runs a repeatable scenario and captures logs.

## Run

From the simulation folder:

- `bash scripts/measure_wcet.sh`

This creates a timestamped directory under:

- `docs/schedulability/results/run-YYYYmmdd-HHMMSS/`

Key files inside that directory:
- `leader.log`
- `follower-6000.log`, `follower-6001.log`, `follower-6002.log`
- `meta.txt`

## What to look for

Search the logs for:
- `=== WCET (thread CPU time) — Leader ===`
- `=== WCET (thread CPU time) — Follower ===`

These tables report (per instrumented section):
- `count` (samples)
- `max` (max observed CPU time)
- `avg` (average CPU time)

## Options

Environment variables:
- `LEADER_PORT` (default: 5000)
- `FOLLOWER_PORTS` (default: "6000 6001 6002")
- `T_FORMATION`, `T_AFTER_SPEEDUP`, `T_STALE_ON`, `T_STALE_OFF` (seconds)
- `ENABLE_LOAD=1` (adds `stress-ng` CPU load if installed)
- `LOAD_CPUS` (stress-ng workers)

Example:
- `ENABLE_LOAD=1 LOAD_CPUS=1 T_STALE_ON=6 bash scripts/measure_wcet.sh`

## Notes

- WCET is measured using `CLOCK_THREAD_CPUTIME_ID` (CPU time only). Blocking in `recv()`, `select()`, or `sleep()` should not inflate the measurements.
- Followers run with stdin redirected to `/dev/null` to avoid background-job terminal issues.
