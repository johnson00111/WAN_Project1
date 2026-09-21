#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "sendto_dbg.h"
#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

int main(int argc, char *argv[]) {

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
