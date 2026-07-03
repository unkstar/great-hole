#!/bin/bash
# Ali side: teardown fec-tokyo routing
set -e

# Remove policy routing
ip rule del from 172.31.17.2/32 table 102 2>/dev/null || true
ip route flush table 102 2>/dev/null || true

# Remove MSS clamp
iptables -D FORWARD -i fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || true
iptables -D FORWARD -o fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || true

echo "fec-tokyo routing removed"
