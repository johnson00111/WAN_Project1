CC=gcc

CFLAGS = -c -Wall -pedantic -g

all: ncp rcv t_rcv t_ncp

ncp: ncp.o sendto_dbg.o
	    $(CC) -o ncp ncp.o sendto_dbg.o

rcv: rcv.o sendto_dbg.o
	    $(CC) -o rcv rcv.o sendto_dbg.o

t_ncp: t_ncp.o
	    $(CC) -o t_ncp t_ncp.o

t_rcv: t_rcv.o
	    $(CC) -o t_rcv t_rcv.o

clean:
	rm *.o

veryclean:
	rm ncp 
	rm rcv
	rm t_ncp
	rm t_rcv

%.o:    %.c
	$(CC) $(CFLAGS) $*.c


