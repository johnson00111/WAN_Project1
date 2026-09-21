#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "sendto_dbg.h"
#include "net_include.h"
#include "stats.h"

/* Sender states. The state picks the poll interval, which is why this is the
 * shape of the main loop rather than a feature bolted on later.
 *
 *   PROBING --BUSY--> BLOCKED --receiver frees up--> PROBING
 *   PROBING --FEEDBACK--> TRANSFERRING
 */
#define ST_PROBING      1
#define ST_BLOCKED      2
#define ST_TRANSFERRING 3

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static void Init_socket(void);
static void Stat_source(void);
static void Send_seq0(void);
static void Handle_reply(const rcv_msg *m, int len);
static const char *State_name(int s);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;
static const params *Params;

/* Session state */
static int             Sock;
static struct sockaddr_in Dest;
static uint32_t        Session_id;
static uint64_t        File_size;
static uint32_t        N;            /* highest sequence number */
static int             State;
static uint32_t        Last_rx_ms;   /* last packet of any kind from the receiver */
static int             Blocked_reported;
static stats_t         Stats;

int main(int argc, char *argv[]) {

    /* Line-buffered: progress has to show up promptly when stdout is a pipe
     * or a log file, not just on a terminal. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }

    /* Also enforces W < W_MAX -- see net_include.h. */
    Params = params_for(Mode);
    printf("\tWire format: header %d B + payload %d B = datagram %d B\n",
           (int)sizeof(pkt_hdr), PAYLOAD, (int)sizeof(ncp_msg));
    printf("\tSender params: W = %d pkt, TIMEOUT = %d ms, RETX_SUPPRESS = %d ms,\n"
           "\t               BUSY_RETRY = %d ms, GIVE_UP_SILENCE = %d ms\n",
           Params->W, Params->timeout_ms, Params->retx_suppress_ms,
           Params->busy_retry_ms, Params->give_up_ms);

    Stat_source();
    Init_socket();

    /* Random enough to tell one transfer from another. */
    Session_id = (uint32_t)(now_ms() ^ (getpid() << 16));

    printf("\tFile size = %" PRIu64 " bytes, N = %" PRIu32 " packets, session_id = %" PRIu32 "\n",
           File_size, N, Session_id);

    State = ST_PROBING;
    Last_rx_ms = now_ms();
    Send_seq0();
    printf("[ncp] PROBING, sent seq 0\n");

    for (;;) {
        fd_set         mask;
        struct timeval tv;
        int            poll_ms;
        int            ready;

        /* This is the whole point of the state machine: a blocked sender polls
         * slowly so it never floods a transfer already in progress. */
        poll_ms = (State == ST_BLOCKED) ? Params->busy_retry_ms : Params->timeout_ms;

        FD_ZERO(&mask);
        FD_SET(Sock, &mask);
        tv = ms_to_tv(poll_ms);

        ready = select(Sock + 1, &mask, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("ncp: select");
            exit(1);
        }

        if (ready > 0 && FD_ISSET(Sock, &mask)) {
            rcv_msg m;
            ssize_t got;

            got = recvfrom(Sock, &m, sizeof(m), 0, NULL, NULL);
            if (got < (ssize_t)HDR_LEN) {
                continue;               /* too short to be ours */
            }
            Handle_reply(&m, (int)got);
            continue;
        }

        /* Poll expiry. */
        if (now_ms() - Last_rx_ms > (uint32_t)Params->give_up_ms) {
            printf("[ncp] no reply for %d ms, giving up\n", Params->give_up_ms);
            exit(1);
        }

        switch (State) {
        case ST_PROBING:
            /* The receiver is still IDLE and ignores every non-zero sequence,
             * so seq 0 is the only thing worth resending. */
            Send_seq0();
            printf("[ncp] PROBING timeout, resent seq 0\n");
            break;

        case ST_BLOCKED:
            Send_seq0();
            printf("[ncp] BLOCKED, retrying seq 0\n");
            break;

        case ST_TRANSFERRING:
            /* TODO stage 3: top the window up and probe with aru + 1. */
            break;
        }
    }
}

/* seq 0 carries the metadata: uint64 file_size, uint32 N, NUL-terminated
 * destination filename. It goes through the normal reliability path, which is
 * why there is no separate handshake. */
