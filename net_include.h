#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> 
#include <netdb.h>

#define MAX_MESS_LEN 1400

#define MODE_LAN 1
#define MODE_WAN 2

typedef struct dummy_ncp_msg {
    /* Fill in header information needed for your protocol */
    char payload[MAX_MESS_LEN];
} ncp_msg;

/* Uncomment and fill in fields for your protocol */
/*
typedef struct dummy_rcv_msg {
} rcv_msg;
*/
