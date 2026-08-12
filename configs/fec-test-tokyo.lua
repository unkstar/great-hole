-- Tokyo side: TEST-ONLY RS FEC tunnel, direct UDP (no dyn_mux negotiation)
local peer_ip = '39.108.136.48'
local test_port = 20086

u = hole.udp(test_port)
c = u:create_channel(peer_ip, test_port)
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
    -- loss_deadband: 本侧 encoder 管 T→A (dl) 方向。裸连 UDP 基准 0% (50M/100M 实测)
    -- → 0.5% 吸收 fb 量化噪声 (fb 步长 = 1/250 = 0.4%)。丢包 ≤ 0.5% 时不发 repair;
    -- 超过后立即恢复 m≥1 并随 EWMA 爬升。两侧可独立配置 (链路不对称)。
    loss_deadband = 0.005,
    loss_window_groups = 50,
    loss_alpha = 0.1,
}

local shared = hole.fec_shared_state()
p_enc = hole.fec_pipeline(t, {}, c, fec_cfg, true, shared)
p_dec = hole.fec_pipeline(c, {}, t, fec_cfg, false, shared)

print("Tokyo RS FEC-test direct UDP started, peer " .. peer_ip .. ":" .. test_port)

hole.wait_for_exit()
p_dec:stop()
p_enc:stop()
t:stop()
c:stop()
u:stop()
