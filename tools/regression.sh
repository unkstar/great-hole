#!/bin/bash
# FEC Regression Matrix — runs on tokyo
# Usage: ./regression.sh [algos] [patterns] [rates]
# Default: all 8 algos, all 6 patterns, 4 rates (1%,5%,10%,20%)
#
# Config: edit the arrays below to customize test scope.
# Results: /tmp/regression.csv

# set -euo pipefail  # disabled: pkill returns non-zero when no process found

# === Test Matrix Configuration ===
ALGOS=(${1:-0 1 2 3 4 5 6 7})
PATTERNS=(${2:-1 2 3 4 5 6})
RATES=(${3:-0.01 0.05 0.10 0.20})
DURATION=10  # iperf3 test seconds

# === Names ===
declare -A ANAME=([0]="Static" [1]="EWMA+Stat" [2]="EWMA+Dyn" [3]="PI" [4]="MIMD" [5]="Quantile" [6]="BurstAware" [7]="Gradient")
declare -A PNAME=([1]="Bernoulli" [2]="Gilbert" [3]="GElliott" [4]="Sine" [5]="Step" [6]="CongWave")

# === Password setup ===
PW="REDACTED"
echo "$PW" | sudo -S true 2>/dev/null || { echo "sudo failed"; exit 1; }
echo "$PW" > /tmp/pw
ssh ali "cat > /tmp/pw" < /tmp/pw
ssh ali "sudo -S < /tmp/pw true" 2>/dev/null || { echo "ssh ali failed"; exit 1; }

# === Output ===
RESULT_CSV="/tmp/regression.csv"
echo "algo,name,pattern,pname,rate,sent_mbps,recv_mbps" > "$RESULT_CSV"
TOTAL=$((${#ALGOS[@]} * ${#PATTERNS[@]} * ${#RATES[@]}))
DONE=0

# === Main Loop ===
for algo in "${ALGOS[@]}"; do
for pat in "${PATTERNS[@]}"; do
for rate in "${RATES[@]}"; do
  DONE=$((DONE + 1))
  aname="${ANAME[$algo]}"
  pname="${PNAME[$pat]}"

  echo -n "[$DONE/$TOTAL] a=$algo($aname) p=$pat($pname) r=$rate "

  # --- Kill old processes ---
  sudo -S < /tmp/pw pkill -9 great-hole 2>/dev/null || true
  ssh ali "sudo -S < /tmp/pw pkill -9 gh-fec 2>/dev/null" || true
  sleep 2

  # --- Write ali config (heredoc from bash, no Python needed) ---
  ssh ali "cat > /tmp/fec-tokyo-ali.lua" << ALIEOF
local t_ip='202.144.195.145'
m=hole.udp_dyn_mux(11555);c=m:create_channel('0123456789abcdef',t_ip,11556);t=hole.tun('fec-tokyo')
local cfg={timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo,safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern=$pat,test_drop_rate=$rate,test_drop_rate2=0.005,test_drop_burst=8}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{},c,cfg,true,s);hole.fec_pipeline(c,{},t,cfg,false,s);hole.wait_for_exit()
ALIEOF

  # --- Write tokyo config ---
  cat > /tmp/fec-tokyo.lua << TOKEOF
local a_ip='39.108.136.48'
m=hole.udp_dyn_mux(11556);c=m:create_channel('0123456789abcdef',a_ip,11555);t=hole.tun('fec-tokyo')
local cfg={timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo,safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern=0,test_drop_rate=0.0}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{},c,cfg,true,s);hole.fec_pipeline(c,{},t,cfg,false,s);hole.wait_for_exit()
TOKEOF

  # --- Start ali ---
  ssh ali "sudo -S < /tmp/pw /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua &>/tmp/fec-ali.log &"
  sleep 1

  # --- Start tokyo ---
  sudo -bS < /tmp/pw /home/ggcuser/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua &>/tmp/fec-tok.log
  sleep 5

  # --- Fix tun peer (systemd-networkd assigns IP but not peer for tun devices) ---
  sudo -S < /tmp/pw ip addr change 172.31.32.2 peer 172.31.32.1 dev fec-tokyo 2>/dev/null || true
  ssh ali "sudo -S < /tmp/pw ip addr change 172.31.32.1 peer 172.31.32.2 dev fec-tokyo 2>/dev/null" || true

  # --- Wait for tunnel negotiation ---
  OK=0
  for i in $(seq 1 10); do
    if ping -c1 -W2 172.31.32.1 >/dev/null 2>&1; then OK=1; break; fi
    sleep 1
  done
  if [ $OK -eq 0 ]; then
    echo "nego FAIL"
    echo "$algo,$aname,$pat,$pname,$rate,-1,-1,nego" >> "$RESULT_CSV"
    continue
  fi

  # --- iperf3 ---
  ssh ali "iperf3 -s -1 -B 172.31.32.1 >/dev/null 2>&1 &"
  sleep 1

  JSON=$(timeout $((DURATION + 5)) iperf3 -c 172.31.32.1 -t "$DURATION" -J 2>/dev/null || echo "")
  if [ -z "$JSON" ]; then
    echo "iperf timeout"
    echo "$algo,$aname,$pat,$pname,$rate,-2,-2,timeout" >> "$RESULT_CSV"
    continue
  fi

  S=$(echo "$JSON" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(round(d["end"]["sum_sent"]["bits_per_second"]/1e6,1))' 2>/dev/null || echo -2)
  R=$(echo "$JSON" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(round(d["end"]["sum_received"]["bits_per_second"]/1e6,1))' 2>/dev/null || echo -2)

  echo "${S}/${R} Mbps"
  echo "$algo,$aname,$pat,$pname,$rate,$S,$R,ok" >> "$RESULT_CSV"

done
done
done

echo "=== REGRESSION COMPLETE ==="
echo "Results: $RESULT_CSV"
echo ""
echo "=== Quick summary (avg by algo, excluding failures) ==="
python3 << 'PY'
import csv
rows = []
with open('/tmp/regression.csv') as f:
    for r in csv.DictReader(f):
        s = float(r['sent_mbps'])
        if s > 0:
            rows.append((r['name'], s))
from collections import defaultdict
avgs = defaultdict(list)
for name, s in rows:
    avgs[name].append(s)
for name in sorted(avgs, key=lambda n: sum(avgs[n])/len(avgs[n]), reverse=True):
    vals = avgs[name]
    print(f"  {name:>12}: avg={sum(vals)/len(vals):.1f} Mbps, n={len(vals)}")
PY
