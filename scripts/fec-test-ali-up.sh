#!/bin/bash
# Ali side: TEST-ONLY fec-test interface (172.31.40.0/30). No routing/NAT — throughput test only.
set -e
ip tuntap add dev fec-test mode tun 2>/dev/null || true
ip addr add 172.31.40.1/32 peer 172.31.40.2/32 dev fec-test 2>/dev/null || true
ip link set fec-test mtu 1420 up
ip route replace 172.31.40.2 dev fec-test 2>/dev/null || true
echo "fec-test ready: 172.31.40.1 -> 172.31.40.2"
