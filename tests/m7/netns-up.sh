#!/bin/sh
# netns-up.sh: the isolated network for M7 (ClassicStack vs macipgw), on the build host. Run with sudo.
#
#   namespace talkipm7:  lo (multicast on, 239.192.76.84 routed here)  <- Snow 512Ke + ClassicStack, LToUDP
#                        tm7-ns 10.77.0.2/30 --veth--> tm7-host 10.77.0.1/30 (VM) --NAT--> LAN, internet
#
# LToUDP (multicast 239.192.76.84:1954) never leaves the namespace: the group is routed to lo, and nft drops
# any multicast out of the veth, so a real AppleTalk network on the LAN never sees it.
# Only ClassicStack's own outbound connections (its MacIP NAT proxy dials the real servers) use the veth.
set -eu
NS=talkipm7

ip netns add $NS
ip link add tm7-host type veth peer name tm7-ns
ip link set tm7-ns netns $NS
ip addr add 10.77.0.1/30 dev tm7-host
ip link set tm7-host up
ip netns exec $NS ip link set lo up
ip netns exec $NS ip link set lo multicast on
ip netns exec $NS ip addr add 10.77.0.2/30 dev tm7-ns
ip netns exec $NS ip link set tm7-ns up
ip netns exec $NS ip route add default via 10.77.0.1
ip netns exec $NS ip route add 239.192.76.84/32 dev lo
ip netns exec $NS nft -f - <<'EOF'
table ip talkipm7 {
    chain out {
        type filter hook output priority filter; policy accept;
        oifname "tm7-ns" ip daddr 224.0.0.0/4 drop
    }
}
EOF
mkdir -p /etc/netns/$NS
echo "nameserver ${DNS:-1.1.1.1}" > /etc/netns/$NS/resolv.conf

# outbound for the namespace: Docker's FORWARD policy is drop, so accept ours first, then masquerade
nft insert rule ip filter FORWARD iifname "tm7-host" accept comment \"talkip-m7\"
nft insert rule ip filter FORWARD oifname "tm7-host" ct state established,related accept comment \"talkip-m7\"
nft add table ip talkipm7nat
nft add chain ip talkipm7nat post '{ type nat hook postrouting priority srcnat; }'
nft add rule ip talkipm7nat post ip saddr 10.77.0.0/30 oifname != "tm7-host" masquerade
echo "talkipm7 up"