static void Send_seq0(void) {
    ncp_msg m;
    size_t  name_len;
    int     body;

    name_len = strlen(Dst_filename) + 1;

    memset(&m, 0, sizeof(m));
    m.hdr.type       = MSG_DATA;
    m.hdr.session_id = Session_id;
    m.hdr.seq        = 0;
    m.hdr.extra      = N;

    memcpy(m.payload, &File_size, sizeof(File_size));
    memcpy(m.payload + 8, &N, sizeof(N));
    memcpy(m.payload + 12, Dst_filename, name_len);

    body = 12 + (int)name_len;
    m.hdr.body_len = (uint16_t)body;

    sendto_dbg(Sock, (const char *)&m, HDR_LEN + body, 0,
               (struct sockaddr *)&Dest, sizeof(Dest));
}

static void Handle_reply(const rcv_msg *m, int len) {
    if (m->hdr.session_id != Session_id) {
        return;                          /* someone else's transfer */
    }

    if (m->hdr.body_len > len - HDR_LEN) {
        return;                          /* claims more body than arrived */
    }

    Last_rx_ms = now_ms();

    switch (m->hdr.type) {
    case MSG_FEEDBACK:
        if (State != ST_TRANSFERRING) {
            /* Clock starts here: time spent PROBING or BLOCKED does not count. */
            stats_start(&Stats, "ncp", File_size);
            State = ST_TRANSFERRING;
        }
        printf("[ncp] FEEDBACK aru=%" PRIu32 " bitmap_bits=%" PRIu32 " (%d B) -> %s\n",
               m->hdr.seq, m->hdr.extra, len, State_name(State));

        /* TODO stage 3: this is where the window gets topped up and the
         * transfer actually runs. Stage 2 stops once the path is proven. */
        printf("[ncp] stage 2: two-way path works, data transfer not implemented yet\n");
        exit(0);

    case MSG_BUSY:
        if (State != ST_BLOCKED) {
            State = ST_BLOCKED;
            if (!Blocked_reported) {
                printf("[ncp] blocked: the receiver is busy with another transfer\n");
                Blocked_reported = 1;
            }
        }
        break;

    default:
        break;                           /* senders never see DATA */
    }
}

static void Stat_source(void) {
    struct stat st;

    if (stat(Src_filename, &st) < 0) {
        fprintf(stderr, "ncp: cannot stat %s: %s\n", Src_filename, strerror(errno));
        exit(1);
    }
    File_size = (uint64_t)st.st_size;
    N = (uint32_t)((File_size + PAYLOAD - 1) / PAYLOAD);   /* 0 for an empty file */

    if (strlen(Dst_filename) + 1 > PAYLOAD - 12) {
        fprintf(stderr, "ncp: destination filename too long for one packet\n");
        exit(1);
    }
}

static void Init_socket(void) {
    struct addrinfo  hints;
    struct addrinfo *res;
    int              err;

    Sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (Sock < 0) {
        perror("ncp: socket");
        exit(1);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    err = getaddrinfo(Hostname, Port_Str, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "ncp: cannot resolve %s:%s: %s\n",
                Hostname, Port_Str, gai_strerror(err));
        exit(1);
    }
    memcpy(&Dest, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
}

static const char *State_name(int s) {
    switch (s) {
    case ST_PROBING:      return "PROBING";
    case ST_BLOCKED:      return "BLOCKED";
    case ST_TRANSFERRING: return "TRANSFERRING";
    default:              return "?";
    }
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {

    if (argc != 5) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    if (!strncmp(argv[2], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[2], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }

    Src_filename = argv[3];
    Dst_filename = strtok(argv[4], "@");
    Hostname = strtok(NULL, ":");
    if (Hostname == NULL) {
        printf("Error: no hostname provided\n");
        Print_help();
    }
    Port_Str = strtok(NULL, ":");
    if (Port_Str == NULL) {
        printf("Error: no port provided\n");
        Print_help();
    }
}

static void Print_help(void) {
    printf("Usage: ncp <loss_rate_percent> <env> <source_file_name> <dest_file_name>@<ip_addr>:<port>\n");
    exit(0);
}
