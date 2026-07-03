#!/bin/bash
# FEC Regression Matrix — netns isolation for real FEC path
# Usage: ./regression-local.sh [algos] [patterns] [rates]
#
# Architecture:
#   Main ns: fec-a (10.100.1.1) + veth-a (192.168.100.1) + great-hole A (encoder)
#   Netns B: fec-b (10.100.1.2) + veth-b (192.168.100.2) + great-hole B (decoder+loss)
#   iperf3 client (main) → fec-a → FEC encode → DynMux/veth → FEC decode → fec-b → iperf3 server (netns)
#   tc netem on veth-a: 50ms delay, 100mbit rate

set -euo pipefail

ALGOS=(${1:-0 1 2 3 4 5 6 7})
PATTERNS=(${2:-1 2 3 4 5 6})
RATES=(${3:-0.01 0.05 0.10 0.20})
DURATION=10
BIN=/home/ggcuser/great-hole/build/src/great-hole
PW="REDACTED"

declare -A ANAME=([0]="Static" [1]="EWMA+Stat" [2]="EWMA+Dyn" [3]="PI" [4]="MIMD" [5]="Quantile" [6]="BurstAware" [7]="Gradient")
declare -A PNAME=([1]="Bernoulli" [2]="Gilbert" [3]="GElliott" [4]="Sine" [5]="Step" [6]="CongWave")

echo "$PW" | sudo -S true 2>/dev/null || { echo "sudo failed"; exit 1; }
echo "$PW" > /tmp/pw

RESULT_CSV="/tmp/regression.csv"
echo "algo,name,pattern,pname,rate,sent_mbps,recv_mbps,status" > "$RESULT_CSV"

