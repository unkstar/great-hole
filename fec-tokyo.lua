-- Tokyo side: FEC hole to ali
-- Run via: ~/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua

local ali_ip = '39.108.136.48'
local ali_port = 11555
local tokyo_port = 11556

m = hole.udp_dyn_mux(tokyo_port)
c = m:create_channel("0123456789abcdef", ali_ip, ali_port)
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

print("Tokyo FEC test started on port " .. tokyo_port)

hole.wait_for_exit()

p_dec:stop() p_enc:stop() tun_fec:stop() c:stop() m:stop()
