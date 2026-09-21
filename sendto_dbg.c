#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>

static int first_time = 1;
static int cutoff = 64; /* default is 25% loss */

void sendto_dbg_init(int percent)
{
    /* percent is in integer form (i.e. 1 = 1%, 5 = 5%) */
    cutoff  = (percent * 0.01 * 0xff);
    printf("\n++++++++++ cutoff value is %d +++++++++\n", cutoff);
}

int sendto_dbg(int s, const char *buf, int len, int flags,
               const struct sockaddr *to, int tolen )
{
    int ret;
    int decision;

    if (first_time)
    {
        struct timeval t;
        gettimeofday( &t, NULL );
        srand( t.tv_sec );
        printf("\n+++++++++\n seed is %d\n++++++++\n", (int)t.tv_sec);
        first_time = 0;
    }

    decision = rand() & 0xff;
    if ((cutoff > 0) && (decision <= cutoff)) { /* drop the packet, but claim success */
        return (len);
    }
    ret = sendto(s, buf, len, flags, to, tolen);

    return (ret);
}
