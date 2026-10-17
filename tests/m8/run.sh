#!/bin/sh
# run.sh up|down: the M8 Snow 512Ke, Xvfb :78, LToUDP on the local AppleTalk network (a router and a MacIP gateway
# reachable there). Workspace $M8_DIR (default ~/talkip-m8/snow): m8.snoww and m8.hda, an HD20 System 6.0.8 disk.
# Never touch m8.hda (hcopy) while it runs: two writers corrupt it. `up` refuses if one is running.
set -eu
W=${M8_DIR:-$HOME/talkip-m8/snow}
SNOW=${SNOW:-snowemu-hd20}
cd "$W"
running() { ps -eo pid=,args= | awk -v a="$SNOW m8.snoww" '{p=$1; $1=""; sub(/^ /,""); if ($0==a) print p}'; }
case "${1:-}" in
up)
    [ -z "$(running)" ] || { echo "M8 Snow already running: $(running)" >&2; exit 1; }
    Xvfb :78 -screen 0 800x600x24 >xvfb.log 2>&1 & echo $! >xvfb.pid
    sleep 1
    DISPLAY=:78 "$SNOW" m8.snoww >snow.log 2>&1 & echo $! >snow.pid
    ;;
down)
    for p in $(running); do kill "$p"; done
    [ -f xvfb.pid ] && kill "$(cat xvfb.pid)" 2>/dev/null || true
    rm -f snow.pid xvfb.pid
    sleep 2
    [ -z "$(running)" ] || { echo "still running: $(running)" >&2; exit 1; }
    ;;
*) echo "usage: run.sh up|down" >&2; exit 2 ;;
esac
