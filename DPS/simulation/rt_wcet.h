#ifndef RT_WCET_H
#define RT_WCET_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Simple per-thread WCET statistics.
 *
 * We intentionally measure using CLOCK_THREAD_CPUTIME_ID so that blocking (recv/select/sleep)
 * does not inflate WCET. This matches schedulability analysis needs (CPU demand).
 */

typedef struct {
    const char* name;
    uint64_t max_ns;
    uint64_t total_ns;
    uint64_t count;
} RtWcetStat;

#define RT_WCET_STAT_INIT(stat_name) { (stat_name), 0u, 0u, 0u }

uint64_t rt_wcet_thread_time_ns(void);

static inline void rt_wcet_stat_add(RtWcetStat* stat, uint64_t delta_ns) {
    if (!stat) return;
    stat->count++;
    stat->total_ns += delta_ns;
    if (delta_ns > stat->max_ns) stat->max_ns = delta_ns;
}

void rt_wcet_stat_print(FILE* out, const RtWcetStat* stat);
void rt_wcet_stats_print_table(FILE* out, const char* title, const RtWcetStat* stats, size_t n);

#endif
