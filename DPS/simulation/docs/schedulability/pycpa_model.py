#!/usr/bin/env python3
"""pyCPA model for the Truck Platooning Simulator.

Goal
- Provide a repeatable model you can run on WSL to compute WCRT under
  fixed-priority scheduling using pyCPA.

Notes
- We analyze **leader** and **follower** as separate single-core systems
  (as per project assumption).
- This uses the WCET bounds from docs/schedulability/wcet_bounds.md.
- If pyCPA is not installed, this script prints a fallback utilization
  summary so you still get a sanity check.

Run
  python3 docs/schedulability/pycpa_model.py --system follower
  python3 docs/schedulability/pycpa_model.py --system leader

Setup (Ubuntu/WSL)
  sudo apt update
  sudo apt install -y python3-pip python3-venv
  python3 -m venv .venv
  source .venv/bin/activate
  pip install -r docs/schedulability/requirements.txt
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from typing import List, Optional


@dataclass(frozen=True)
class TaskSpec:
    name: str
    wcet_ms: float
    period_ms: float
    deadline_ms: Optional[float] = None
    priority: int = 10  # smaller is higher priority (typical convention)


def follower_taskset() -> List[TaskSpec]:
    # Periods from docs/schedulability/task_model.md
    # FOLLOWER_PHYS_DT = 250ms, watchdog period = 100ms.
    #
    # Costs: derived from docs/schedulability/wcet_bounds.md (already with margin).
    return [
        TaskSpec(name="F_emergency_udp+fsm", wcet_ms=3.657, period_ms=50.0, deadline_ms=50.0, priority=1),
        TaskSpec(name="F_watchdog_check", wcet_ms=0.700, period_ms=100.0, deadline_ms=100.0, priority=2),
        # Model timeout handling pessimistically as periodic (safe, pessimistic).
        TaskSpec(name="F_timeout_fsm", wcet_ms=0.866, period_ms=100.0, deadline_ms=100.0, priority=2),
        # Cruise command path: TCP decode + FSM cruise.
        TaskSpec(name="F_cruise_tcp+fsm", wcet_ms=1.175 + 0.419, period_ms=250.0, deadline_ms=250.0, priority=5),
        # Distance update path: UDP decode + FSM distance.
        TaskSpec(name="F_distance_udp+fsm", wcet_ms=1.104 + 0.465, period_ms=250.0, deadline_ms=250.0, priority=5),
        # Periodic physics loop.
        TaskSpec(name="F_physics_iter", wcet_ms=1.467, period_ms=250.0, deadline_ms=250.0, priority=8),
    ]


def leader_taskset() -> List[TaskSpec]:
    # Period from docs/schedulability/task_model.md
    # LEADER_TICK_DT = 250ms.
    return [
        TaskSpec(name="L_tick_producer", wcet_ms=0.852, period_ms=250.0, deadline_ms=250.0, priority=8),
        TaskSpec(name="L_fsm_per_event", wcet_ms=2.634, period_ms=250.0, deadline_ms=250.0, priority=5),
        TaskSpec(name="L_send_one_cmd", wcet_ms=2.163, period_ms=250.0, deadline_ms=250.0, priority=6),
        # Rare in steady-state; include as a safety check anyway.
        TaskSpec(name="L_rx_msg_to_event", wcet_ms=0.400, period_ms=250.0, deadline_ms=250.0, priority=7),
    ]


def utilization(taskset: List[TaskSpec]) -> float:
    return sum(t.wcet_ms / t.period_ms for t in taskset)


def print_taskset(taskset: List[TaskSpec]) -> None:
    header = f"{'name':<24}  {'C(ms)':>8}  {'T(ms)':>8}  {'D(ms)':>8}  {'prio':>4}  {'U':>10}"
    print(header)
    print("-" * len(header))
    for t in taskset:
        d = t.deadline_ms if t.deadline_ms is not None else t.period_ms
        u = t.wcet_ms / t.period_ms
        print(
            f"{t.name:<24}  {t.wcet_ms:8.3f}  {t.period_ms:8.1f}  {d:8.1f}  {t.priority:4d}  {u:10.6f}"
        )
    print("-" * len(header))
    print(f"Total utilization U={utilization(taskset):.6f}")


def run_pycpa_fp(taskset: List[TaskSpec], system_name: str) -> int:
    try:
        from pycpa import model, analysis  # type: ignore
    except Exception as e:
        print("pyCPA is not available in this Python environment.")
        print("Reason:", str(e))
        print("\nFallback (non-pyCPA) summary:")
        print_taskset(taskset)
        print("\nTo install on Ubuntu/WSL:")
        print("  sudo apt update && sudo apt install -y python3-pip python3-venv")
        print("  python3 -m venv .venv && source .venv/bin/activate")
        print("  pip install -r docs/schedulability/requirements.txt")
        return 2

    # Try to be resilient against small API differences between pyCPA versions.
    sys = model.System(system_name)

    # Scheduler: most pyCPA versions support fixed-priority (SPP).
    # Some versions store schedulers under analysis or a separate module.
    scheduler = None
    for mod in (analysis, model):
        for attr in ("SPPScheduler", "SPPSchedulerFP", "FPPScheduler", "FixedPriorityScheduler"):
            if hasattr(mod, attr):
                scheduler = getattr(mod, attr)()
                break
        if scheduler is not None:
            break

    if scheduler is None:
        # Default scheduler (will still allow analysis, but might not be FP).
        scheduler = None

    if scheduler is None:
        res = model.Resource("cpu0")
    else:
        res = model.Resource("cpu0", scheduler=scheduler)

    sys.resources.add(res)

    # Event model: periodic with jitter=0 (PJd) if available.
    def make_event_model(period_ms: float):
        period = period_ms / 1000.0
        for cls_name in ("PJdEventModel", "PJD"):  # common spellings
            if hasattr(model, cls_name):
                cls = getattr(model, cls_name)
                try:
                    return cls(P=period, J=0)
                except TypeError:
                    return cls(P=period)
        # Fall back to plain periodic event model name.
        if hasattr(model, "PeriodicEventModel"):
            return model.PeriodicEventModel(P=period)
        raise RuntimeError("No supported periodic event model found in pyCPA")

    # Create tasks
    tasks = []
    for spec in taskset:
        # pyCPA uses seconds for bcet/wcet in many versions.
        wcet_s = spec.wcet_ms / 1000.0
        bcet_s = wcet_s  # no BCET measurement; set equal for now

        try:
            t = model.Task(spec.name, res, wcet=wcet_s, bcet=bcet_s, scheduling_parameter=spec.priority)
        except TypeError:
            # Some versions use positional args
            t = model.Task(spec.name, res, bcet_s, wcet_s, spec.priority)

        t.in_event_model = make_event_model(spec.period_ms)
        tasks.append(t)
        sys.tasks.add(t)

    # Run analysis
    results = analysis.analyze_system(sys)

    print_taskset(taskset)
    print("\npyCPA results (if available):")

    # Best-effort extraction of WCRT from results/tasks.
    for t in tasks:
        wcrt = None
        bcrt = None

        # Common patterns:
        # - results[t]["wcrt"]
        # - results[t].wcrt
        # - t.wcrt
        if isinstance(results, dict) and t in results:
            r = results[t]
            if isinstance(r, dict):
                wcrt = r.get("wcrt")
                bcrt = r.get("bcrt")
            else:
                wcrt = getattr(r, "wcrt", None)
                bcrt = getattr(r, "bcrt", None)

        if wcrt is None:
            wcrt = getattr(t, "wcrt", None)
        if bcrt is None:
            bcrt = getattr(t, "bcrt", None)

        def fmt_s(x):
            if x is None:
                return "n/a"
            try:
                return f"{float(x) * 1000.0:.3f} ms"
            except Exception:
                return str(x)

        print(f"- {t.name}: WCRT={fmt_s(wcrt)} BCRT={fmt_s(bcrt)}")

    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--system", choices=["leader", "follower"], required=True)
    args = ap.parse_args()

    if args.system == "leader":
        ts = leader_taskset()
        return run_pycpa_fp(ts, system_name="leader")

    ts = follower_taskset()
    return run_pycpa_fp(ts, system_name="follower")


if __name__ == "__main__":
    raise SystemExit(main())
