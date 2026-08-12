-- Tokyo side: TEST-ONLY RS FEC tunnel via dyn_mux (PSK negotiation)
local test_port = 20086

m = hole.udp_dyn_mux(test_port)
-- active side: configured peer sends the first initiate; ali replies (passive)
c = m:create_channel("0123456789abcdef", "39.108.136.48", test_port)
t = hole.tun("fec-test")

local fec_cfg = {
    fec_codec = "rs",
    timeout_ms = 20,
    overhead = 0.03,
    max_overhead = 0.50,
    repeat_ratio = 4.0,
    symbol_size = 1440,
    mtu = 1420,
    max_batch = 20,
    obfuscate = false,
    iv_len = 4,
    decode_window = 64,
    ping_interval_ms = 1000,
    feedback_timeout_ms = 2000,
    feedback_stale_ms = 10000,
    ping_loss_threshold = 5,
    decode_timeout_ms = 200,
    algo = 1,
    safety_margin = 0.01,
    -- loss_deadband = -1 (禁用): 保留每批 m=1 的底补偿。理由 (2026-08-13 实测):
    -- TCP 为主时 88M payload + 5% repair ≈ 99M wire < 100M 网口, 底补偿无代价;
    -- 且单分片丢包对 TCP 完全隐形 (deadband=0 时丢包要等 ~1s 检测窗口, TCP 先吃
    -- dup-ACK)。5% 的真实代价只在饱和 UDP (95M 时超网口 ~5%), 非常态。
    loss_deadband = -1,
    loss_window_groups = 50,
    loss_alpha = 0.1,
}

local shared = hole.fec_shared_state()
p_enc = hole.fec_pipeline(t, {}, c, fec_cfg, true, shared)
p_dec = hole.fec_pipeline(c, {}, t, fec_cfg, false, shared)

print("Tokyo RS FEC-test dyn_mux started, port " .. test_port)

hole.wait_for_exit()
p_dec:stop()
p_enc:stop()
t:stop()
c:stop()
m:stop()
