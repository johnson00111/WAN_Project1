#ifndef CS2520_NET_INCLUDE
#define CS2520_NET_INCLUDE

#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> 
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_MESS_LEN 1400                       /* whole datagram, stays under the MTU */
#define HDR_LEN      16
#define PAYLOAD      (MAX_MESS_LEN - HDR_LEN)   /* 1384 */

#define MODE_LAN 1
#define MODE_WAN 2

#define MSG_DATA     1
#define MSG_FEEDBACK 2
#define MSG_BUSY     3

/* Receiver's reorder ring, 4096 * 1384 = 5.7 MB.
 *
 * Indexed seq % W_MAX, so W has to stay below it. A wider window puts two
 * live sequences on one slot: the later overwrites the earlier, have[] still
 * says "present", and the file comes out corrupt with no error. Only diff
 * notices. Raise this and recompile if tuning ever wants W > 4095. */
#define W_MAX 4096

_Static_assert(W_MAX >= 2, "W_MAX too small");

/* Wire format. One header for all three types, body read per type.
 *
 *   type(1) reserved(1) body_len(2) session_id(4) seq(4) extra(4)
 *
 *              seq                      extra
 *   DATA       packet index, 0 = meta   N, the last sequence
 *   FEEDBACK   ARU                      bitmap length in bits
 *   BUSY       0                        0
 *
 * Host byte order, no htonl(). Both ends run the same container image, so
 * they always agree. Deliberate -- not portable across architectures. */
#pragma pack(push, 1)
typedef struct dummy_pkt_hdr {
    uint8_t  type;
    uint8_t  reserved;    /* keeps the three 4-byte fields aligned */
    uint16_t body_len;    /* bytes after this header */
    uint32_t session_id;  /* sender picks it, receiver echoes it */
    uint32_t seq;
    uint32_t extra;
} pkt_hdr;
#pragma pack(pop)

_Static_assert(sizeof(pkt_hdr) == HDR_LEN, "header must be exactly 16 bytes on the wire");

/* seq 0 = metadata (uint64 file_size, uint32 N, NUL-terminated filename).
 * seq s >= 1 = file bytes at (s-1) * PAYLOAD, only the last one short. */
typedef struct dummy_ncp_msg {
    pkt_hdr hdr;
    char    payload[PAYLOAD];
} ncp_msg;

/* FEEDBACK payload = gap bitmap from ARU+1; bit i set means ARU+1+i arrived,
 * so bit 0 is always 0. BUSY payload empty. */
typedef struct dummy_rcv_msg {
    pkt_hdr hdr;
    char    payload[PAYLOAD];
} rcv_msg;

_Static_assert(sizeof(ncp_msg) == MAX_MESS_LEN, "ncp_msg over the datagram budget");
_Static_assert(sizeof(rcv_msg) == MAX_MESS_LEN, "rcv_msg over the datagram budget");

#define BITMAP_MAX_BITS (PAYLOAD * 8)   /* 11072. Clamp to it; don't trust high_seq. */

/* ms clock for every timer. Wraps every ~49 days; only differences matter. */
static inline uint32_t now_ms(void)
{
    struct timeval t;

    gettimeofday(&t, NULL);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_usec / 1000);
}

/* select() timeout from a ms value. */
static inline struct timeval ms_to_tv(int ms)
{
    struct timeval t;

    t.tv_sec  = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
    return t;
}

/* One set per environment, picked by the LAN/WAN argument, so reproducing
 * either set of results needs no code change. Derivation is in the design
 * doc; short version: WAN BDP is 361 packets and W starts around 4x that.
 * Everything in ms. */
typedef struct dummy_params {
    int W;                   /* sender:   packets in flight */
    int fb_period_ms;        /* receiver: feedback heartbeat */
    int nack_min_ms;         /* receiver: floor between immediate gap reports */
    int session_timeout_ms;  /* receiver: sender presumed dead */
    int linger_ms;           /* receiver: answer strays after completion */
    int timeout_ms;          /* sender:   no feedback -> probe */
    int retx_suppress_ms;    /* sender:   min gap between resends of one seq */
    int busy_retry_ms;       /* sender:   retry interval while BLOCKED */
    int give_up_ms;          /* sender:   total silence -> quit */
} params;

/*                      W    fb  nack  sess  ling   to  retx  busy  give_up */
#define PARAMS_LAN {  256,    2,    1, 10000, 2000,  20,    1, 1000, 10000 }
#define PARAMS_WAN { 1500,   10,    1, 10000, 2000, 100,   40, 1000, 10000 }

/* Also the one choke point every window value passes through, so the W_MAX
 * check lives here rather than at each call site. */
static inline const params *params_for(int mode)
{
    static const params lan = PARAMS_LAN;
    static const params wan = PARAMS_WAN;
    const params *p;

    p = (mode == MODE_WAN) ? &wan : &lan;

    if (p->W >= W_MAX) {
        fprintf(stderr, "FATAL: W=%d >= W_MAX=%d. The receiver's ring would alias "
                        "and silently corrupt the file.\nRaise W_MAX in "
                        "net_include.h and recompile, or lower W.\n", p->W, W_MAX);
        exit(1);
    }
    if (p->W < 1) {
        fprintf(stderr, "FATAL: W=%d must be at least 1\n", p->W);
        exit(1);
    }
    return p;
}

#endif
