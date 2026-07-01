-- Ali side: FEC hole to tokyo
-- Run via: /usr/local/bin/gh-fec2 --startlua /tmp/fec-tokyo-ali.lua
-- Uses separate tun interface and ports from production

local tokyo_ip = '202.144.195.145'
local ali_port = 11555
local tokyo_port = 11556

m = hole.udp_dyn_mux(ali_port)
c = m:create_channel("0123456789abcdef", tokyo_ip, tokyo_port)
tun_fec = hole.tun("fec-tokyo")

local fec_cfg = {
    timeout_ms = 4, overhead = 0.5, max_overhead = 0.50, repeat_ratio = 3.0,
    symbol_size = 1440, mtu = 1420, max_batch = 65535, obfuscate = false,
    iv_len = 4, decode_window = 64, ping_interval_ms = 1000,
    feedback_timeout_ms = 2000, feedback_stale_ms = 10000,
    ping_loss_threshold = 5, decode_timeout_ms = 200,
}

local shared = hole.fec_shared_state()
p_enc = hole.fec_pipeline(tun_fec, {}, c, fec_cfg, true, shared)
p_dec = hole.fec_pipeline(c, {}, tun_fec, fec_cfg, false, shared)

print("Ali FEC-tokyo test started on port " .. ali_port)

hole.wait_for_exit()

p_dec:stop() p_enc:stop() tun_fec:stop() c:stop() m:stop()
