#!/bin/bash
# Matrix test: 8 algos, 15s x 4 streams
NAMES=("Static" "EWMA+Static" "EWMA+Dynamic" "PI" "MIMD" "Quantile" "BurstAware" "Gradient")
echo "algo,name,sent_mbps,recv_mbps"

for algo in 0 1 2 3 4 5 6 7; do
  name=${NAMES[$algo]}

  # Kill old (single-quoted password for literal $)
  echo 'REDACTED' | sudo -S pkill great-hole 2>/dev/null
  ssh ali 'echo REDACTED | sudo -S pkill gh-fec 2>/dev/null'
  sleep 2

  # Ali config (heredoc preserves literal content)
  ssh ali "cat > /tmp/fec-tokyo-ali.lua << 'LUA'
local tokyo_ip = '202.144.195.145'
m = hole.udp_dyn_mux(11555)
c = m:create_channel('0123456789abcdef', tokyo_ip, 11556)
t = hole.tun('fec-tokyo')
local cfg = {timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo}
local s=hole.fec_shared_state()
hole.fec_pipeline(t,{},c,cfg,true,s)
hole.fec_pipeline(c,{},t,cfg,false,s)
hole.wait_for_exit()
LUA"

  # Tokyo config ($algo expanded by bash, rest is literal)
  cat > /tmp/fec-tokyo.lua << LUA
local ali_ip = '39.108.136.48'
m = hole.udp_dyn_mux(11556)
c = m:create_channel('0123456789abcdef', ali_ip, 11555)
t = hole.tun('fec-tokyo')
local cfg = {timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=$algo}
local s=hole.fec_shared_state()
hole.fec_pipeline(t,{},c,cfg,true,s)
hole.fec_pipeline(c,{},t,cfg,false,s)
hole.wait_for_exit()
LUA

  # Start instances
  ssh ali 'echo REDACTED | sudo -S /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua > /tmp/fec-tokyo-ali.log 2>&1 &'
  sleep 1
  echo 'REDACTED' | sudo -S ~/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua > /tmp/fec-tokyo.log 2>&1 &
  sleep 5

  # Setup tun interfaces
  echo 'REDACTED' | sudo -S ip addr add 172.31.32.2 peer 172.31.32.1 dev fec-tokyo 2>/dev/null
  echo 'REDACTED' | sudo -S ip link set fec-tokyo up 2>/dev/null
  ssh ali 'echo REDACTED | sudo -S ip addr add 172.31.32.1 peer 172.31.32.2 dev fec-tokyo 2>/dev/null; echo REDACTED | sudo -S ip link set fec-tokyo up 2>/dev/null'

  # Wait for tunnel negotiation
  OK=0
  for i in $(seq 1 12); do
    ping -c1 -W2 172.31.32.1 >/dev/null 2>&1 && { OK=1; break; }
    sleep 1
  done
  if [ $OK -eq 0 ]; then echo "$algo,$name,-1,-1,nego"; continue; fi

  # iperf3 TCP test (15s, 4 streams)
  ssh ali 'iperf3 -s -1 -B 172.31.32.1 >/dev/null 2>&1 &'
  sleep 1
  JSON=$(iperf3 -c 172.31.32.1 -t 15 -P 4 -J 2>/dev/null)
  S=$(echo "$JSON" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(f"{d[\"end\"][\"sum_sent\"][\"bits_per_second\"]/1e6:.1f}")' 2>/dev/null || echo 0)
  R=$(echo "$JSON" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(f"{d[\"end\"][\"sum_received\"][\"bits_per_second\"]/1e6:.1f}")' 2>/dev/null || echo 0)
  echo "$algo,$name,${S:-0},${R:-0}"
done
echo "=== DONE ==="