TOTAL=$((${#ALGOS[@]} * ${#PATTERNS[@]} * ${#RATES[@]}))
DONE=0
NETNS="fecns"
NS="sudo ip netns exec $NETNS"

cleanup_all() {
    sudo pkill -9 great-hole 2>/dev/null || true
    $NS pkill -9 great-hole 2>/dev/null || true
    sleep 0.5
    sudo ip link del fec-a 2>/dev/null || true
    sudo ip link del fec-b 2>/dev/null || true
    sudo ip link del veth-a 2>/dev/null || true
    sudo ip netns del $NETNS 2>/dev/null || true
    sudo rm -f /tmp/fec-a.log /tmp/fec-b.log /tmp/fec-a.lua /tmp/fec-b.lua
}

setup_ns() {
    cleanup_all
    sleep 1

    sudo ip netns add $NETNS

    # veth pair for DynMux channel
    sudo ip link add veth-a type veth peer name veth-b
    sudo ip link set veth-b netns $NETNS
    sudo ip addr add 192.168.100.1/24 dev veth-a
    sudo ip link set veth-a up
    $NS ip addr add 192.168.100.2/24 dev veth-b
    $NS ip link set veth-b up

    # tc delay on veth-a: simulate ali↔tokyo (50ms one-way = 100ms RTT)
    sudo tc qdisc add dev veth-a root netem delay 50ms rate 100mbit 2>/dev/null || true

    # TUNs
    sudo ip tuntap add dev fec-a mode tun
    sudo ip addr add 10.100.1.1 peer 10.100.1.2 dev fec-a
    sudo ip link set fec-a up

    sudo ip tuntap add dev fec-b mode tun
    sudo ip link set fec-b netns $NETNS
    $NS ip addr add 10.100.1.2 peer 10.100.1.1 dev fec-b
    $NS ip link set fec-b up
    $NS ip link set lo up
}

# ==================== Main Loop ====================
for algo in "${ALGOS[@]}"; do
for pat in "${PATTERNS[@]}"; do
for rate in "${RATES[@]}"; do
  DONE=$((DONE + 1))
  aname="${ANAME[$algo]}"
  pname="${PNAME[$pat]}"

  # Static=15% baseline, adaptive=1% start (must ramp up)
  if [ $algo -eq 0 ]; then OH=0.15; else OH=0.01; fi

  echo -n "[$DONE/$TOTAL] a=$algo($aname) p=$pat($pname) r=$rate "

  setup_ns

  # Config A (main ns, encoder, NO loss injection)
  sudo tee /tmp/fec-a.lua > /dev/null << EOF
m=hole.udp_dyn_mux(22555)
c=m:create_channel('0123456789abcdef','192.168.100.2',22556)
t=hole.tun('fec-a')
local cfg={timeout_ms=4,overhead=$OH,max_overhead=0.50,repeat_ratio=1.0,repeat_ratio_min=0,repeat_ratio_max=3,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=200,feedback_timeout_ms=500,feedback_stale_ms=5000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo,safety_margin=0.01,loss_window_groups=10,loss_alpha=0.3,test_drop_pattern=0,test_drop_rate=0.0}
local s=hole.fec_shared_state()
hole.fec_pipeline(t,{},c,cfg,true,s)
hole.fec_pipeline(c,{},t,cfg,false,s)
hole.wait_for_exit()
EOF

  # Config B (netns, decoder, WITH loss injection)
  sudo tee /tmp/fec-b.lua > /dev/null << EOF
m=hole.udp_dyn_mux(22556)
c=m:create_channel('0123456789abcdef','192.168.100.1',22555)
t=hole.tun('fec-b')
local cfg={timeout_ms=4,overhead=$OH,max_overhead=0.50,repeat_ratio=1.0,repeat_ratio_min=0,repeat_ratio_max=3,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=200,feedback_timeout_ms=500,feedback_stale_ms=5000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo,safety_margin=0.01,loss_window_groups=10,loss_alpha=0.3,test_drop_pattern=$pat,test_drop_rate=$rate,test_drop_rate2=0.005,test_drop_burst=8}
local s=hole.fec_shared_state()
hole.fec_pipeline(c,{},t,cfg,false,s)
hole.fec_pipeline(t,{},c,cfg,true,s)
hole.wait_for_exit()
EOF

  # Start B in netns
  sudo rm -f /tmp/fec-b.log
  $NS $BIN --startlua /tmp/fec-b.lua &>/tmp/fec-b.log &
  sleep 2

  # Start A in main ns
  sudo rm -f /tmp/fec-a.log
  $BIN --startlua /tmp/fec-a.lua &>/tmp/fec-a.log &
  sleep 3

  # Fix tun peer
  sudo ip addr change 10.100.1.1 peer 10.100.1.2 dev fec-a 2>/dev/null || true
  $NS ip addr change 10.100.1.2 peer 10.100.1.1 dev fec-b 2>/dev/null || true

  # Negotiation
  OK=0
  for i in $(seq 1 15); do
    if ping -c1 -W1 10.100.1.2 >/dev/null 2>&1; then OK=1; break; fi
    sleep 1
  done
  if [ $OK -eq 0 ]; then
    echo "nego FAIL"
    echo "$algo,$aname,$pat,$pname,$rate,-1,-1,nego_fail" >> "$RESULT_CSV"
    continue
  fi

  # iperf3 TCP - FEC overhead directly affects effective throughput
  $NS iperf3 -s -1 -B 10.100.1.2 &>/dev/null &
  sleep 0.5

  JSON=$(timeout $((DURATION + 5)) iperf3 -c 10.100.1.2 -t "$DURATION" -J 2>/dev/null || echo "")
  if [ -z "$JSON" ]; then
    echo "iperf timeout"
    echo "$algo,$aname,$pat,$pname,$rate,-2,-2,timeout" >> "$RESULT_CSV"
    continue
  fi

  # Parse TCP: sum_sent and sum_received bits_per_second
  S=$(echo "$JSON" | python3 -c "
import sys,json
d=json.load(sys.stdin)
sent=round(d['end']['sum_sent']['bits_per_second']/1e6,1)
recv=round(d['end']['sum_received']['bits_per_second']/1e6,1)
print(f'{sent},{recv}')
" 2>/dev/null || echo "-3,-3")
  M=$(echo "$S" | cut -d, -f1)
  R=$(echo "$S" | cut -d, -f2)
  echo "${M}/${R} Mbps"
  echo "$algo,$aname,$pat,$pname,$rate,$M,$R,ok" >> "$RESULT_CSV"

done
done
done

cleanup_all
rm -f /tmp/pw

echo ""
echo "=== REGRESSION COMPLETE ==="
echo "Results: $RESULT_CSV"
echo ""

python3 << 'PY'
import csv
from collections import defaultdict
rows=[]
with open('/tmp/regression.csv') as f:
    for r in csv.DictReader(f):
        s=float(r['sent_mbps']); r2=float(r['recv_mbps'])
        rows.append((r['name'],r['pname'],float(r['rate']),s,r2))
ok=[r for r in rows if r[3]>0]
if not ok: print("No successful tests!"); exit(1)

print("=== Avg throughput by algorithm ===")
avgs=defaultdict(list)
for name,_,_,s,r in ok: avgs[name].append(r)
for name in sorted(avgs, key=lambda n: sum(avgs[n])/len(avgs[n]), reverse=True):
    vals=avgs[name]; print(f"  {name:>12}: avg={sum(vals)/len(vals):.1f} Mbps, n={len(vals)}")

print("\n=== Best per loss rate (by recv Mbps) ===")
for rate in [0.01,0.05,0.10,0.20]:
    best=max([r for r in ok if abs(r[2]-rate)<0.005], key=lambda r: r[4], default=None)
    if best: print(f"  Rate {rate:.0%}: {best[0]:>12} @ sent={best[3]:.1f} recv={best[4]:.1f} Mbps")
PY
