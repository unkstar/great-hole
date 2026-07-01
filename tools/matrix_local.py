#!/usr/bin/env python3
"""Matrix test run from local machine, controlling both ali and tokyo."""
import subprocess, time, json, sys

PW = "REDACTED"
ALI = "ali"
TOK = "tokyo"
NAMES = ["Static","EWMA+Static","EWMA+Dynamic","PI","MIMD","Quantile","BurstAware","Gradient"]
SSH_BASE = "/c/WINDOWS/System32/OpenSSH/ssh.exe -o StrictHostKeyChecking=no"

def ssh_run(host, cmd):
    """Run a command via SSH, capture output, return stdout."""
    r = subprocess.run(
        f'{SSH_BASE} {host} "{cmd}"',
        shell=True, capture_output=True, text=True, timeout=60
    )
    return r.stdout.strip()

def ssh_bg(host, cmd):
    """Run a command in background via SSH (no waiting)."""
    subprocess.Popen(
        f'{SSH_BASE} {host} "{cmd}"',
        shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

# Config template for each side
def ali_config(algo):
    return f"""cat > /tmp/fec-tokyo-ali.lua << 'LUAEOF'
local tokyo_ip = '202.144.195.145'
m = hole.udp_dyn_mux(11555)
c = m:create_channel('0123456789abcdef', tokyo_ip, 11556)
t = hole.tun('fec-tokyo')
local cfg = {{timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo={algo}}}
local s=hole.fec_shared_state()
hole.fec_pipeline(t,{{}},c,cfg,true,s)
hole.fec_pipeline(c,{{}},t,cfg,false,s)
hole.wait_for_exit()
LUAEOF"""

def tok_config(algo):
    return f"""cat > /tmp/fec-tokyo.lua << 'LUAEOF'
local ali_ip = '39.108.136.48'
m = hole.udp_dyn_mux(11556)
c = m:create_channel('0123456789abcdef', ali_ip, 11555)
t = hole.tun('fec-tokyo')
local cfg = {{timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo={algo}}}
local s=hole.fec_shared_state()
hole.fec_pipeline(t,{{}},c,cfg,true,s)
hole.fec_pipeline(c,{{}},t,cfg,false,s)
hole.wait_for_exit()
LUAEOF"""

print("algo,name,sent_mbps,recv_mbps")

for algo in range(8):
    name = NAMES[algo]
    print(f"\n=== algo={algo} ({name}) ===", flush=True)

    # 1. Kill old processes
    ssh_run(ALI, f"echo '{PW}' | sudo -S pkill -9 gh-fec 2>/dev/null; echo ok")
    ssh_run(TOK, f"echo '{PW}' | sudo -S pkill -9 great-hole 2>/dev/null; echo ok")
    time.sleep(2)

    # 2. Write configs
    ssh_run(ALI, ali_config(algo).replace('"', '\\"'))
    ssh_run(TOK, tok_config(algo).replace('"', '\\"'))

    # 3. Start FEC on both sides
    ssh_bg(ALI, f"echo '{PW}' | sudo -S /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua > /tmp/fec-tokyo-ali.log 2>&1")
    time.sleep(0.5)
    ssh_bg(TOK, f"echo '{PW}' | sudo -S /home/ggcuser/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua > /tmp/fec-tokyo.log 2>&1")
    time.sleep(5)

    # 4. Check processes started
    ali_pid = ssh_run(ALI, "pgrep -a gh-fec 2>/dev/null || echo NONE")
    tok_pid = ssh_run(TOK, "pgrep -a great-hole 2>/dev/null || echo NONE")
    if "NONE" in ali_pid or "NONE" in tok_pid:
        print(f"FAIL: ali={ali_pid[:30]} tok={tok_pid[:30]}")
        continue

    # 5. Setup tun
    ssh_run(ALI, f"echo '{PW}' | sudo -S ip addr add 172.31.32.1 peer 172.31.32.2 dev fec-tokyo 2>/dev/null; echo '{PW}' | sudo -S ip link set fec-tokyo up 2>/dev/null")
    ssh_run(TOK, f"echo '{PW}' | sudo -S ip addr add 172.31.32.2 peer 172.31.32.1 dev fec-tokyo 2>/dev/null; echo '{PW}' | sudo -S ip link set fec-tokyo up 2>/dev/null")

    # 6. Wait for negotiation
    ok = False
    for i in range(15):
        r = subprocess.run(f'{SSH_BASE} {TOK} "ping -c1 -W2 172.31.32.1"',
                          shell=True, capture_output=True, timeout=5)
        if r.returncode == 0:
            print(f"  tunnel up after {i+1}s", flush=True)
            ok = True
            break
        time.sleep(1)
    if not ok:
        print(f"  {algo},{name},-1,-1,nego_fail")
        continue

    # 7. iperf3 test
    ssh_bg(ALI, "iperf3 -s -1 -B 172.31.32.1 >/dev/null 2>&1")
    time.sleep(1)

    r = subprocess.run(f'{SSH_BASE} {TOK} "iperf3 -c 172.31.32.1 -t 15 -P 4 -J"',
                      shell=True, capture_output=True, text=True, timeout=30)
    try:
        d = json.loads(r.stdout)
        s = round(d["end"]["sum_sent"]["bits_per_second"] / 1e6, 1)
        rv = round(d["end"]["sum_received"]["bits_per_second"] / 1e6, 1)
        print(f"  {algo},{name},{s},{rv}", flush=True)
    except:
        print(f"  {algo},{name},-2,-2,json_error", flush=True)

print("\n=== DONE ===")
