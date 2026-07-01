#!/usr/bin/env python3
"""Matrix test: 8 algos on ali-tokyo FEC tunnel."""
import subprocess, time, json

PW = "REDACTED"
ALI = "ali"
RES = "/tmp/fec-matrix-final.csv"
DUR = 15

NAMES = ["Static","EWMA+Static","EWMA+Dynamic","PI","MIMD","Quantile","BurstAware","Gradient"]

def sh(cmd):
    subprocess.run(cmd, shell=True, capture_output=True)

def sali(cmd):
    # Use double quotes for SSH to avoid single-quote escaping issues
    escaped = cmd.replace('"', '\\"')
    subprocess.run(f'ssh {ALI} "{escaped}"', shell=True, capture_output=True)

def lua_config(algo, is_ali=True):
    ip = "202.144.195.145" if is_ali else "39.108.136.48"
    port_self = 11555 if is_ali else 11556
    port_peer = 11556 if is_ali else 11555
    return f"""m = hole.udp_dyn_mux({port_self})
c = m:create_channel('0123456789abcdef', '{ip}', {port_peer})
t = hole.tun('fec-tokyo')
local cfg = {{
    timeout_ms = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 3.0,
    symbol_size = 1440, mtu = 1420, max_batch = 65535, obfuscate = false,
    iv_len = 4, decode_window = 64, ping_interval_ms = 1000,
    feedback_timeout_ms = 2000, feedback_stale_ms = 10000,
    ping_loss_threshold = 5, decode_timeout_ms = 200,
    algo = {algo},
}}
local s = hole.fec_shared_state()
p_enc = hole.fec_pipeline(t, {{}}, c, cfg, true, s)
p_dec = hole.fec_pipeline(c, {{}}, t, cfg, false, s)
hole.wait_for_exit()
p_dec:stop() p_enc:stop() t:stop() c:stop() m:stop()
"""

results = []
print("algo,name,sent_mbps,recv_mbps")

for algo in range(8):
    name = NAMES[algo]

    # Kill old
    sh(f"echo '{PW}' | sudo -S pkill great-hole 2>/dev/null")
    sali(f"echo '{PW}' | sudo -S pkill gh-fec 2>/dev/null")
    time.sleep(2)

    # Write configs (using Python file I/O via ssh)
    ali_cfg = lua_config(algo, is_ali=True)
    tok_cfg = lua_config(algo, is_ali=False)

    # Write ali config via ssh
    escaped = ali_cfg.replace("'", "'\\''")
    sali(f"cat > /tmp/fec-tokyo-ali.lua << 'PYEOF'\n{ali_cfg}\nPYEOF")

    # Write tokyo config locally
    with open("/tmp/fec-tokyo.lua", "w") as f:
        f.write(tok_cfg)

    # Start both
    sali(f"echo '{PW}' | sudo -S nohup /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua > /tmp/fec-tokyo-ali.log 2>&1 &")
    time.sleep(0.5)
    sh(f"echo '{PW}' | sudo -S nohup /home/ggcuser/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua > /tmp/fec-tokyo.log 2>&1 &")
    time.sleep(4)

    # Setup tun (use separate commands to avoid quoting issues)
    sh(f"echo '{PW}' | sudo -S ip addr add 172.31.32.2 peer 172.31.32.1 dev fec-tokyo 2>/dev/null")
    sh(f"echo '{PW}' | sudo -S ip link set fec-tokyo up 2>/dev/null")
    sali(f"echo '{PW}' | sudo -S ip addr add 172.31.32.1 peer 172.31.32.2 dev fec-tokyo 2>/dev/null")
    sali(f"echo '{PW}' | sudo -S ip link set fec-tokyo up 2>/dev/null")

    # Wait for negotiation
    ok = False
    for i in range(10):
        r = subprocess.run(["ping", "-c1", "-W2", "172.31.32.1"], capture_output=True)
        if r.returncode == 0:
            ok = True
            break
        time.sleep(1)

    if not ok:
        print(f"{algo},{name},-1,-1,nego_fail")
        results.append((algo, name, -1, -1))
        continue

    # iperf3
    sali(f"iperf3 -s -1 -B 172.31.32.1 >/dev/null 2>&1 &")
    time.sleep(0.5)
    r = subprocess.run(["iperf3", "-c", "172.31.32.1", "-t", str(DUR), "-P", "4", "-J"],
                       capture_output=True, text=True, timeout=DUR+10)
    try:
        d = json.loads(r.stdout)
        s = round(d["end"]["sum_sent"]["bits_per_second"] / 1e6, 1)
        rv = round(d["end"]["sum_received"]["bits_per_second"] / 1e6, 1)
    except:
        s, rv = 0, 0

    print(f"{algo},{name},{s},{rv}")
    results.append((algo, name, s, rv))

# Summary
print("\n=== Summary ===")
print(f"{'Algo':>15} {'Sent':>8} {'Recv':>8}")
for algo, name, s, rv in results:
    print(f"{name:>15} {s:>8.1f} {rv:>8.1f}")
