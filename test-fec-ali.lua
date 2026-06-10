-- Ali side: listen on port 11451, send to osaka
-- Run on ali machine

local ali_port = 11451
local osaka_port = 25252  -- osaka receives here

u1 = hole.udp(ali_port)

-- Create channel to osaka
local c_osaka = u1:create_channel('osaka', osaka_port)

-- FEC config
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

local f = hole.filter_xor('test_fec_key_32_bytes_min_len')

local p_send = hole.fec_pipeline(c_osaka, {f}, c_osaka, fec_cfg, true)  -- encode
local p_recv = hole.fec_pipeline(c_osaka, {f}, c_osaka, fec_cfg, false) -- decode

print('Ali FEC pipeline started on port ' .. ali_port)

hole.wait_for_exit()

p_recv:stop()
p_send:stop()
c_osaka:stop()
u1:stop()
