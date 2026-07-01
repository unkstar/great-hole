#!/bin/bash
set -x
PW="REDACTED"
echo "$PW" | sudo -S pkill -9 -f 'fec-a.lua|fec-b.lua' 2>/dev/null
echo "$PW" | sudo -S ip netns del fecns 2>/dev/null
echo "$PW" | sudo -S ip link del fec-a 2>/dev/null
echo "$PW" | sudo -S ip link del fec-b 2>/dev/null
sleep 1
echo "$PW" | sudo -S ip netns add fecns

# A config
cat > /tmp/fec-a.lua << 'LUAEOF'
m = hole.udp_dyn_mux(21000)
c = m:create_channel('0123456789abcdef', '127.0.0.1', 21001)
t = hole.tun('fec-a')
local cfg = {timeout_ms = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 3.0, symbol_size = 1440, mtu = 1420, max_batch = 65535, obfuscate = false, iv_len = 4, decode_window = 64, ping_interval_ms = 500, feedback_timeout_ms = 1000, feedback_stale_ms = 10000, ping_loss_threshold = 5, decode_timeout_ms = 200, algo = 0, test_drop_pattern = 0, test_drop_rate = 0.0, test_drop_rate2 = 0.0, test_drop_burst = 1}
local sh = hole.fec_shared_state()
p_enc = hole.fec_pipeline(t, {}, c, cfg, true, sh)
p_dec = hole.fec_pipeline(c, {}, t, cfg, false, sh)
print('A ready')
hole.wait_for_exit()
p_dec:stop() p_enc:stop() t:stop() c:stop() m:stop()
LUAEOF

echo "$PW" | sudo -S ip tuntap add dev fec-a mode tun
echo "$PW" | sudo -S ip addr add 10.99.1.1 peer 10.99.1.2 dev fec-a
echo "$PW" | sudo -S ip link set fec-a up

# B config
cat > /tmp/fec-b.lua << 'LUAEOF'
m = hole.udp_dyn_mux(21001)
c = m:create_channel('0123456789abcdef', '127.0.0.1', 21000)
t = hole.tun('fec-b')
local cfg = {timeout_ms = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 3.0, symbol_size = 1440, mtu = 1420, max_batch = 65535, obfuscate = false, iv_len = 4, decode_window = 64, ping_interval_ms = 500, feedback_timeout_ms = 1000, feedback_stale_ms = 10000, ping_loss_threshold = 5, decode_timeout_ms = 200, algo = 0, test_drop_pattern = 1, test_drop_rate = 0.01, test_drop_rate2 = 0.0, test_drop_burst = 1}
local sh = hole.fec_shared_state()
p_enc = hole.fec_pipeline(t, {}, c, cfg, true, sh)
p_dec = hole.fec_pipeline(c, {}, t, cfg, false, sh)
print('B ready')
hole.wait_for_exit()
p_dec:stop() p_enc:stop() t:stop() c:stop() m:stop()
LUAEOF

echo "$PW" | sudo -S ip tuntap add dev fec-b mode tun
echo "$PW" | sudo -S ip link set fec-b netns fecns
echo "$PW" | sudo -S ip netns exec fecns ip addr add 10.99.1.2 peer 10.99.1.1 dev fec-b
echo "$PW" | sudo -S ip netns exec fecns ip link set fec-b up
echo "$PW" | sudo -S ip netns exec fecns ip link set lo up

echo "$PW" | sudo -S nohup /home/ggcuser/great-hole/build/src/great-hole --startlua /tmp/fec-a.lua > /tmp/fec-a.log 2>&1 &
echo "$PW" | sudo -S ip netns exec fecns nohup /home/ggcuser/great-hole/build/src/great-hole --startlua /tmp/fec-b.lua > /tmp/fec-b.log 2>&1 &

sleep 5
echo "=== A log ==="
tail -5 /tmp/fec-a.log
echo "=== B log ==="
tail -5 /tmp/fec-b.log
echo "=== ping ==="
ping -c 3 -W 2 10.99.1.2
