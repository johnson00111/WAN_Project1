#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sendto_dbg.h"
#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;

int main(int argc, char *argv[]) {
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
