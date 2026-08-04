-- Tokyo side: TEST-ONLY FEC tunnel from Ali (port 20086, direct UDP, no speederv2)
-- Production nested tunnel uses 10086/10087 — do not touch.
-- great-hole-fec --startlua /etc/great-hole/fec/fec-test-tokyo.lua

local test_port = 20086

m = hole.udp_dyn_mux(test_port)
c = m:create_channel("0123456789abcdef")
tun_fec = hole.tun("fec-test")

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

print("Tokyo FEC-test from Ali started on port " .. test_port .. " (PI algo, overhead=1% base)")

hole.wait_for_exit()

p_dec:stop()
p_enc:stop()
tun_fec:stop()
c:stop()
m:stop()
