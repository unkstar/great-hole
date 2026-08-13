#!/bin/bash
# switch-tunnel — 一键切换生产隧道(嵌套 speederv2)与 fec 隧道(RS FEC)
#
# Usage:
#   switch-tunnel {prod|fec|status}
#
# 在本机运行,通过 ssh 操作 ali/tokyo/路由器:
#   - ali:  策略路由 (ER-X 172.31.17.2 -> table 102) + table 100/102 默认网关
#           + TCPMSS 钳制 + ER-X 路由下载 URL (best effort)
#   - tokyo: 回程路由 (172.31.17.0/24, 172.31.16.0/24 经当前隧道回 ali)
#   - tokyo 的 SNAT 按源子网匹配,与隧道接口无关,无需切换
#
# 前置: 本机 ssh 密钥可达 ali (39.108.136.48) / tokyo (202.144.195.103:63916)
#   - ali sudo 密码经环境变量 ALI_SUDO_PW 提供 (不写死在脚本里)
#   - tokyo sudo 免密
#   - 路由器经 ~/.ssh/config 的 Host router (192.168.2.1)

set -euo pipefail

MODE="${1:-status}"
ALI_HOST="unkstar@39.108.136.48"
TOKYO_SSH=(ssh -o BatchMode=yes -o PasswordAuthentication=no -p 63916 "ggcuser@202.144.195.103")
ALI_SSH=(ssh -o BatchMode=yes -o PasswordAuthentication=no "$ALI_HOST")

ERX_IP="172.31.17.2"
WG_NET="172.31.17.0/24"
OVPN_NET="172.31.16.0/24"
TABLE_OVPN=100
TABLE_ERX=102
MSS=1380

# 生产嵌套隧道 (great-hole nofec + speederv2)
PROD_ALI_IF="fec-tokyo";  PROD_ALI_IP="172.31.30.1";  PROD_GW="172.31.30.2";  PROD_TOKYO_PEER="172.31.30.1"
# RS FEC 隧道 (great-hole-fec-test, 端口 20086)
FEC_ALI_IF="fec-test";    FEC_ALI_IP="172.31.40.1";    FEC_GW="172.31.40.2";    FEC_TOKYO_PEER="172.31.40.1"

# 在 ali 以 root 执行 (stdin 传入脚本内容)
ali_root() {
    if [ -n "${ALI_SUDO_PW:-}" ]; then
        { printf '%s\n' "$ALI_SUDO_PW"; cat; } | "${ALI_SSH[@]}" 'sudo -S bash -s' 2>/dev/null
    else
        ssh -t "$ALI_HOST" 'sudo bash -s' 2>/dev/null
    fi
}

# 在 tokyo 以 root 执行 (免密 sudo)
tokyo_root() {
    "${TOKYO_SSH[@]}" 'sudo bash -s'
}

ali_switch() {  # $1 = gateway, $2 = ali 侧隧道接口
    local gw="$1" if="$2"
    ali_root <<EOF
set -e
ip rule del from ${ERX_IP}/32 table ${TABLE_ERX} 2>/dev/null || true
ip rule add from ${ERX_IP}/32 table ${TABLE_ERX} 2>/dev/null || true
ip route replace default via ${gw} dev ${if} table ${TABLE_ERX}
ip route replace default via ${gw} dev ${if} table ${TABLE_OVPN}
iptables -C FORWARD -i ${if} -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss ${MSS} 2>/dev/null || \
    iptables -I FORWARD 1 -i ${if} -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss ${MSS}
iptables -C FORWARD -o ${if} -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss ${MSS} 2>/dev/null || \
    iptables -I FORWARD 1 -o ${if} -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss ${MSS}
EOF
}

tokyo_switch() {  # $1 = ali 端隧道 peer IP, $2 = tokyo 侧隧道接口, $3 = 隧道子网 (ali 出口伪装后的源地址段)
    local peer="$1" if="$2" subnet="$3"
    tokyo_root <<EOF
set -e
ip route replace ${OVPN_NET} via ${peer} dev ${if}
ip route replace ${WG_NET} via ${peer} dev ${if}
# 关键: ali 侧 POSTROUTING 把家庭流量伪装成出口接口 IP (即 ali 的隧道 IP),
# 因此 tokyo 必须对当前隧道子网做 MASQUERADE, 否则 SYN 带私网源地址出网、
# 回包永远回不来 (2026-08-13 切换 fec 后断网根因)。
iptables -t nat -C POSTROUTING -s ${subnet} -o eth0 -j MASQUERADE 2>/dev/null || \\
    iptables -t nat -A POSTROUTING -s ${subnet} -o eth0 -j MASQUERADE
EOF
}

