#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

int main(int argc, char *argv[]) {

    /* Initialize */
    Usage(argc, argv);
    printf("Successfully initialized with:\n");
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {

    if (argc != 3) {
        Print_help();
    }

    Src_filename = argv[1];
    Dst_filename = strtok(argv[2], "@");
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
    printf("Usage: t_ncp <source_file_name> <dest_file_name>@<ip_addr>:<port>\n");
    exit(0);
}
