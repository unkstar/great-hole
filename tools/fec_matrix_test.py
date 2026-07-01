#!/usr/bin/env python3
"""FEC Adaptive Overhead Matrix Test — uses netns for isolation."""

import subprocess, time, json, csv, os

BIN = os.path.expanduser("~/great-hole/build/src/great-hole")
CSV = "/tmp/fec-matrix-results.csv"
DURATION = 10
ALGOS = range(8)
PATTERNS = range(1, 7)
RATES = [0.01, 0.05, 0.10, 0.20]
PW = "REDACTED"

ANAME = {0:"Static",1:"EWMA+Static",2:"EWMA+Dynamic",3:"PI",4:"MIMD",5:"Quantile",6:"BurstAware",7:"Gradient"}
PNAME = {1:"Bernoulli",2:"Gilbert",3:"GilbertElliott",4:"Sinusoidal",5:"Step",6:"CongestionWave"}

def sh(cmd):
    return subprocess.run(f"echo '{PW}' | sudo -S {cmd}", shell=True, capture_output=True, text=True)

def lua(path, port_self, port_peer, tun, ip, peer, algo, pattern, rate):
    s = f"""m = hole.udp_dyn_mux({port_self})
c = m:create_channel('0123456789abcdef', '127.0.0.1', {port_peer})
t = hole.tun('{tun}')
local cfg = {{
    timeout_ms = 4, overhead = 0.15, max_overhead = 0.50, repeat_ratio = 3.0,
    symbol_size = 1440, mtu = 1420, max_batch = 65535, obfuscate = false,
    iv_len = 4, decode_window = 64, ping_interval_ms = 500,
    feedback_timeout_ms = 1000, feedback_stale_ms = 10000,
    ping_loss_threshold = 5, decode_timeout_ms = 200,
    algo = {algo},
    test_drop_pattern = {pattern}, test_drop_rate = {rate},
    test_drop_rate2 = 0.005, test_drop_burst = 8,
}}
local sh = hole.fec_shared_state()
local enc = hole.fec_pipeline(t, {{}}, c, cfg, true, sh)
local dec = hole.fec_pipeline(c, {{}}, t, cfg, false, sh)
print('{tun} ready algo={algo} patt={pattern} rate={rate}')
hole.wait_for_exit()
dec:stop() enc:stop() t:stop() c:stop() m:stop()
"""
    with open(path, 'w') as f: f.write(s)

def run_one(algo, pattern, rate):
    pa, pb = 21000, 21001
    ns = "fecns"
    tun_a, tun_b = "fec-a", "fec-b"
    ip_a, ip_b = "10.99.1.1", "10.99.1.2"

    # Cleanup
    sh(f"ip netns del {ns} 2>/dev/null")
    sh(f"ip link del {tun_a} 2>/dev/null")
    sh(f"pkill -9 -f 'fec-a.lua|fec-b.lua' 2>/dev/null")
    sh(f"pkill -9 iperf3 2>/dev/null")
    time.sleep(1)

    # Create netns
    sh(f"ip netns add {ns}")

    # Instance A (main namespace, encoder, no loss)
    lua("/tmp/fec-a.lua", pa, pb, tun_a, ip_a, ip_b, algo, 0, 0.0)
    sh(f"ip tuntap add dev {tun_a} mode tun 2>/dev/null")
    sh(f"ip addr add {ip_a} peer {ip_b} dev {tun_a}")
    sh(f"ip link set {tun_a} up")
    sh_bg = f"echo '{PW}' | sudo -S nohup {BIN} --startlua /tmp/fec-a.lua > /tmp/fec-a.log 2>&1 &"
    subprocess.run(sh_bg, shell=True, capture_output=True)

    # Instance B (netns, decoder, with loss pattern)
    lua("/tmp/fec-b.lua", pb, pa, tun_b, ip_b, ip_a, algo, pattern, rate)
    sh(f"ip tuntap add dev {tun_b} mode tun 2>/dev/null")
    sh(f"ip link set {tun_b} netns {ns}")
    sh(f"ip netns exec {ns} ip addr add {ip_b} peer {ip_a} dev {tun_b}")
    sh(f"ip netns exec {ns} ip link set {tun_b} up")
    sh(f"ip netns exec {ns} ip link set lo up")
    sh_bg2 = f"echo '{PW}' | sudo -S ip netns exec {ns} nohup {BIN} --startlua /tmp/fec-b.lua > /tmp/fec-b.log 2>&1 &"
    subprocess.run(sh_bg2, shell=True, capture_output=True)

    time.sleep(3)

    # Verify
    pa_ok = subprocess.run(["pgrep", "-f", "fec-a.lua"], capture_output=True).returncode == 0
    pb_ok = sh(f"ip netns exec {ns} pgrep -f 'fec-b.lua'").stdout.strip() != ""
    if not pa_ok or not pb_ok:
        sh(f"ip netns del {ns} 2>/dev/null")
        return {"sent_mbps": -1, "recv_mbps": -1, "error": "startup"}

    # Wait for negotiation + ping test
    ping_ok = False
    for i in range(8):
        time.sleep(1.5)
        pr = subprocess.run(["ping", "-c", "1", "-W", "2", ip_b], capture_output=True, timeout=3)
        if pr.returncode == 0:
            ping_ok = True
            break
    if not ping_ok:
        sh(f"ip netns del {ns} 2>/dev/null")
        return {"sent_mbps": -2, "recv_mbps": -2, "error": "negotiation"}

    # Start iperf3 server in netns, client in main namespace
    sh(f"ip netns exec {ns} nohup iperf3 -s -1 --json > /tmp/iperf-srv.log 2>&1 &")
    time.sleep(0.5)

    try:
        iperf = subprocess.run(
            ["iperf3", "-c", ip_b, "-t", str(DURATION), "-P", "2", "-J"],
            capture_output=True, text=True, timeout=DURATION + 10)
        d = json.loads(iperf.stdout)
        sent = d["end"]["sum_sent"]["bits_per_second"] / 1e6
        recv = d["end"]["sum_received"]["bits_per_second"] / 1e6
        retrans = d["end"]["sum_sent"].get("retransmits", 0)
        result = {"sent_mbps": round(sent, 2), "recv_mbps": round(recv, 2), "retrans": retrans, "error": None}
    except Exception as e:
        result = {"sent_mbps": -3, "recv_mbps": -3, "error": str(e)[:60]}

    sh(f"ip netns del {ns} 2>/dev/null")
    sh(f"pkill -9 -f 'fec-a.lua|fec-b.lua' 2>/dev/null")
    sh(f"pkill -9 iperf3 2>/dev/null")
    return result