update_router_url() {  # $1 = tokyo 端 TUN IP (routes 下载地址); 从 ali 执行 (与 switch-exit 一致, 本机可能到不了路由器)
    local gw="$1"
    # 注意 KexAlgorithms: 老 EdgeOS 设备需要 (本机 ~/.ssh/config 的 Host router 同款配置)
    ssh -o BatchMode=yes -o PasswordAuthentication=no -o ConnectTimeout=5 "$ALI_HOST" \
        "ssh -o BatchMode=yes -o ConnectTimeout=5 -o KexAlgorithms=+diffie-hellman-group1-sha1 unkstar@192.168.2.1 \"sudo sed -i -E 's|http://172\.31\.(30|40)\.2/routes/|http://${gw}/routes/|g' /config/scripts/update-routes.sh\"" \
        2>/dev/null || echo "  WARNING: 无法更新路由器 update-routes.sh (router 不可达?)"
}

do_switch() {  # $1 = mode: prod|fec
    local mode="$1" gw if peer subnet ali_ip
    if [ "$mode" = "fec" ]; then
        gw="$FEC_GW"; if="$FEC_ALI_IF"; peer="$FEC_TOKYO_PEER"; subnet="172.31.40.0/30"; ali_ip="$FEC_ALI_IP"
        echo "=== 切换到 fec 隧道 (RS FEC): $if / $gw ==="
    else
        gw="$PROD_GW"; if="$PROD_ALI_IF"; peer="$PROD_TOKYO_PEER"; subnet="172.31.30.0/30"; ali_ip="$PROD_ALI_IP"
        echo "=== 切换到生产嵌套隧道: $if / $gw ==="
    fi

    # 前置检查: 目标网关隧道连通性 (ali 视角)。必须 -I 显式指定源地址:
    # 裸 ping 的源地址由内核按接口主地址选择, 残留地址会导致误判 (2026-08-13
    # 生产 fec-tokyo 上的旧 172.31.32.1 曾让回切 ping 门禁误报失败)。
    echo "--- 检查隧道连通性 (ali ping $gw) ---"
    if ! ssh -o BatchMode=yes -o PasswordAuthentication=no -o ConnectTimeout=5 "$ALI_HOST" \
        "ping -I ${ali_ip} -c 1 -W 2 $gw >/dev/null 2>&1"; then
        echo "ERROR: 隧道不可达 ($gw), 放弃切换" >&2
        exit 1
    fi

    echo "--- 切换 ali 策略路由 ---"
    ali_switch "$gw" "$if"

    echo "--- 切换 tokyo 回程路由 + 子网伪装 ---"
    tokyo_switch "$peer" "$if" "$subnet"

    echo "--- 更新路由器 routes URL (best effort) ---"
    update_router_url "$gw"

    echo "--- 切换完成, 当前状态 ---"
    show_status
}

show_status() {
    echo "ali ip rule (ER-X):"
    ssh -o BatchMode=yes -o PasswordAuthentication=no -o ConnectTimeout=5 "$ALI_HOST" \
        "ip rule | grep 'from ${ERX_IP}' || echo '  (none)'" 2>/dev/null
    echo "ali table ${TABLE_ERX}:"
    ssh -o BatchMode=yes -o PasswordAuthentication=no -o ConnectTimeout=5 "$ALI_HOST" \
        "ip route show table ${TABLE_ERX} | grep default || echo '  (empty)'" 2>/dev/null
    echo "ali table ${TABLE_OVPN}:"
    ssh -o BatchMode=yes -o PasswordAuthentication=no -o ConnectTimeout=5 "$ALI_HOST" \
        "ip route show table ${TABLE_OVPN} | grep default || echo '  (empty)'" 2>/dev/null
    echo "tokyo 回程路由:"
    "${TOKYO_SSH[@]}" \
        "ip route show | grep -E '172.31.(16|17).0/24' || echo '  (none)'" 2>/dev/null
}

case "$MODE" in
    prod) do_switch prod ;;
    fec)  do_switch fec ;;
    status) show_status ;;
    *) echo "Usage: $0 {prod|fec|status}" >&2; exit 1 ;;
esac
