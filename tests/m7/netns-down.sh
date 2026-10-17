#!/bin/sh
# netns-down.sh: remove everything netns-up.sh made. Run with sudo. Safe to rerun.
NS=talkipm7

for h in $(nft -a list chain ip filter FORWARD 2>/dev/null | sed -n 's/.*comment "talkip-m7".*# handle \([0-9]*\)/\1/p'); do
    nft delete rule ip filter FORWARD handle "$h"
done
nft delete table ip talkipm7nat 2>/dev/null
ip link del tm7-host 2>/dev/null
ip netns del $NS 2>/dev/null
rm -rf /etc/netns/$NS
echo "talkipm7 down"
