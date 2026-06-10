-- Osaka FEC over hole tunnel
local f = hole.filter_xor("603A71E267F8418CB2EA9FF05DF05B49EFA4780D865C4CA88C55CF3E31E7F5A6AE03C8BF8E914E81938E8095430EA7DC")
local m = hole.udp_dyn_mux(14176)
local c = m:create_channel("0123456789abcdef", "127.0.0.1", 14176)
local o = hole.tun("hole")
local fec_cfg = {
    timeout = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 4.0,
    symbol_size = 1400, mtu = 1500, max_batch = 200, obfuscate = true,
    iv_len = 4, decode_window = 64, ping_interval = 1000,
    feedback_timeout = 2000, feedback_stale_ms = 10000,
    ping_loss_threshold = 5, decode_timeout = 200,
}
local p_dec = hole.fec_pipeline(c, {f}, o, fec_cfg, false)
local p_enc = hole.fec_pipeline(o, {f}, c, fec_cfg, true)
print("Osaka FEC hole tunnel started")
hole.wait_for_exit()
p_enc:stop() p_dec:stop() o:stop() c:stop() m:stop()
