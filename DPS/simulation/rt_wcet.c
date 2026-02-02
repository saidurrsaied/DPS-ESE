#include "rt_wcet.h"

#include <inttypes.h>
#include <time.h>

uint64_t rt_wcet_thread_time_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void rt_wcet_stat_print(FILE* out, const RtWcetStat* stat) {
    if (!out) out = stderr;
    if (!stat || !stat->name) return;

    double max_us = (double)stat->max_ns / 1000.0;
    double avg_us = 0.0;
    if (stat->count > 0) {
        avg_us = ((double)stat->total_ns / (double)stat->count) / 1000.0;
    }

    fprintf(out, "%-28s count=%" PRIu64 "  max=%.3fus  avg=%.3fus\n",
            stat->name, stat->count, max_us, avg_us);
}

void rt_wcet_stats_print_table(FILE* out, const char* title, const RtWcetStat* stats, size_t n) {
    if (!out) out = stderr;
    if (title) {
        fprintf(out, "\n=== WCET (thread CPU time) — %s ===\n", title);
    } else {
        fprintf(out, "\n=== WCET (thread CPU time) ===\n");
    }
    if (!stats || n == 0) {
        fprintf(out, "(no stats)\n");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        rt_wcet_stat_print(out, &stats[i]);
    }
}
