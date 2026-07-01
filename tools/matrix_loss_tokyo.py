#!/usr/bin/env python3
"""Matrix test runs ON tokyo — no SSH quoting issues."""
import subprocess, time, json

PW = "REDACTED"
NAMES = ["Static","EWMA+Static","EWMA+Dynamic","PI","MIMD","Quantile","BurstAware","Gradient"]
PNAMES = {1:"Bernoulli",2:"Gilbert",3:"GElliott",4:"Sine",5:"Step",6:"CongWave"}
ALGOS, PATTERNS, RATES = range(8), [1,2,3,4,5,6], [0.01,0.05,0.10,0.20]

def sh(cmd):
    subprocess.run(cmd, shell=True, capture_output=True)

def one_test(algo, pat, rate):
    aname, pname = NAMES[algo], PNAMES[pat]

    # Kill old
    sh("sudo -S pkill -9 great-hole 2>/dev/null ' | sudo -S" % PW)
    sh("ssh ali 'sudo -S pkill -9 gh-fec 2>/dev/null <<< %s'" % PW)
    time.sleep(2)

    # Write ali config (loss pattern on decoder)
    ali_cfg = """cat > /tmp/fec-tokyo-ali.lua << 'LUAEOF'
local t_ip='202.144.195.145'
m=hole.udp_dyn_mux(11555);c=m:create_channel('0123456789abcdef',t_ip,11556);t=hole.tun('fec-tokyo')
local cfg={timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=%d,safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern=%d,test_drop_rate=%.4f,test_drop_rate2=0.005,test_drop_burst=8}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{},c,cfg,true,s);hole.fec_pipeline(c,{},t,cfg,false,s);hole.wait_for_exit()
LUAEOF
sudo -S pkill -9 gh-fec 2>/dev/null ' | sudo -S
sudo -S /tmp/gh-fec-tokyo --startlua /tmp/fec-tokyo-ali.lua > /tmp/fec.log 2>&1 ' | sudo -S""" % (algo, pat, rate, PW, PW)
    sh("ssh ali '%s'" % ali_cfg)

    # Write tokyo config (no loss pattern)
    tok_cfg = """cat > /tmp/fec-tokyo.lua << 'LUAEOF'
local a_ip='39.108.136.48'
m=hole.udp_dyn_mux(11556);c=m:create_channel('0123456789abcdef',a_ip,11555);t=hole.tun('fec-tokyo')
local cfg={timeout_ms=4,overhead=0.15,max_overhead=0.50,repeat_ratio=3.0,repeat_ratio_min=1,repeat_ratio_max=5,symbol_size=1440,mtu=1420,max_batch=65535,obfuscate=false,iv_len=4,decode_window=64,ping_interval_ms=1000,feedback_timeout_ms=2000,feedback_stale_ms=10000,ping_loss_threshold=5,decode_timeout_ms=200,algo=%d,safety_margin=0.01,loss_window_groups=50,loss_alpha=0.1,test_drop_pattern=0,test_drop_rate=0.0}
local s=hole.fec_shared_state();hole.fec_pipeline(t,{},c,cfg,true,s);hole.fec_pipeline(c,{},t,cfg,false,s);hole.wait_for_exit()
LUAEOF
sudo -S pkill -9 great-hole 2>/dev/null ' | sudo -S
sudo -S /home/ggcuser/great-hole/build/src/great-hole --startlua /tmp/fec-tokyo.lua > /tmp/fec.log 2>&1 ' | sudo -S""" % (algo, PW, PW)
    sh(tok_cfg)
    time.sleep(5)

    # Setup tun
    sh("sudo -S ip addr add 172.31.32.2 peer 172.31.32.1 dev fec-tokyo 2>/dev/null ' | sudo -S" % PW)
    sh("sudo -S ip link set fec-tokyo up 2>/dev/null ' | sudo -S" % PW)
    sh("ssh ali 'sudo -S ip addr add 172.31.32.1 peer 172.31.32.2 dev fec-tokyo 2>/dev/null <<< %s'" % PW)
    sh("ssh ali 'sudo -S ip link set fec-tokyo up 2>/dev/null <<< %s'" % PW)

    # Wait negotiation
    ok = False
    for i in range(12):
        r = subprocess.run("ping -c1 -W2 172.31.32.1", shell=True, capture_output=True)
        if r.returncode == 0: ok = True; break
        time.sleep(1)
    if not ok:
        return -1, -1, "nego"

    # iperf3
    sh("ssh ali 'iperf3 -s -1 -B 172.31.32.1 >/dev/null 2>&1 &'")
    time.sleep(1)
    r = subprocess.run("iperf3 -c 172.31.32.1 -t 10 -J", shell=True, capture_output=True, text=True, timeout=20)
    try:
        d = json.loads(r.stdout)
        return round(d["end"]["sum_sent"]["bits_per_second"]/1e6,1), round(d["end"]["sum_received"]["bits_per_second"]/1e6,1), "ok"
    except:
        return -2, -2, "iperf"

# Main
total = len(ALGOS)*len(PATTERNS)*len(RATES)
done = 0
print("algo,name,pattern,pname,rate,sent_mbps,recv_mbps,status")
for algo in ALGOS:
    for pat in PATTERNS:
        for rate in RATES:
            done += 1
            print(f"[{done}/{total}] {NAMES[algo]} {PNAMES[pat]} {rate:.0%}", flush=True)
            s, rv, st = one_test(algo, pat, rate)
            print(f"  -> {s}/{rv} {st}")
            # Write to file for safety
            with open("/tmp/matrix-results.csv","a") as f:
                f.write(f"{algo},{NAMES[algo]},{pat},{PNAMES[pat]},{rate},{s},{rv},{st}\n")
print("DONE")
