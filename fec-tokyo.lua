-- Tokyo side: FEC tunnel to Ali
-- great-hole-fec --startlua /etc/great-hole/fec/tokyo.lua

local ali_ip = '39.108.136.48'
local ali_port = 11555
local tokyo_port = 11556

m = hole.udp_dyn_mux(tokyo_port)
c = m:create_channel("0123456789abcdef", ali_ip, ali_port)
tun_fec = hole.tun("fec-tokyo")

local fec_cfg = {
    -- Core FEC
    timeout_ms = 4,
    overhead = 0.01,           -- PI starts from 1% base
    max_overhead = 0.50,
    repeat_ratio = 4.0,
    symbol_size = 1440,
    mtu = 1420,
    max_batch = 200,
    obfuscate = true,
    iv_len = 4,
    decode_window = 64,

    -- PING / RTT / Feedback
    ping_interval_ms = 1000,
    feedback_timeout_ms = 2000,
    feedback_stale_ms = 10000,
    ping_loss_threshold = 5,
    decode_timeout_ms = 200,

    -- PI Controller (best adaptive algorithm)
    algo = 3,
    safety_margin = 0.01,
    loss_window_groups = 50,
    loss_alpha = 0.1,
}

local shared = hole.fec_shared_state()
p_enc = hole.fec_pipeline(tun_fec, {}, c, fec_cfg, true, shared)
p_dec = hole.fec_pipeline(c, {}, tun_fec, fec_cfg, false, shared)

print("Tokyo FEC tunnel started on port " .. tokyo_port .. " (PI algo, overhead=1% base)")

hole.wait_for_exit()

p_dec:stop()
p_enc:stop()
tun_fec:stop()
c:stop()
m:stop()
