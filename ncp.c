#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/mman.h>
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
static void Map_source(void);
static void Send_seq0(void);
static void Send_data(uint32_t seq);
static void Fill_window(void);
static void Finish(void);
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

static const char *Base;             /* mmap()ed source; doubles as the send buffer */
static uint32_t    Aru;              /* highest in-order sequence the receiver confirmed */
static uint32_t    Next_new;         /* next sequence never sent */
static uint64_t    Bytes_raw;        /* everything put on the wire, headers included */
static uint32_t    Last_probe_ms;    /* rate limit on the timeout safety net */

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

    Map_source();
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
            if (got >= (ssize_t)HDR_LEN) {
                Handle_reply(&m, (int)got);
            }
        }

        /* Timers run every iteration, not only when select() times out. With
         * feedback arriving every FB_PERIOD the socket is almost always
         * readable, and anything left in a timeout-only branch would never
         * execute. */
        if (now_ms() - Last_rx_ms > (uint32_t)Params->give_up_ms) {
            printf("[ncp] no reply for %d ms, giving up\n", Params->give_up_ms);
            exit(1);
        }

        /* The safety net: nothing from the receiver for a whole poll interval.
         * TIMEOUT is 10x FB_PERIOD, so while feedback flows this never fires
         * and retransmission is left to the receiver's NACKs. */
        if (now_ms() - Last_rx_ms < (uint32_t)poll_ms ||
            now_ms() - Last_probe_ms < (uint32_t)poll_ms) {
            continue;
        }
        Last_probe_ms = now_ms();

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
            /* Only aru + 1 moves the left edge, so poke at that and refill
             * whatever the window still allows. */
            Send_data(Aru + 1);
            Fill_window();
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
    Bytes_raw += HDR_LEN + body;
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
            Next_new = 1;
            printf("[ncp] %s, %" PRIu32 " packets to send\n", State_name(State), N);
        }

        if (m->hdr.seq > Aru) {
            uint64_t acked = (uint64_t)(m->hdr.seq - Aru) * PAYLOAD;

            if (m->hdr.seq == N) {
                acked = File_size - (uint64_t)Aru * PAYLOAD;   /* last one is short */
            }
            Aru = m->hdr.seq;
            stats_add(&Stats, acked, 0);
        }

        if (Aru >= N) {
            Finish();
        }

        /* TODO stage 4: rebuild retx[] from the bitmap and send those first. */
        Fill_window();
        break;

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

/* Read-only mmap instead of a send buffer: the payload for sequence s is just
 * Base + (s-1) * PAYLOAD, so a retransmission is pointer arithmetic and text
 * and binary files are handled identically. */
static void Map_source(void) {
    struct stat st;
    int         fd;
    void       *p;

    if (strlen(Dst_filename) + 1 > PAYLOAD - 12) {
        fprintf(stderr, "ncp: destination filename too long for one packet\n");
        exit(1);
    }

    fd = open(Src_filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ncp: cannot open %s: %s\n", Src_filename, strerror(errno));
        exit(1);
    }
    if (fstat(fd, &st) < 0) {
        perror("ncp: fstat");
        exit(1);
    }

    File_size = (uint64_t)st.st_size;
    N = (uint32_t)((File_size + PAYLOAD - 1) / PAYLOAD);   /* 0 for an empty file */

    if (File_size == 0) {
        /* mmap() of a zero-length file fails with EINVAL, and there is nothing
         * to map anyway -- seq 0 alone completes the transfer. */
        Base = NULL;
        close(fd);
        return;
    }

    p = mmap(NULL, (size_t)File_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "ncp: cannot mmap %s: %s\n", Src_filename, strerror(errno));
        exit(1);
    }
    Base = (const char *)p;
    close(fd);                           /* the mapping keeps its own reference */
}

/* Sequence s >= 1 carries the file bytes at (s-1) * PAYLOAD. Only the last one
 * is short. */
static void Send_data(uint32_t seq) {
    ncp_msg  m;
    uint64_t off;
    int      body;

    if (seq < 1 || seq > N) {
        return;
    }

    off  = (uint64_t)(seq - 1) * PAYLOAD;
    body = (File_size - off > PAYLOAD) ? PAYLOAD : (int)(File_size - off);

    m.hdr.type       = MSG_DATA;
    m.hdr.reserved   = 0;
    m.hdr.body_len   = (uint16_t)body;
    m.hdr.session_id = Session_id;
    m.hdr.seq        = seq;
    m.hdr.extra      = N;
    memcpy(m.payload, Base + off, (size_t)body);

    sendto_dbg(Sock, (const char *)&m, HDR_LEN + body, 0,
               (struct sockaddr *)&Dest, sizeof(Dest));
    Bytes_raw += HDR_LEN + body;
}

/* The window is [aru+1, aru+W] and nothing outside it is ever transmitted. */
static void Fill_window(void) {
    while (Next_new <= N && Next_new <= Aru + (uint32_t)Params->W) {
        Send_data(Next_new);
        Next_new++;
    }
}

/* aru == N means the receiver holds the whole file. The sender has the final
 * ACK in hand, so there is nothing left to confirm and it can just leave. */
static void Finish(void) {
    stats_final(&Stats);
    printf("[ncp] done: aru == N == %" PRIu32 ", %" PRIu64 " bytes acked, "
           "%" PRIu64 " bytes on the wire\n", N, File_size, Bytes_raw);
    exit(0);
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
