#!/usr/bin/env bash
set -euo pipefail

# Measure WCET (thread CPU time) for leader+followers.
# Designed for WSL/Linux; avoids background-job SIGTTIN by redirecting follower stdin.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LEADER_PORT="${LEADER_PORT:-5000}"
FOLLOWER_PORTS=(${FOLLOWER_PORTS:-6000 6001 6002})

# Timings (seconds)
T_FORMATION="${T_FORMATION:-3}"          # wait for followers to connect and topology finalize
T_AFTER_SPEEDUP="${T_AFTER_SPEEDUP:-3}"  # let system run after speedup
T_STALE_ON="${T_STALE_ON:-4}"            # stale ON duration
T_STALE_OFF="${T_STALE_OFF:-4}"          # stale OFF duration

# Optional background CPU load
ENABLE_LOAD="${ENABLE_LOAD:-0}"
LOAD_CPUS="${LOAD_CPUS:-1}"

RUN_DIR="$ROOT_DIR/docs/schedulability/results/run-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN_DIR"

log() { printf '[measure_wcet] %s\n' "$*"; }

# Use taskset if available (pin to CPU0). If not, run unpinned.
PIN_CMD=()
if command -v taskset >/dev/null 2>&1; then
  PIN_CMD=(taskset -c 0)
  log "Pinning processes to CPU core 0 (taskset)"
else
  log "taskset not found; running unpinned"
fi

# Clean up any background processes we start.
LEADER_PID=""
FOLLOWER_PIDS=()
LOAD_PID=""
FIFO="$RUN_DIR/leader.stdin.fifo"

cleanup() {
  set +e
  if [[ -n "${LEADER_PID}" ]] && kill -0 "${LEADER_PID}" 2>/dev/null; then
    kill -INT "${LEADER_PID}" 2>/dev/null || true
    sleep 0.5
    kill -KILL "${LEADER_PID}" 2>/dev/null || true
  fi
  for p in "${FOLLOWER_PIDS[@]:-}"; do
    if kill -0 "$p" 2>/dev/null; then
      kill -INT "$p" 2>/dev/null || true
      sleep 0.2
      kill -KILL "$p" 2>/dev/null || true
    fi
  done
  if [[ -n "${LOAD_PID}" ]] && kill -0 "${LOAD_PID}" 2>/dev/null; then
    kill -TERM "${LOAD_PID}" 2>/dev/null || true
  fi
  rm -f "$FIFO" 2>/dev/null || true
}
trap cleanup EXIT

log "Writing logs to: $RUN_DIR"
{
  echo "date: $(date -Is)"
  echo "uname: $(uname -a)"
  echo "wsl: ${WSL_DISTRO_NAME:-no}"
  echo "leader_port: $LEADER_PORT"
  echo "follower_ports: ${FOLLOWER_PORTS[*]}"
  echo "pin_cmd: ${PIN_CMD[*]:-(none)}"
  echo "timings: formation=$T_FORMATION after_speedup=$T_AFTER_SPEEDUP stale_on=$T_STALE_ON stale_off=$T_STALE_OFF"
} > "$RUN_DIR/meta.txt"

# Optional stress
if [[ "$ENABLE_LOAD" == "1" ]] && command -v stress-ng >/dev/null 2>&1; then
  log "Starting stress-ng CPU load ($LOAD_CPUS workers)"
  "${PIN_CMD[@]}" stress-ng --cpu "$LOAD_CPUS" --timeout $((T_FORMATION + T_AFTER_SPEEDUP + T_STALE_ON + T_STALE_OFF + 5))s \
    > "$RUN_DIR/stress-ng.log" 2>&1 &
  LOAD_PID=$!
elif [[ "$ENABLE_LOAD" == "1" ]]; then
  log "ENABLE_LOAD=1 but stress-ng not found; skipping load"
fi

# Create FIFO to feed leader keyboard input deterministically.
mkfifo "$FIFO"

log "Starting leader on port $LEADER_PORT"
"${PIN_CMD[@]}" stdbuf -oL -eL ./leader "$LEADER_PORT" < "$FIFO" > "$RUN_DIR/leader.log" 2>&1 &
LEADER_PID=$!

# Give the leader a moment to bind() + listen() before followers connect.
sleep 0.5

# Start followers (stdin redirected to avoid SIGTTIN when run in background).
for port in "${FOLLOWER_PORTS[@]}"; do
  log "Starting follower on UDP port $port"
  "${PIN_CMD[@]}" stdbuf -oL -eL ./follower "$port" < /dev/null > "$RUN_DIR/follower-$port.log" 2>&1 &
  FOLLOWER_PIDS+=("$!")
  sleep 0.3
done

# Feed leader input.
# - speed up a bit
# - toggle stale ON, then OFF
# - quit
(
  sleep "$T_FORMATION"
  printf 'wwwwww' > "$FIFO"
  sleep "$T_AFTER_SPEEDUP"
  printf 'p' > "$FIFO"
  sleep "$T_STALE_ON"
  printf 'p' > "$FIFO"
  sleep "$T_STALE_OFF"
  printf 'q' > "$FIFO"
) &

# If formation never completes, leader ignores 'q'. Enforce an upper bound on runtime.
MAX_RUNTIME=$((T_FORMATION + T_AFTER_SPEEDUP + T_STALE_ON + T_STALE_OFF + 8))
( sleep "$MAX_RUNTIME"; if [[ -n "${LEADER_PID}" ]] && kill -0 "${LEADER_PID}" 2>/dev/null; then kill -INT "${LEADER_PID}" 2>/dev/null || true; fi ) &

log "Waiting for leader to exit (it will print WCET table at shutdown)"
wait "$LEADER_PID" || true

# Followers should self-terminate when TCP closes; give them a moment.
sleep 1.0

log "Done. Key outputs: leader.log and follower-*.log (search for '=== WCET')"
