#include <stdio.h>
#include <inttypes.h>

#include "stats.h"

/* Implemented against the interface frozen 2026-09-09 (team handbook 3.3).
 * jaa554 owns this module; this is a working version so the protocol work and
 * the measurements are not blocked. Replacing the file needs no change at any
 * call site.
 *
 * Mbps is decimal megabits per second, to match how the 100 Mbps link is
 * specified. Sizes are binary: a "10 MB" mark is 10 MiB.
 */

static double Seconds(const struct timeval *from, const struct timeval *to)
{
    return (double)(to->tv_sec - from->tv_sec)
         + (double)(to->tv_usec - from->tv_usec) / 1000000.0;
}

static double Mbps(uint64_t bytes, double seconds)
{
    if (seconds <= 0.0) {
        return 0.0;
    }
    return (double)bytes * 8.0 / seconds / 1000000.0;
}

void stats_start(stats_t *s, const char *label, uint64_t total_bytes)
{
    s->label       = label;
    s->total_bytes = total_bytes;
    s->bytes       = 0;
    s->bytes_raw   = 0;
    s->next_mark   = STATS_MARK_BYTES;

    gettimeofday(&s->t_start, NULL);
    s->t_mark = s->t_start;
}

void stats_add(stats_t *s, uint64_t payload_bytes, uint64_t raw_bytes)
{
    s->bytes     += payload_bytes;
    s->bytes_raw += raw_bytes;

    /* A loop rather than an if: one call can carry several marks' worth if a
     * large contiguous run is written at once. */
    while (s->bytes >= s->next_mark) {
        struct timeval now;
        double         elapsed;
        double         interval;

        gettimeofday(&now, NULL);
        elapsed  = Seconds(&s->t_start, &now);
        interval = Seconds(&s->t_mark, &now);

        /* Report the threshold, not the overshoot, so the marks land on round
         * numbers and successive intervals stay comparable. */
        printf("[STAT] label=%s bytes=%" PRIu64 " mb=%" PRIu64
               " interval_mbps=%.2f avg_mbps=%.2f elapsed_s=%.3f\n",
               s->label, s->next_mark, s->next_mark / (1024 * 1024),
               Mbps(STATS_MARK_BYTES, interval), Mbps(s->next_mark, elapsed),
               elapsed);

        s->t_mark      = now;
        s->next_mark  += STATS_MARK_BYTES;
    }
}

void stats_final(stats_t *s)
{
    struct timeval now;
    double         elapsed;
    double         overhead;

    gettimeofday(&now, NULL);
    elapsed  = Seconds(&s->t_start, &now);
    overhead = (s->bytes > 0) ? (double)s->bytes_raw / (double)s->bytes : 0.0;

    printf("[FINAL] label=%s bytes=%" PRIu64 " raw=%" PRIu64
           " elapsed_s=%.3f avg_mbps=%.2f overhead=%.4f\n",
           s->label, s->bytes, s->bytes_raw, elapsed,
           Mbps(s->bytes, elapsed), overhead);
}
