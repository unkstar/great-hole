#!/bin/bash
# Matrix test with loss patterns — runs on tokyo
# 8 algos × 6 patterns × 4 rates = 192 tests
echo 'REDACTED' | tee /tmp/pw > /dev/null
ssh ali "echo 'REDACTED' > /tmp/pw"

NAMES=("Static" "EWMA+Static" "EWMA+Dynamic" "PI" "MIMD" "Quantile" "BurstAware" "Gradient")
PNAMES=("" "Bernoulli" "Gilbert" "GElliott" "Sine" "Step" "CongWave")
RATES=(0.01 0.05 0.10 0.20)
PATTERNS=(1 2 3 4 5 6)
TOTAL=$((8 * 6 * 4))
DONE=0

echo "algo,name,pattern,pname,rate,sent_mbps,recv_mbps"

for algo in 0 1 2 3 4 5 6 7; do
for pat in ${PATTERNS[@]}; do
for rate in ${RATES[@]}; do
  DONE=$((DONE+1))
  name=${NAMES[$algo]}
  pname=${PNAMES[$pat]}
  echo -n "[$DONE/$TOTAL] $name $pname ${rate}: "

  # Kill old
  sudo -S < /tmp/pw pkill -9 great-hole 2>/dev/null || true
  ssh ali "sudo -S < /tmp/pw pkill -9 gh-fec 2>/dev/null" || true
  sleep 2

  # Ali config: loss pattern on decoder
  ssh ali "cat > /tmp/fec-tokyo-ali.lua << 'LUA'
local t_ip='202.144.195.145'
m=hole.udp_dyn_mux(11555);c=m:create_channel('0123456789abcdef',t_ip,11556);t=hole.tun('fec-tokyo')
local cfg={timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo,safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern=$pat,test_drop_rate=$rate,test_drop_rate2=0.005,test_drop_burst=8}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{},c,cfg,true,s);hole.fec_pipeline(c,{},t,cfg,false,s);hole.wait_for_exit()
LUA
sudo -S < /tmp/pw /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua > /tmp/fec.log 2>&1 &"

  # Tokyo config: no loss
  cat > /tmp/fec-tokyo.lua << LUA
local a_ip='39.108.136.48'
m=hole.udp_dyn_mux(11556);c=m:create_channel('0123456789abcdef',a_ip,11555);t=hole.tun('fec-tokyo')
local cfg={timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo,safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern=0,test_drop_rate=0.0}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{},c,cfg,true,s);hole.fec_pipeline(c,{},t,cfg,false,s);hole.wait_for_exit()
LUA
  sudo -S < /tmp/pw /home/ggcuser/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua > /tmp/fec.log 2>&1 &
  sleep 5

  # Tun setup
  sudo -S < /tmp/pw ip addr add 172.31.32.2 peer 172.31.32.1 dev fec-tokyo 2>/dev/null || true
  sudo -S < /tmp/pw ip link set fec-tokyo up 2>/dev/null || true
  ssh ali "sudo -S < /tmp/pw ip addr add 172.31.32.1 peer 172.31.32.2 dev fec-tokyo 2>/dev/null; sudo -S < /tmp/pw ip link set fec-tokyo up 2>/dev/null" || true

  # Wait negotiation
  OK=0
  for i in $(seq 1 10); do
    if ping -c1 -W2 172.31.32.1 >/dev/null 2>&1; then OK=1; break; fi
    sleep 1
  done

  if [ $OK -eq 0 ]; then
    echo "nego FAIL"
    echo "$algo,$name,$pat,$pname,$rate,-1,-1,nego" >> /tmp/matrix-results.csv
    continue
  fi

  # iperf3
  ssh ali "iperf3 -s -1 -B 172.31.32.1 >/dev/null 2>&1 &"
  sleep 1
  JSON=$(iperf3 -c 172.31.32.1 -t 10 -J 2>/dev/null || echo "{}")
  S=$(echo "$JSON" | python3 -c "import sys,json;d=json.load(sys.stdin);print(round(d['end']['sum_sent']['bits_per_second']/1e6,1))" 2>/dev/null || echo -1)
  R=$(echo "$JSON" | python3 -c "import sys,json;d=json.load(sys.stdin);print(round(d['end']['sum_received']['bits_per_second']/1e6,1))" 2>/dev/null || echo -1)
  echo "$S/$R Mbps"
  echo "$algo,$name,$pat,$pname,$rate,$S,$R,ok" >> /tmp/matrix-results.csv
done; done; done
echo "=== ALL DONE ==="
cat /tmp/matrix-results.csv
