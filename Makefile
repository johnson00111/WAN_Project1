CC=gcc

CFLAGS = -c -Wall -pedantic -g

# make only tracks .c, so without this a .h edit is silently ignored.
HEADERS = net_include.h sendto_dbg.h stats.h

all: ncp rcv t_rcv t_ncp

ncp: ncp.o sendto_dbg.o stats.o
	    $(CC) -o ncp ncp.o sendto_dbg.o stats.o

rcv: rcv.o sendto_dbg.o stats.o
	    $(CC) -o rcv rcv.o sendto_dbg.o stats.o

t_ncp: t_ncp.o stats.o
	    $(CC) -o t_ncp t_ncp.o stats.o

t_rcv: t_rcv.o stats.o
	    $(CC) -o t_rcv t_rcv.o stats.o

clean:
	rm *.o

veryclean:
	rm ncp 
	rm rcv
	rm t_ncp
	rm t_rcv

%.o:    %.c $(HEADERS)
	$(CC) $(CFLAGS) $*.c

