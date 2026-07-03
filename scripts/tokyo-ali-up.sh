#!/bin/bash
# Ali side: setup fec-tokyo interface and routing
set -e

# System tuning
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.rmem_max=16777216
sysctl -w net.ipv4.ip_forward=1

# Configure fec-tokyo TUN interface (Ali side: 172.31.30.1)
ip addr add 172.31.30.1 peer 172.31.30.2/32 dev fec-tokyo 2>/dev/null || true
ip link set fec-tokyo mtu 1420 up

# Policy routing table 102 for Tokyo exit
ip route replace default via 172.31.30.2 dev fec-tokyo table 102 2>/dev/null || true

# MSS clamping for fec-tokyo (idempotent)
iptables -C FORWARD -i fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || \
    iptables -I FORWARD 1 -i fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380
iptables -C FORWARD -o fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || \
    iptables -I FORWARD 1 -o fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380

echo "fec-tokyo interface ready: 172.31.30.1 -> 172.31.30.2"
