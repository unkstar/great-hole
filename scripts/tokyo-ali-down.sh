#!/bin/bash
# Ali side: teardown FEC tunnel to Tokyo
# fec 模式下 table 102 / ip rule 归 switch-tunnel 管理, 停止嵌套隧道时不得清空,
# 否则 fec 出口路由被连带删除导致断网 (2026-08-29 事故复盘)。
set -e
MODE="$(cat /etc/great-hole/fec/tunnel-mode 2>/dev/null || echo prod)"

ip rule del from 172.31.17.2/32 table 102 2>/dev/null || true
ip route del 172.31.30.2 dev fec-tokyo 2>/dev/null || true

if [ "$MODE" != "fec" ]; then
    ip route flush table 102 2>/dev/null || true
    iptables -D FORWARD -i fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || true
    iptables -D FORWARD -o fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || true
    iptables -D FORWARD -i wg0 -o fec-tokyo -j ACCEPT 2>/dev/null || true
    iptables -D FORWARD -i fec-tokyo -o wg0 -j ACCEPT 2>/dev/null || true
    iptables -D FORWARD -i tun0 -o fec-tokyo -j ACCEPT 2>/dev/null || true
    iptables -D FORWARD -i fec-tokyo -o tun0 -j ACCEPT 2>/dev/null || true
fi

echo "fec-tokyo torn down (mode=$MODE)"
