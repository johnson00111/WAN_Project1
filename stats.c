#include "stats.h"

/* Stub until jaa554's real module lands (T2.1). Frozen signatures with no-op
 * bodies, so the protocol can call them now and nothing changes later.
 * Nothing prints until then. The (void) casts just silence -Wall. */

void stats_start(stats_t *s, const char *label, uint64_t total_bytes)
{
    (void)s; (void)label; (void)total_bytes;
}

void stats_add(stats_t *s, uint64_t payload_bytes, uint64_t raw_bytes)
{
    (void)s; (void)payload_bytes; (void)raw_bytes;
}

void stats_final(stats_t *s)
{
    (void)s;
}
