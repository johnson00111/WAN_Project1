#!/bin/sh
# Sweep one sender-side parameter and print a CSV row per run.
#
#   ./tune.sh <LAN|WAN> <rcv_host> <loss%> <macro> <values...>
#   ./tune.sh LAN rcv 0 W_LAN 64 128 256 512 1024
#   ./tune.sh WAN rcv 10 W_WAN 361 722 1083 1500 2000
#
# EXTRA pins other parameters while one is swept, which matters because the
# window and the retransmission suppression interact:
#
#   EXTRA="-DRETX_SUPPRESS_LAN=10" ./tune.sh LAN rcv 10 W_LAN 64 128 256
#
# Rebuilds ncp for each value, runs one 100 MB transfer, and pulls the rate out
# of the [FINAL] line. Only sender-side parameters can be swept this way --
# FB_PERIOD lives in the receiver, so that one needs the same treatment on the
# other container.
#
# Leaves the default build in place when it finishes.

set -e

MODE=$1; HOST=$2; LOSS=$3; MACRO=$4
shift 4

SRC=testdata/test100MB.txt
DST=tune.out
RUNS=${RUNS:-3}

if [ ! -f "$SRC" ]; then
    echo "missing $SRC -- run: python3 gen_file.py -f $SRC -n 100000000" >&2
    exit 1
fi

echo "# EXTRA=$EXTRA"
echo "mode,loss,$MACRO,run,mbps,elapsed_s,raw_bytes,overhead"

for VALUE in "$@"; do
    make clean >/dev/null 2>&1 || true
    make CFLAGS="-c -Wall -pedantic -g -D$MACRO=$VALUE $EXTRA" >/dev/null

    R=1
    while [ "$R" -le "$RUNS" ]; do
        # The receiver refuses a new session while it LINGERs over the previous
        # one, so give it room rather than racing it.
        sleep 3
        LINE=$(./ncp "$LOSS" "$MODE" "$SRC" "$DST@$HOST:5000" | grep '^\[FINAL\]' || true)
        if [ -z "$LINE" ]; then
            echo "$MODE,$LOSS,$VALUE,$R,FAILED,,," 
        else
            MBPS=$(echo "$LINE" | sed 's/.*avg_mbps=\([0-9.]*\).*/\1/')
            SECS=$(echo "$LINE" | sed 's/.*elapsed_s=\([0-9.]*\).*/\1/')
            RAW=$(echo "$LINE" | sed 's/.*raw=\([0-9]*\).*/\1/')
            OVER=$(echo "$LINE" | sed 's/.*overhead=\([0-9.]*\).*/\1/')
            echo "$MODE,$LOSS,$VALUE,$R,$MBPS,$SECS,$RAW,$OVER"
        fi
        R=$((R + 1))
    done
done

make clean >/dev/null 2>&1 || true
make >/dev/null
