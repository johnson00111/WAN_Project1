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

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static void Init_socket(void);
static void Handle_packet(const ncp_msg *m, int len, const struct sockaddr_in *from);
static void Adopt_session(const ncp_msg *m, const struct sockaddr_in *from);
static void Send_feedback(void);
static void Send_busy(uint32_t session, const struct sockaddr_in *to);

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
static stats_t            Stats;

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
            if (got < (ssize_t)HDR_LEN) {
                continue;               /* too short to be ours */
            }
            Handle_packet(&m, (int)got, &from);
            continue;
        }

        /* Poll expiry. A sender that dies mid-transfer must not pin the
         * receiver forever. Deleting the partial file belongs here too, once
         * there is a file. */
        if (State == ST_RECEIVING &&
            now_ms() - Last_data_ms > (uint32_t)Params->session_timeout_ms) {
            printf("[rcv] session %" PRIu32 " silent for %d ms, back to IDLE\n",
                   Cur_session, Params->session_timeout_ms);
            State = ST_IDLE;
        }

        /* TODO: periodic FEEDBACK every FB_PERIOD once a transfer is running. */
    }
}

static void Handle_packet(const ncp_msg *m, int len, const struct sockaddr_in *from) {
    printf("[rcv] from %s:%d  type=%" PRIu8 " seq=%" PRIu32 " extra=%" PRIu32
           " body_len=%" PRIu16 " (%d B)\n",
           inet_ntoa(from->sin_addr), ntohs(from->sin_port),
           m->hdr.type, m->hdr.seq, m->hdr.extra, m->hdr.body_len, len);

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

    if (State == ST_IDLE) {
        if (m->hdr.seq != 0) {
            /* Only seq 0 carries the filename, so nothing else is usable here.
             * Almost always a leftover from an abandoned transfer. */
            printf("[rcv]   ignored: IDLE and seq != 0\n");
            return;
        }
        Adopt_session(m, from);
        return;
    }

    /* RECEIVING */
    if (m->hdr.session_id != Cur_session) {
        printf("[rcv]   busy: session %" PRIu32 " is in progress\n", Cur_session);
        Send_busy(m->hdr.session_id, from);
        return;
    }

    Last_data_ms = now_ms();

    if (m->hdr.seq == 0) {
        printf("[rcv]   duplicate seq 0, re-acking\n");
    } else {
        /* TODO stage 3: buffer at slot[seq % W_MAX], walk aru forward, write
         * out whatever just became contiguous. */
        printf("[rcv]   data seq %" PRIu32 " (not buffered yet)\n", m->hdr.seq);
    }
    Send_feedback();
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
    State        = ST_RECEIVING;

    stats_start(&Stats, "rcv", file_size);

    printf("[rcv]   adopted session %" PRIu32 ": \"%s\", %" PRIu64 " bytes, N = %" PRIu32 "\n",
           Cur_session, name, file_size, N);
    printf("[rcv]   RECEIVING\n");

    /* TODO stage 3: open the file here. */
    Send_feedback();
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
