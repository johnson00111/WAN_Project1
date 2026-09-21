#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <arpa/inet.h>

#include "sendto_dbg.h"
#include "net_include.h"
#include "stats.h"

/* Receiver states. LINGER arrives with termination in a later stage.
 *
 *   IDLE --seq 0 arrives--> RECEIVING --sender goes quiet--> IDLE
 */
#define ST_IDLE      1
#define ST_RECEIVING 2
#define ST_LINGER    3

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static void Init_socket(void);
static void Handle_packet(const ncp_msg *m, int len, const struct sockaddr_in *from);
static void Adopt_session(const ncp_msg *m, const struct sockaddr_in *from);
static void Send_feedback(void);
static void Send_busy(uint32_t session, const struct sockaddr_in *to);
static void Store_packet(const ncp_msg *m);
static void Advance_aru(void);
static void Complete_session(void);
static void Abort_session(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;
static const params *Params;

/* Session state */
static int                Sock;
static int                State = ST_IDLE;
static uint32_t           Cur_session;
static struct sockaddr_in Cur_addr;
static uint32_t           Aru;
static uint32_t           N;
static uint32_t           Last_data_ms;
static uint32_t           Last_fb_ms;
static uint32_t           Linger_start_ms;
static stats_t            Stats;

/* Reorder ring. Indexed seq % W_MAX -- see the W_MAX comment in net_include.h
 * for why the window has to stay below it. 4096 * 1384 = 5.7 MB, fixed, so
 * tuning W never means resizing anything. */
static char     Slot[W_MAX][PAYLOAD];
static uint16_t Slot_len[W_MAX];
static char     Have[W_MAX];

static FILE     *Fp;
static char      Dst_name[PAYLOAD];
static uint64_t  Bytes_written;
static int       Done_valid;         /* Cur_session finished; keep answering it */
static int       Stray_reported;     /* one line per idle spell, not per packet */

int main(int argc, char *argv[]) {
    /* Line-buffered: progress has to show up promptly when stdout is a pipe
     * or a log file, not just on a terminal. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }

    /* Also enforces W < W_MAX -- see net_include.h. */
    Params = params_for(Mode);
    printf("\tWire format: header %d B + payload %d B = datagram %d B\n",
           (int)sizeof(pkt_hdr), PAYLOAD, (int)sizeof(rcv_msg));
    printf("\tRing buffer: W_MAX = %d slots (%.1f MB), bitmap capacity = %d bits\n",
           W_MAX, (double)W_MAX * PAYLOAD / 1000000.0, BITMAP_MAX_BITS);
    printf("\tReceiver params: FB_PERIOD = %d ms, NACK_MIN = %d ms,\n"
           "\t                 SESSION_TIMEOUT = %d ms, LINGER_TIME = %d ms\n",
           Params->fb_period_ms, Params->nack_min_ms,
           Params->session_timeout_ms, Params->linger_ms);

    Init_socket();
    printf("[rcv] IDLE, waiting on port %s\n", Port_Str);

    for (;;) {
        fd_set         mask;
        struct timeval tv;
        int            ready;

        FD_ZERO(&mask);
        FD_SET(Sock, &mask);
        tv = ms_to_tv(Params->fb_period_ms);

        ready = select(Sock + 1, &mask, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("rcv: select");
            exit(1);
        }

        if (ready > 0 && FD_ISSET(Sock, &mask)) {
            ncp_msg            m;
            struct sockaddr_in from;
            socklen_t          from_len = sizeof(from);
            ssize_t            got;

            got = recvfrom(Sock, &m, sizeof(m), 0,
                           (struct sockaddr *)&from, &from_len);
            if (got >= (ssize_t)HDR_LEN) {
                Handle_packet(&m, (int)got, &from);
            }
        }

        /* Timers are checked every iteration, not just when select() times
         * out: while data is pouring in select() always returns ready and the
         * timeout branch would never run. */
        if (State == ST_RECEIVING) {
            if (now_ms() - Last_fb_ms >= (uint32_t)Params->fb_period_ms) {
                Send_feedback();
            }
            /* A sender that dies mid-transfer must not pin the receiver. */
            if (now_ms() - Last_data_ms > (uint32_t)Params->session_timeout_ms) {
                Abort_session();
            }
        } else if (State == ST_LINGER) {
            if (now_ms() - Linger_start_ms > (uint32_t)Params->linger_ms) {
                State = ST_IDLE;
                Stray_reported = 0;
                printf("[rcv] LINGER over, IDLE\n");
            }
        }
    }
}

static void Handle_packet(const ncp_msg *m, int len, const struct sockaddr_in *from) {
    /* Deliberately not one line per packet: a 100 MB transfer is 72,255 of
     * them, and the printing alone would dominate the measurement. Only the
     * events worth seeing get a line. */
    if (m->hdr.seq == 0) {
        printf("[rcv] from %s:%d  type=%" PRIu8 " seq=%" PRIu32 " extra=%" PRIu32
               " body_len=%" PRIu16 " (%d B)\n",
               inet_ntoa(from->sin_addr), ntohs(from->sin_port),
               m->hdr.type, m->hdr.seq, m->hdr.extra, m->hdr.body_len, len);
    }

    if (m->hdr.type != MSG_DATA) {
        return;                          /* receivers never see FEEDBACK or BUSY */
    }

    /* body_len is 16 bits and comes off the wire, so it can claim far more than
     * actually arrived. Trust the datagram length, not the sender. */
    if (m->hdr.body_len > len - HDR_LEN) {
        printf("[rcv]   ignored: body_len %" PRIu16 " exceeds the %d B received\n",
               m->hdr.body_len, len);
        return;
    }

    /* A sender whose final ACK was lost keeps asking. Answering it costs one
     * packet and saves it from hanging; this is what LINGER is for, and it
     * outlives LINGER so the sender is never stranded. */
    if (Done_valid && m->hdr.session_id == Cur_session) {
        Send_feedback();
        return;
    }

    if (State == ST_IDLE) {
        if (m->hdr.seq != 0) {
            /* Only seq 0 carries the filename, so nothing else is usable here.
             * Almost always a leftover from an abandoned transfer, and it
             * arrives in floods, so say it once per idle spell. */
            if (!Stray_reported) {
                printf("[rcv]   ignoring stray data: IDLE, and only seq 0 is usable\n");
                Stray_reported = 1;
            }
            return;
        }
        Adopt_session(m, from);
        return;
    }

    if (m->hdr.session_id != Cur_session) {
        printf("[rcv]   busy: session %" PRIu32 " is in progress\n", Cur_session);
        Send_busy(m->hdr.session_id, from);
        return;
    }

    Last_data_ms = now_ms();

    if (m->hdr.seq == 0) {
        Send_feedback();                 /* duplicate metadata, just re-ack */
        return;
    }

    Store_packet(m);
    Advance_aru();

    if (Aru >= N) {
        Complete_session();
    }
}

static void Adopt_session(const ncp_msg *m, const struct sockaddr_in *from) {
    uint64_t file_size;
    char     name[PAYLOAD];

    if (m->hdr.body_len < 13) {
        printf("[rcv]   ignored: seq 0 metadata too short\n");
        return;
    }

    memcpy(&file_size, m->payload, sizeof(file_size));
    memcpy(&N, m->payload + 8, sizeof(N));

    /* The filename is NUL-terminated inside the payload, but a corrupt or
     * hostile packet need not be, so terminate it ourselves. */
    memcpy(name, m->payload + 12, m->hdr.body_len - 12);
    name[m->hdr.body_len - 12 - 1] = '\0';

    Cur_session  = m->hdr.session_id;
    Cur_addr     = *from;
    Aru          = 0;
    Last_data_ms = now_ms();
    Bytes_written = 0;
    Done_valid   = 0;
    State        = ST_RECEIVING;

    Stray_reported = 0;
    memset(Have, 0, sizeof(Have));
    strcpy(Dst_name, name);

    Fp = fopen(Dst_name, "wb");
    if (Fp == NULL) {
        fprintf(stderr, "rcv: cannot open %s for writing: %s\n",
                Dst_name, strerror(errno));
        State = ST_IDLE;
        return;
    }

    stats_start(&Stats, "rcv", file_size);

    printf("[rcv]   adopted session %" PRIu32 ": \"%s\", %" PRIu64 " bytes, N = %" PRIu32 "\n",
           Cur_session, Dst_name, file_size, N);
    printf("[rcv]   RECEIVING\n");

    if (N == 0) {
        Complete_session();              /* empty file: seq 0 is the whole thing */
        return;
    }
    Send_feedback();
}

/* Out-of-order packets wait in the ring until the gap below them is filled. */
static void Store_packet(const ncp_msg *m) {
    uint32_t seq = m->hdr.seq;
    int      idx;

    if (seq <= Aru || seq > N) {
        return;                          /* already written, or past the end */
    }
    if (seq - Aru >= W_MAX) {
        /* Beyond what the ring can hold without aliasing. Cannot happen while
         * W < W_MAX, but dropping it is the only safe answer if it ever does. */
        printf("[rcv]   dropped seq %" PRIu32 ": %" PRIu32 " past aru, ring holds %d\n",
               seq, seq - Aru, W_MAX);
        return;
    }

    idx = seq % W_MAX;
    memcpy(Slot[idx], m->payload, m->hdr.body_len);
    Slot_len[idx] = m->hdr.body_len;
    Have[idx]     = 1;
}

/* Bytes only reach the file once every gap below them is filled, so the file
 * is always a correct prefix and Bytes_written is exactly the "received in
 * order" number the report asks for. */
static void Advance_aru(void) {
    while (Aru < N) {
        int idx = (int)((Aru + 1) % W_MAX);

        if (!Have[idx]) {
            break;
        }
        if (fwrite(Slot[idx], 1, Slot_len[idx], Fp) != Slot_len[idx]) {
            fprintf(stderr, "rcv: write to %s failed: %s\n", Dst_name, strerror(errno));
            exit(1);
        }
        Bytes_written += Slot_len[idx];
        stats_add(&Stats, Slot_len[idx], Slot_len[idx]);
        Have[idx] = 0;
        Aru++;
    }
}

static void Complete_session(void) {
    stats_final(&Stats);
    fclose(Fp);
    Fp = NULL;

    printf("[rcv] done: %" PRIu64 " bytes written to \"%s\"\n", Bytes_written, Dst_name);

    Done_valid      = 1;
    State           = ST_LINGER;
    Linger_start_ms = now_ms();
    Send_feedback();                     /* final ACK, aru == N */
}

/* A truncated file is worse than no file: it looks valid while being corrupt. */
static void Abort_session(void) {
    printf("[rcv] session %" PRIu32 " silent for %d ms, discarding \"%s\"\n",
           Cur_session, Params->session_timeout_ms, Dst_name);
    if (Fp != NULL) {
        fclose(Fp);
        Fp = NULL;
    }
    remove(Dst_name);
    Stray_reported = 0;
    State = ST_IDLE;
}

static void Send_feedback(void) {
    rcv_msg m;

    memset(&m, 0, sizeof(m));
    m.hdr.type       = MSG_FEEDBACK;
    m.hdr.session_id = Cur_session;
    m.hdr.seq        = Aru;
    m.hdr.extra      = 0;                /* no gaps to report yet */
    m.hdr.body_len   = 0;

    sendto_dbg(Sock, (const char *)&m, HDR_LEN, 0,
               (struct sockaddr *)&Cur_addr, sizeof(Cur_addr));
    Last_fb_ms = now_ms();
}

static void Send_busy(uint32_t session, const struct sockaddr_in *to) {
    rcv_msg m;

    memset(&m, 0, sizeof(m));
    m.hdr.type       = MSG_BUSY;
    m.hdr.session_id = session;          /* echo theirs, not ours */
    m.hdr.body_len   = 0;

    sendto_dbg(Sock, (const char *)&m, HDR_LEN, 0,
               (struct sockaddr *)to, sizeof(*to));
}

static void Init_socket(void) {
    struct sockaddr_in addr;
    int                port;

    port = atoi(Port_Str);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "rcv: bad port %s\n", Port_Str);
        exit(1);
    }

    Sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (Sock < 0) {
        perror("rcv: socket");
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(Sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "rcv: cannot bind port %d: %s\n", port, strerror(errno));
        exit(1);
    }
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 4) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    Port_Str = argv[2];

    if (!strncmp(argv[3], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[3], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }
}

static void Print_help(void) {
    printf("Usage: rcv <loss_rate_percent> <port> <env>\n");
    exit(0);
}
