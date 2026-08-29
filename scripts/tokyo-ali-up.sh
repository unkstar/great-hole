#!/bin/bash
# Ali side: setup fec-tokyo interface and full routing for Tokyo exit
# 出口模式: /etc/great-hole/fec/tunnel-mode (prod=嵌套隧道 / fec=RS FEC)
# fec 模式下 table 102 出口路由归 switch-tunnel 管理 (指向 fec-test), 本脚本
# 启动时不得覆盖 — 否则 Ali 重启后出口会错误回到嵌套隧道 (2026-08-29 事故)。
set -e
MODE="$(cat /etc/great-hole/fec/tunnel-mode 2>/dev/null || echo prod)"

sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.rmem_max=16777216
sysctl -w net.ipv4.ip_forward=1
ip tuntap add dev fec-tokyo mode tun 2>/dev/null || true
ip addr add 172.31.30.1/32 peer 172.31.30.2/32 dev fec-tokyo 2>/dev/null || true
ip link set fec-tokyo mtu 1420 up
ip route replace 172.31.30.2 dev fec-tokyo 2>/dev/null || true

if [ "$MODE" != "fec" ]; then
    # Table 102: default route via Tokyo for ER-X and OpenVPN traffic (prod 模式专属)
    ip route replace 172.31.26.25 dev hole table 102 2>/dev/null || true
    ip route replace 172.31.17.0/24 dev wg0 table 102 2>/dev/null || true
    ip route replace 172.31.16.0/24 dev tun0 table 102 2>/dev/null || true
    ip route replace 172.31.30.2 dev fec-tokyo table 102
    ip route replace default via 172.31.30.2 dev fec-tokyo table 102 2>/dev/null || true

    # MSS clamping for fec-tokyo (idempotent)
    iptables -C FORWARD -i fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || \
        iptables -I FORWARD 1 -i fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380
    iptables -C FORWARD -o fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380 2>/dev/null || \
        iptables -I FORWARD 1 -o fec-tokyo -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 1380

    # Forward rules for ER-X and OpenVPN through fec-tokyo
    iptables -C FORWARD -i wg0 -o fec-tokyo -j ACCEPT 2>/dev/null || iptables -I FORWARD -i wg0 -o fec-tokyo -j ACCEPT
    iptables -C FORWARD -i fec-tokyo -o wg0 -j ACCEPT 2>/dev/null || iptables -I FORWARD -i fec-tokyo -o wg0 -j ACCEPT
    iptables -C FORWARD -i tun0 -o fec-tokyo -j ACCEPT 2>/dev/null || iptables -I FORWARD -i tun0 -o fec-tokyo -j ACCEPT
    iptables -C FORWARD -i fec-tokyo -o tun0 -j ACCEPT 2>/dev/null || iptables -I FORWARD -i fec-tokyo -o tun0 -j ACCEPT

    ip rule add from 172.31.17.2/32 table 102 2>/dev/null || ip rule replace from 172.31.17.2/32 table 102
fi

echo "fec-tokyo ready (mode=$MODE): 172.31.30.1 -> 172.31.30.2"