def main():
    total = len(ALGOS) * len(PATTERNS) * len(RATES)
    done = 0
    print(f"=== FEC Matrix Test: {total} combinations ===")
    open(CSV, 'w').close()  # clear

    with open(CSV, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(["algo","algo_name","pattern","pattern_name","rate","sent_mbps","recv_mbps","retrans","error"])

    for algo in ALGOS:
        for pattern in PATTERNS:
            for rate in RATES:
                done += 1
                pct = done / total * 100
                print(f"[{done}/{total} {pct:.0f}%] a={algo}({ANAME[algo]}) p={pattern}({PNAME[pattern]}) r={rate:.2f}",
                      end=" ", flush=True)

                r = run_one(algo, pattern, rate)
                msglen = len(str(r))
                if msglen < 50:
                    print(str(r))
                else:
                    print("OK" if r['error'] is None else f"ERR={r['error']}")

                with open(CSV, 'a', newline='') as f:
                    w = csv.writer(f)
                    w.writerow([algo, ANAME[algo], pattern, PNAME[pattern], rate,
                                r['sent_mbps'], r['recv_mbps'], r.get('retrans',0),
                                r.get('error','')])

    print("\n=== Complete ===")
    # Print summary
    rows = []
    with open(CSV) as f:
        for r in csv.DictReader(f):
            r['sent_mbps'] = float(r['sent_mbps']); r['recv_mbps'] = float(r['recv_mbps'])
            rows.append(r)

    ok = [r for r in rows if r['sent_mbps'] > 0]
    nok = [r for r in rows if r['sent_mbps'] <= 0]
    print(f"Passed: {len(ok)}, Failed: {len(nok)}")
    if ok:
        avg = sum(r['sent_mbps'] for r in ok) / len(ok)
        print(f"Average throughput: {avg:.1f} Mbps")

        # Best per rate
        print("\n--- Best per loss rate ---")
        print(f"{'Rate':>6} {'Algo':>15} {'SentMbps':>10} {'Err'}")
        for rate in RATES:
            best = max([r for r in ok if abs(float(r['rate'])-rate) < 0.005], key=lambda r: r['sent_mbps'], default=None)
            if best:
                print(f"{rate:>5.0%}  {best['algo_name']:>15} {best['sent_mbps']:>10.1f}  {best.get('error','')}")

        # Average by algo
        print("\n--- Avg throughput by algorithm ---")
        for an in sorted(set(r['algo_name'] for r in ok)):
            rs = [r['sent_mbps'] for r in ok if r['algo_name'] == an]
            print(f"{an:>15}: avg={sum(rs)/len(rs):.1f} Mbps, n={len(rs)}")

if __name__ == "__main__":
    main()
