#!/bin/bash
# Tokyo side: setup fec-tokyo interface, SNAT, and routing
set -e

# System tuning
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.rmem_max=16777216
sysctl -w net.ipv4.ip_forward=1

# Configure fec-tokyo TUN interface (Tokyo side: 172.31.30.2)
ip addr add 172.31.30.2 peer 172.31.30.1/32 dev fec-tokyo 2>/dev/null || true
ip link set fec-tokyo mtu 1420 up

# SNAT for traffic coming from Ali (ER-X and OpenVPN subnets)
iptables -t nat -C POSTROUTING -s 172.31.17.0/24 -o eth0 -j MASQUERADE 2>/dev/null || \
    iptables -t nat -A POSTROUTING -s 172.31.17.0/24 -o eth0 -j MASQUERADE
iptables -t nat -C POSTROUTING -s 172.31.16.0/24 -o eth0 -j MASQUERADE 2>/dev/null || \
    iptables -t nat -A POSTROUTING -s 172.31.16.0/24 -o eth0 -j MASQUERADE

# Return routes: traffic back to Ali through fec-tokyo
ip route replace 172.31.17.0/24 via 172.31.30.1 dev fec-tokyo 2>/dev/null || true
ip route replace 172.31.16.0/24 via 172.31.30.1 dev fec-tokyo 2>/dev/null || true

echo "fec-tokyo ready: 172.31.30.2 -> 172.31.30.1 (SNAT enabled)"
