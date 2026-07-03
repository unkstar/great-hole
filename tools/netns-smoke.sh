#!/bin/bash
set -e
PW="REDACTED"
BIN=/home/ggcuser/great-hole/build/src/great-hole

echo "$PW" | sudo -S true

# Cleanup
sudo pkill -9 great-hole 2>/dev/null || true
sudo ip netns exec fecns pkill -9 great-hole 2>/dev/null || true
sudo ip link del fec-a 2>/dev/null || true
sudo ip link del fec-b 2>/dev/null || true
sudo ip netns del fecns 2>/dev/null || true
sleep 1

# Create netns
sudo ip netns add fecns

# veth pair: DynMux channel between namespaces
sudo ip link add veth-a type veth peer name veth-b
sudo ip link set veth-b netns fecns
sudo ip addr add 192.168.100.1/24 dev veth-a
sudo ip link set veth-a up
sudo ip netns exec fecns ip addr add 192.168.100.2/24 dev veth-b
sudo ip netns exec fecns ip link set veth-b up

# tc delay on veth-a (simulates ali↔tokyo latency)
sudo tc qdisc add dev veth-a root netem delay 50ms rate 100mbit 2>/dev/null || true

# fec-a (main ns, TUN for iperf3 traffic)
sudo ip tuntap add dev fec-a mode tun
sudo ip addr add 10.100.1.1 peer 10.100.1.2 dev fec-a
sudo ip link set fec-a up

# fec-b (netns, TUN for iperf3 traffic)
sudo ip tuntap add dev fec-b mode tun
sudo ip link set fec-b netns fecns
sudo ip netns exec fecns ip addr add 10.100.1.2 peer 10.100.1.1 dev fec-b
sudo ip netns exec fecns ip link set fec-b up
sudo ip netns exec fecns ip link set lo up

# Config A (encoder, main ns, no loss)
sudo tee /tmp/fec-a.lua > /dev/null << LUA
m=hole.udp_dyn_mux(22555)
c=m:create_channel('0123456789abcdef','192.168.100.2',22556)
t=hole.tun('fec-a')
local cfg={timeout_ms=4,overhead=0.01,max_overhead=0.50,repeat_ratio=1.0,repeat_ratio_min=0,repeat_ratio_max=3,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=200,feedback_timeout_ms=500,feedback_stale_ms=5000,ping_loss_threshold=5,decode_timeout_ms=200,algo=0,safety_margin=0.01,loss_window_groups=10,loss_alpha=0.3,test_drop_pattern=0,test_drop_rate=0.0}
local s=hole.fec_shared_state()
hole.fec_pipeline(t,{},c,cfg,true,s)
hole.fec_pipeline(c,{},t,cfg,false,s)
hole.wait_for_exit()
LUA

# Config B (decoder, netns, Bernoulli 20% loss)
sudo tee /tmp/fec-b.lua > /dev/null << LUA
m=hole.udp_dyn_mux(22556)
c=m:create_channel('0123456789abcdef','192.168.100.1',22555)
t=hole.tun('fec-b')
local cfg={timeout_ms=4,overhead=0.01,max_overhead=0.50,repeat_ratio=1.0,repeat_ratio_min=0,repeat_ratio_max=3,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=200,feedback_timeout_ms=500,feedback_stale_ms=5000,ping_loss_threshold=5,decode_timeout_ms=200,algo=0,safety_margin=0.01,loss_window_groups=10,loss_alpha=0.3,test_drop_pattern=1,test_drop_rate=0.20,test_drop_rate2=0.005,test_drop_burst=8}
local s=hole.fec_shared_state()
hole.fec_pipeline(c,{},t,cfg,false,s)
hole.fec_pipeline(t,{},c,cfg,true,s)
hole.wait_for_exit()
LUA

# Start B in netns
sudo rm -f /tmp/fec-b.log /tmp/fec-a.log
sudo ip netns exec fecns $BIN --startlua /tmp/fec-b.lua &>/tmp/fec-b.log &
sleep 2

# Start A in main ns
$BIN --startlua /tmp/fec-a.lua &>/tmp/fec-a.log &
sleep 3

# Fix peer
sudo ip addr change 10.100.1.1 peer 10.100.1.2 dev fec-a 2>/dev/null || true
sudo ip netns exec fecns ip addr change 10.100.1.2 peer 10.100.1.1 dev fec-b 2>/dev/null || true

# Wait for negotiation
echo -n "Negotiating..."
for i in $(seq 1 20); do
  if ping -c1 -W1 10.100.1.2 >/dev/null 2>&1; then echo " OK (${i}s)"; break; fi
  sleep 1
done

# iperf3 UDP
sudo ip netns exec fecns sudo -u ggcuser iperf3 -s -1 -B 10.100.1.2 &>/dev/null &
sleep 0.5
echo "--- iperf3 UDP (Static, 1% overhead, Bernoulli 20% loss) ---"
iperf3 -c 10.100.1.2 -u -b 90M -t 5 -J 2>/dev/null | python3 -c "
import sys,json
d=json.load(sys.stdin)
mbps=round(d['end']['sum']['bits_per_second']/1e6,1)
loss=round(d['end']['sum']['lost_percent'],1)
print(f'Throughput: {mbps} Mbps')
print(f'UDP Loss:  {loss}%')
"

echo "--- Decoder K values (should be >1 if batch working) ---"
sudo grep 'decoded group' /tmp/fec-b.log 2>/dev/null | tail -8

echo "--- Decoder failures ---"
sudo grep -c 'decode failed' /tmp/fec-b.log 2>/dev/null && echo "failures found!" || echo "none"

# Cleanup
sudo pkill -9 great-hole 2>/dev/null || true
sudo ip netns exec fecns pkill -9 great-hole 2>/dev/null || true
sudo ip link del fec-a 2>/dev/null || true
sudo ip netns del fecns 2>/dev/null || true
