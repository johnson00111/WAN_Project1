#ifndef CS2520_STATS
#define CS2520_STATS

#include <stdint.h>
#include <sys/time.h>

/* Shared by all four programs so one harness parses both the UDP and the TCP
 * runs. Signatures frozen 2026-09-09; jaa554 owns the implementation, so tell
 * her before touching them.
 *
 * [STAT] label=rcv bytes=10485760 mb=10 interval_mbps=94.31 avg_mbps=93.88 elapsed_s=0.894
 * [FINAL] label=rcv bytes=104857600 raw=106168832 elapsed_s=9.412 avg_mbps=89.12 overhead=1.0125
 */

#define STATS_MARK_BYTES (10 * 1024 * 1024)

typedef struct {
    const char    *label;        /* ncp / rcv / t_ncp / t_rcv */
    uint64_t       total_bytes;  /* file size, 0 when unknown (t_rcv) */
    uint64_t       bytes;        /* so far; for rcv this is the in-order count */
    uint64_t       bytes_raw;    /* incl. headers and retransmissions, 0 if unused */
    struct timeval t_start;
    struct timeval t_mark;       /* last 10 MB report */
    uint64_t       next_mark;    /* next threshold */
} stats_t;

/* Starts the clock. For ncp/rcv that means the first FEEDBACK, not startup --
 * time spent BLOCKED or PROBING doesn't count. */
void stats_start(stats_t *s, const char *label, uint64_t total_bytes);

/* One packet's progress. Prints [STAT] itself when a 10 MB mark is crossed,
 * so callers have one place to touch. */
void stats_add(stats_t *s, uint64_t payload_bytes, uint64_t raw_bytes);

void stats_final(stats_t *s);

#endif
