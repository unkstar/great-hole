-- Osaka side: listen on 25252, send to ali (172.31.26.26):11451
local ali_ip = '172.31.26.26'
local ali_port = 11451
local osaka_port = 25252

-- Use separate local ports for send/recv to avoid channel sharing
local u_recv = hole.udp(25252)
local u_send = hole.udp(25253)

local c_out = u_send:create_channel(ali_ip, 11451)
local c_in = u_recv:create_channel(ali_ip, 11452)

local fec_cfg = {
    timeout = 4,
    overhead = 0.15,
    max_overhead = 0.50,
    repeat_ratio = 4.0,
    symbol_size = 1400,
    mtu = 1500,
    max_batch = 200,
    obfuscate = false,
    iv_len = 4,
    decode_window = 64,
    ping_interval = 1000,
    feedback_timeout = 2000,
    feedback_stale_ms = 10000,
    ping_loss_threshold = 5,
    decode_timeout = 200,
}

local f = hole.filter_xor('test_fec_key_32_bytes_min_len!!!')

local p_send = hole.fec_pipeline(c_in, {f}, c_out, fec_cfg, true)
local p_recv = hole.fec_pipeline(c_out, {f}, c_in, fec_cfg, false)

print('Osaka FEC pipeline started on port ' .. osaka_port)

hole.wait_for_exit()

p_recv:stop()
p_send:stop()
c_in:stop()
c_out:stop()
u_recv:stop()
u_send:stop()
