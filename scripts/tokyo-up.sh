#!/bin/bash
# Tokyo side: setup fec-tokyo interface, SNAT, and return routes
# 出口模式: /etc/great-hole/fec/tunnel-mode (prod=嵌套隧道 / fec=RS FEC)
# fec 模式下回程路由 (172.31.16/17.0/24) 归 switch-tunnel 管理 (via fec-test),
# 本脚本启动时不得覆盖 — 否则嵌套隧道重启会把回程路由改回 fec-tokyo,
# fec 模式回程就断了 (2026-08-29 事故复盘)。
set -e
MODE="$(cat /etc/great-hole/fec/tunnel-mode 2>/dev/null || echo prod)"

sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.rmem_max=16777216
sysctl -w net.ipv4.ip_forward=1
ip tuntap add dev fec-tokyo mode tun 2>/dev/null || true
ip addr add 172.31.30.2/32 peer 172.31.30.1/32 dev fec-tokyo 2>/dev/null || true
ip link set fec-tokyo mtu 1420 up
ip route replace 172.31.30.1 dev fec-tokyo 2>/dev/null || true

# SNAT for traffic from Ali (after MASQUERADE and direct ER-X/OVPN subnets)
iptables -t nat -C POSTROUTING -s 172.31.30.0/30 -o eth0 -j MASQUERADE 2>/dev/null || \
    iptables -t nat -A POSTROUTING -s 172.31.30.0/30 -o eth0 -j MASQUERADE
iptables -t nat -C POSTROUTING -s 172.31.17.0/24 -o eth0 -j MASQUERADE 2>/dev/null || \
    iptables -t nat -A POSTROUTING -s 172.31.17.0/24 -o eth0 -j MASQUERADE
iptables -t nat -C POSTROUTING -s 172.31.16.0/24 -o eth0 -j MASQUERADE 2>/dev/null || \
    iptables -t nat -A POSTROUTING -s 172.31.16.0/24 -o eth0 -j MASQUERADE

if [ "$MODE" != "fec" ]; then
    # Return routes for ER-X and OpenVPN subnets (prod 模式专属)
    ip route replace 172.31.17.0/24 via 172.31.30.1 dev fec-tokyo 2>/dev/null || true
    ip route replace 172.31.16.0/24 via 172.31.30.1 dev fec-tokyo 2>/dev/null || true
fi

echo "fec-tokyo ready (mode=$MODE): 172.31.30.2 -> 172.31.30.1 SNAT enabled"
