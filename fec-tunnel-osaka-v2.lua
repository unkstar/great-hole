-- Osaka FEC Tunnel with direct UDP
local f = hole.filter_xor("test_fec_key_32_bytes_min_len!!!")
local u = hole.udp(25252)
local c = u:create_channel("172.17.60.123", 25253)
local o_enc = hole.tun("fec-out")
local o_dec = hole.tun("fec-in")
local fec_cfg = {
    timeout = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 4.0,
    symbol_size = 1400, mtu = 1500, max_batch = 200, obfuscate = false,
    iv_len = 4, decode_window = 64, ping_interval = 1000,
    feedback_timeout = 2000, feedback_stale_ms = 10000,
    ping_loss_threshold = 5, decode_timeout = 200,
}
local p_enc = hole.fec_pipeline(o_enc, {f}, c, fec_cfg, true)
local p_dec = hole.fec_pipeline(c, {f}, o_dec, fec_cfg, false)
print("Osaka FEC tunnel started")
hole.wait_for_exit()
p_dec:stop() p_enc:stop() o_dec:stop() o_enc:stop() c:stop() u:stop()
