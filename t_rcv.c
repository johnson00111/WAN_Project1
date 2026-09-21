#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static char *Port_Str;

int main(int argc, char *argv[]) {
    /* Initialize */
    Usage(argc, argv);
    printf("Successfully initialized with:\n");
    printf("\tPort = %s\n", Port_Str);
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 2) {
        Print_help();
    }

    Port_Str = argv[1];
}

static void Print_help(void) {
    printf("Usage: t_rcv <port>\n");
    exit(0);
}
