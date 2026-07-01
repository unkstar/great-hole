#!/usr/bin/env python3
"""Matrix test: algorithms × loss patterns × loss rates on ali-tokyo FEC tunnel.
Loss pattern runs on ali's decoder (simulates loss on tokyo→ali path).
"""
import subprocess, time, json, sys

PW = "REDACTED"
ALI, TOK = "ali", "tokyo"
SSH = "/c/WINDOWS/System32/OpenSSH/ssh.exe -o StrictHostKeyChecking=no -o ConnectTimeout=10"

NAMES = ["Static","EWMA+Static","EWMA+Dynamic","PI","MIMD","Quantile","BurstAware","Gradient"]
PNAMES = {1:"Bernoulli",2:"Gilbert",3:"GElliott",4:"Sine",5:"Step",6:"CongWave"}
ALGOS = range(8)
PATTERNS = [1,2,3,4,5,6]
RATES = [0.01, 0.05, 0.10, 0.20]

def sh(host, cmd, timeout=30):
    r = subprocess.run(f'{SSH} {host} "{cmd}"', shell=True, capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()

def sh_bg(host, cmd):
    subprocess.Popen(f'{SSH} {host} "{cmd}"', shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def write_configs(algo, pattern, rate):
    """Write Lua configs on both sides. Loss pattern only on ali's decoder."""
    # Ali config: decode side gets loss pattern
    ali_lua = f"""cat > /tmp/fec-tokyo-ali.lua << 'LUA'
local t_ip='202.144.195.145'
m=hole.udp_dyn_mux(11555);c=m:create_channel('0123456789abcdef',t_ip,11556);t=hole.tun('fec-tokyo')
local cfg={{timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo={algo},safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern={pattern},test_drop_rate={rate},test_drop_rate2=0.005,test_drop_burst=8}}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{{}},c,cfg,true,s);hole.fec_pipeline(c,{{}},t,cfg,false,s);hole.wait_for_exit()
LUA
echo 'REDACTED' | sudo -S pkill -9 gh-fec 2>/dev/null
echo 'REDACTED' | sudo -S /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua > /tmp/fec.log 2>&1"""

    # Tokyo config: no loss pattern
    tok_lua = f"""cat > /tmp/fec-tokyo.lua << 'LUA'
local a_ip='39.108.136.48'
m=hole.udp_dyn_mux(11556);c=m:create_channel('0123456789abcdef',a_ip,11555);t=hole.tun('fec-tokyo')
local cfg={{timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo={algo},safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern=0,test_drop_rate=0.0}}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{{}},c,cfg,true,s);hole.fec_pipeline(c,{{}},t,cfg,false,s);hole.wait_for_exit()
LUA
echo 'REDACTED' | sudo -S pkill -9 great-hole 2>/dev/null
echo 'REDACTED' | sudo -S ~/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua > /tmp/fec.log 2>&1"""

    sh(ALI, ali_lua, timeout=5)
    sh(TOK, tok_lua, timeout=5)
    time.sleep(5)

    # Tun setup
    sh(ALI, f"echo '{PW}' | sudo -S ip addr add 172.31.32.1 peer 172.31.32.2 dev fec-tokyo 2>/dev/null; echo '{PW}' | sudo -S ip link set fec-tokyo up 2>/dev/null")
    sh(TOK, f"echo '{PW}' | sudo -S ip addr add 172.31.32.2 peer 172.31.32.1 dev fec-tokyo 2>/dev/null; echo '{PW}' | sudo -S ip link set fec-tokyo up 2>/dev/null")

    # Wait for negotiation
    for i in range(12):
        r = subprocess.run(f'{SSH} {TOK} "ping -c1 -W2 172.31.32.1"', shell=True, capture_output=True, timeout=5)
        if r.returncode == 0:
            return True
        time.sleep(1)
    return False

def run_iperf():
    sh_bg(ALI, "iperf3 -s -1 -B 172.31.32.1 >/dev/null 2>&1")
    time.sleep(1)
    r = subprocess.run(f'{SSH} {TOK} "iperf3 -c 172.31.32.1 -t 10 -J"',
                      shell=True, capture_output=True, text=True, timeout=20)
    try:
        d = json.loads(r.stdout)
        s = round(d["end"]["sum_sent"]["bits_per_second"] / 1e6, 1)
        rv = round(d["end"]["sum_received"]["bits_per_second"] / 1e6, 1)
        return s, rv
    except:
        return -1, -1

# Main
total = len(ALGOS) * len(PATTERNS) * len(RATES)
done = 0
print(f"algo,name,pattern,pname,rate,sent_mbps,recv_mbps")

for algo in ALGOS:
    for pat in PATTERNS:
        for rate in RATES:
            done += 1
            name = NAMES[algo]
            pname = PNAMES[pat]
            print(f"[{done}/{total}] {name} {pname} {rate:.0%} ", end="", flush=True)

            if not write_configs(algo, pat, rate):
                print("→ nego_fail")
                continue

            s, rv = run_iperf()
            print(f"→ {s}/{rv} Mbps")
            print(f"{algo},{name},{pat},{pname},{rate},{s},{rv}", flush=True)

print("=== DONE ===")
