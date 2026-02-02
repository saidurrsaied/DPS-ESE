#!/usr/bin/env python3
"""Compute EDF utilization + RMS response-time analysis (RTA).

This script uses the WCET bounds-with-margin we derived in
`docs/schedulability/wcet_bounds.md` and the periods/deadlines described in
`docs/schedulability/task_model.md`.

Assumptions
- Analyze leader and follower separately (each gets its own single CPU core).
- Costs are CPU demand (thread CPU time), already margin-inflated.
- Fixed-priority priorities are assigned by deadline/criticality.

Run
  python3 docs/schedulability/compute_sched.py
"""

from __future__ import annotations

from dataclasses import dataclass
from math import ceil
from typing import Iterable, List, Optional


@dataclass(frozen=True)
class Task:
    name: str
    C_ms: float
    T_ms: float
    D_ms: float
    prio: int  # smaller = higher priority


def utilization(tasks: Iterable[Task]) -> float:
    return sum(t.C_ms / t.T_ms for t in tasks)


def rta_fp(tasks: List[Task]) -> List[tuple[Task, float, bool]]:
    """Classic fixed-priority RTA for independent preemptive periodic tasks.

    R_i = C_i + sum_{j in hp(i)} ceil(R_i/T_j) * C_j
    Schedulable if R_i <= D_i for all tasks.
    """

    tasks_sorted = sorted(tasks, key=lambda t: t.prio)
    results: List[tuple[Task, float, bool]] = []

    for i, ti in enumerate(tasks_sorted):
        hp = tasks_sorted[:i]
        R = ti.C_ms

        # Iterate to fixed point (or declare unschedulable if it explodes).
        for _ in range(100):
            interference = sum(ceil(R / tj.T_ms) * tj.C_ms for tj in hp)
            R_next = ti.C_ms + interference
            if abs(R_next - R) < 1e-9:
                R = R_next
                break
            R = R_next
            if R > ti.D_ms * 10:
                break

        ok = R <= ti.D_ms
        results.append((ti, R, ok))

    return results


def print_table(tasks: List[Task], title: str) -> None:
    print(f"\n== {title} ==")
    header = f"{'Task':<24}  {'C(ms)':>8}  {'T(ms)':>8}  {'D(ms)':>8}  {'prio':>4}  {'U':>10}"
    print(header)
    print("-" * len(header))
    for t in sorted(tasks, key=lambda x: x.prio):
        u = t.C_ms / t.T_ms
        print(f"{t.name:<24}  {t.C_ms:8.3f}  {t.T_ms:8.1f}  {t.D_ms:8.1f}  {t.prio:4d}  {u:10.6f}")
    print("-" * len(header))
    print(f"Total utilization U = {utilization(tasks):.6f}")


def print_rta(tasks: List[Task], title: str) -> None:
    print(f"\n-- RMS / FP Response-Time Analysis: {title} --")
    rows = rta_fp(tasks)
    header = f"{'Task':<24}  {'R(ms)':>10}  {'D(ms)':>10}  {'OK':>3}"
    print(header)
    print("-" * len(header))
    for t, R, ok in rows:
        print(f"{t.name:<24}  {R:10.3f}  {t.D_ms:10.1f}  {'YES' if ok else 'NO':>3}")
    print("-" * len(header))
    all_ok = all(ok for _, _, ok in rows)
    print("Schedulable under fixed-priority RTA:" , "YES" if all_ok else "NO")


def print_edf(tasks: List[Task], title: str) -> None:
    print(f"\n-- EDF Utilization Test: {title} --")
    U = utilization(tasks)
    # For implicit-deadline systems (D_i == T_i), EDF schedulable iff U <= 1.
    implicit = all(abs(t.D_ms - t.T_ms) < 1e-9 for t in tasks)
    if implicit:
        print(f"Implicit deadlines detected (D=T). U={U:.6f} -> EDF schedulable iff U<=1: {'PASS' if U <= 1.0 else 'FAIL'}")
    else:
        # For constrained deadlines (D_i <= T_i), utilization <= 1 is necessary but not sufficient.
        print(f"Constrained deadlines present (some D!=T). U={U:.6f}")
        print("U<=1 is necessary (PASS if <=1), but strict proof would use demand bound function.")
        print("Utilization necessary check:", "PASS" if U <= 1.0 else "FAIL")


def leader_tasks() -> List[Task]:
    # WCET bounds-with-margin from docs/schedulability/wcet_bounds.md
    # Period/deadline from docs/schedulability/task_model.md
    T = 250.0
    D = 250.0

    return [
        Task("L_fsm", C_ms=2.634, T_ms=T, D_ms=D, prio=3),
        Task("L_send", C_ms=2.163, T_ms=T, D_ms=D, prio=4),
        Task("L_tick", C_ms=0.852, T_ms=T, D_ms=D, prio=6),
        Task("L_rx", C_ms=0.400, T_ms=T, D_ms=D, prio=5),
    ]


def follower_tasks() -> List[Task]:
    # Periods/deadlines from docs/schedulability/task_model.md
    # WCET bounds-with-margin from docs/schedulability/wcet_bounds.md
    #
    # We conservatively combine RX decode + FSM handling into a single task per message class.
    # This avoids double-counting interference across separate threads in the worst-case.
    #
    # Emergency path: UDP RX + FSM emergency handling
    C_em = 1.104 + 2.553  # F4 + EVT_EMERGENCY
    # Timeout handling: FSM timeout handling (watchdog thread just enqueues; still include it separately)
    C_watchdog = 0.700
    C_timeout_fsm = 0.866
    # Cruise path: TCP RX + FSM cruise
    C_cruise = 1.175 + 0.419
    # Distance path: UDP RX + FSM distance
    C_dist = 1.104 + 0.465
    # Physics loop
    C_phys = 1.467

    return [
        Task("F_emergency", C_ms=C_em, T_ms=50.0, D_ms=50.0, prio=1),
        Task("F_watchdog", C_ms=C_watchdog, T_ms=100.0, D_ms=100.0, prio=2),
        Task("F_timeout_fsm", C_ms=C_timeout_fsm, T_ms=100.0, D_ms=100.0, prio=2),
        Task("F_cruise", C_ms=C_cruise, T_ms=250.0, D_ms=250.0, prio=5),
        Task("F_distance", C_ms=C_dist, T_ms=250.0, D_ms=250.0, prio=5),
        Task("F_physics", C_ms=C_phys, T_ms=250.0, D_ms=250.0, prio=8),
    ]


def main() -> int:
    lt = leader_tasks()
    ft = follower_tasks()

    print_table(lt, "Leader task set")
    print_edf(lt, "Leader")
    print_rta(lt, "Leader")

    print_table(ft, "Follower task set")
    print_edf(ft, "Follower")
    print_rta(ft, "Follower")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
