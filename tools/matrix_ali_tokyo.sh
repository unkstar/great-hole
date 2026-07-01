#!/bin/bash
# FEC Matrix Test: ali-tokyo tunnel
# Tests all 8 algorithms sequentially

set -e
PW="REDACTED"
ALI_IP=172.31.32.1
TOK_IP=172.31.32.2
RES=/tmp/fec-matrix-final.csv
echo "algo,name,sent_mbps,recv_mbps" > $RES

test_one() {
    local algo=$1
    local aname=$2

    echo "=== Testing algo=$algo ($aname) ==="

    # Stop both
    echo "$PW" | sudo -S pkill great-hole 2>/dev/null || true
    ssh ali "echo '$PW' | sudo -S pkill gh-fec 2>/dev/null" || true
    sleep 2

    # Write ali config
    ssh ali "cat > /tmp/fec-tokyo-ali.lua << 'LUA'
local tokyo_ip = '202.144.195.145'
m = hole.udp_dyn_mux(11555)
c = m:create_channel('0123456789abcdef', tokyo_ip, 11556)
tun_fec = hole.tun('fec-tokyo')
local fec_cfg = {
    timeout_ms = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 3.0,
    symbol_size = 1440, mtu = 1420, max_batch = 65535, obfuscate = false,
    iv_len = 4, decode_window = 64, ping_interval_ms = 1000,
    feedback_timeout_ms = 2000, feedback_stale_ms = 10000,
    ping_loss_threshold = 5, decode_timeout_ms = 200,
    algo = $algo,
}
local shared = hole.fec_shared_state()
p_enc = hole.fec_pipeline(tun_fec, {}, c, fec_cfg, true, shared)
p_dec = hole.fec_pipeline(c, {}, tun_fec, fec_cfg, false, shared)
hole.wait_for_exit()
p_dec:stop() p_enc:stop() tun_fec:stop() c:stop() m:stop()
LUA
echo 'ali config algo=$algo'" || { echo "$algo,$aname,-1,-1,cfg_fail" >> $RES; return; }

    # Write tokyo config
    cat > /tmp/fec-tokyo.lua << LUA
local ali_ip = '39.108.136.48'
m = hole.udp_dyn_mux(11556)
c = m:create_channel('0123456789abcdef', ali_ip, 11555)
tun_fec = hole.tun('fec-tokyo')
local fec_cfg = {
    timeout_ms = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 3.0,
    symbol_size = 1440, mtu = 1420, max_batch = 65535, obfuscate = false,
    iv_len = 4, decode_window = 64, ping_interval_ms = 1000,
    feedback_timeout_ms = 2000, feedback_stale_ms = 10000,
    ping_loss_threshold = 5, decode_timeout_ms = 200,
    algo = $algo,
}
local shared = hole.fec_shared_state()
p_enc = hole.fec_pipeline(tun_fec, {}, c, fec_cfg, true, shared)
p_dec = hole.fec_pipeline(c, {}, tun_fec, fec_cfg, false, shared)
hole.wait_for_exit()
p_dec:stop() p_enc:stop() tun_fec:stop() c:stop() m:stop()
LUA
    echo "tokyo config algo=$algo"

    # Start ali first, then tokyo
    ssh ali "echo '$PW' | sudo -S nohup /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua > /tmp/fec-tokyo-ali.log 2>&1 &" &
    sleep 1
    echo "$PW" | sudo -S nohup ~/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua > /tmp/fec-tokyo.log 2>&1 &
    sleep 4

    # Setup tun
    echo "$PW" | sudo -S ip addr add $TOK_IP peer $ALI_IP dev fec-tokyo 2>/dev/null || true
    echo "$PW" | sudo -S ip link set fec-tokyo up 2>/dev/null || true
    ssh ali "echo '$PW' | sudo -S ip addr add $ALI_IP peer $TOK_IP dev fec-tokyo 2>/dev/null; echo '$PW' | sudo -S ip link set fec-tokyo up 2>/dev/null" || true

    # Wait for negotiation
    for i in $(seq 1 10); do
        if ping -c 1 -W 2 $ALI_IP > /dev/null 2>&1; then
            echo "tunnel up after ${i}s"
            break
        fi
        sleep 1
    done

    if ! ping -c 1 -W 2 $ALI_IP > /dev/null 2>&1; then
        echo "$algo,$aname,-1,-1,nego_fail" >> $RES
        return
    fi

    # iperf3 TCP test (single call, JSON)
    ssh ali "iperf3 -s -1 -B $ALI_IP --json > /dev/null 2>&1 &" &
    sleep 1

    JSON=$(iperf3 -c $ALI_IP -t 10 -P 2 -J 2>/dev/null)
    SENT=$(echo "$JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['end']['sum_sent']['bits_per_second']/1e6)" 2>/dev/null || echo "0")
    RECV=$(echo "$JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['end']['sum_received']['bits_per_second']/1e6)" 2>/dev/null || echo "0")

    echo "$algo,$aname,${SENT:-0},${RECV:-0}" >> $RES
    echo "  -> sent=${SENT:-0}Mbps recv=${RECV:-0}Mbps"
}

# Run all algorithms
for algo in 0 1 2 3 4 5 6 7; do
    case $algo in
        0) name="Static" ;;
        1) name="EWMA+Static" ;;
        2) name="EWMA+Dynamic" ;;
        3) name="PI" ;;
        4) name="MIMD" ;;
        5) name="Quantile" ;;
        6) name="BurstAware" ;;
        7) name="Gradient" ;;
    esac
    test_one $algo "$name"
done

echo "=== FINAL RESULTS ==="
cat $RES
