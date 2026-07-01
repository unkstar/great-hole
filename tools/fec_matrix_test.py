#!/usr/bin/env python3
"""FEC Adaptive Overhead Matrix Test.

Tests all combinations of:
  - 8 algorithms (0-7)
  - 6 loss patterns (1-6)
  - 4 loss rates (0.01, 0.05, 0.10, 0.20)

For each combination:
  1. Start two great-hole instances on loopback
  2. Instance B has LossPattern active
  3. iperf3 TCP through FEC tunnel
  4. Record throughput + overhead stats
"""

import subprocess, time, json, csv, sys, os, re

BIN = os.path.expanduser("~/great-hole/build/src/great-hole")
RESULTS_CSV = "/tmp/fec-matrix-results.csv"
DURATION = 10
ALGOS = range(8)
PATTERNS = range(1, 7)
RATES = [0.01, 0.05, 0.10, 0.20]

ALGO_NAMES = {
    0: "Static", 1: "EWMA+Static", 2: "EWMA+Dynamic",
    3: "PI", 4: "MIMD", 5: "Quantile", 6: "BurstAware", 7: "Gradient"
}
PATT_NAMES = {
    1: "Bernoulli", 2: "Gilbert", 3: "GilbertElliott",
    4: "Sinusoidal", 5: "Step", 6: "CongestionWave"
}

def sudo(cmd):
    return f"echo 'REDACTED' | sudo -S {cmd}"

def write_lua(path, port_self, port_peer, tun_name, tun_ip, tun_peer,
              algo, pattern=0, rate=0.0, extra=""):
    lua = f"""m = hole.udp_dyn_mux({port_self})
c = m:create_channel('0123456789abcdef', '127.0.0.1', {port_peer})
t = hole.tun('{tun_name}')
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
local s = hole.fec_shared_state()
p_enc = hole.fec_pipeline(t, {{}}, c, cfg, true, s)
p_dec = hole.fec_pipeline(c, {{}}, t, cfg, false, s)
print('{tun_name} started')
hole.wait_for_exit()
p_dec:stop() p_enc:stop() t:stop() c:stop() m:stop()
"""
    with open(path, 'w') as f:
        f.write(lua)

def setup_tun(name, ip, peer):
    subprocess.run(sudo(f"ip tuntap add dev {name} mode tun 2>/dev/null").split(), capture_output=True)
    subprocess.run(sudo(f"ip addr add {ip} peer {peer} dev {name} 2>/dev/null").split(), capture_output=True)
    subprocess.run(sudo(f"ip link set {name} up 2>/dev/null").split(), capture_output=True)

def kill_all():
    subprocess.run(sudo("pkill -f 'fec-test-[ab]'").split(), capture_output=True)
    time.sleep(1)

def run_one(algo, pattern, rate):
    port_a = 20000 + algo * 1000 + pattern * 100 + int(rate * 100)
    port_b = port_a + 1
    algo_name = ALGO_NAMES.get(algo, f"algo{algo}")
    patt_name = PATT_NAMES.get(pattern, f"patt{pattern}")

    kill_all()

    # Write configs
    write_lua("/tmp/fec-test-a.lua", port_a, port_b, "fec-test-a", "10.99.1.1", "10.99.1.2",
              algo, 0, 0.0)
    write_lua("/tmp/fec-test-b.lua", port_b, port_a, "fec-test-b", "10.99.1.2", "10.99.1.1",
              algo, pattern, rate)

    # Setup tun
    setup_tun("fec-test-a", "10.99.1.1", "10.99.1.2")
    setup_tun("fec-test-b", "10.99.1.2", "10.99.1.1")

    # Start instances
    subprocess.run(sudo(f"nohup {BIN} --startlua /tmp/fec-test-a.lua > /tmp/fec-test-a.log 2>&1 &").split())
    subprocess.run(sudo(f"nohup {BIN} --startlua /tmp/fec-test-b.lua > /tmp/fec-test-b.log 2>&1 &").split())
    time.sleep(4)

    # Verify running
    pa = subprocess.run(["pgrep", "-f", "fec-test-a.lua"], capture_output=True)
    pb = subprocess.run(["pgrep", "-f", "fec-test-b.lua"], capture_output=True)
    if pa.returncode != 0 or pb.returncode != 0:
        kill_all()
        return {"sent_mbps": -1, "recv_mbps": -1, "error": "startup_failed"}

    # Wait for tunnel negotiation
    ping_ok = False
    for _ in range(5):
        pr = subprocess.run(["ping", "-c", "1", "-W", "2", "10.99.1.2"],
                            capture_output=True, timeout=3)
        if pr.returncode == 0:
            ping_ok = True
            break
        time.sleep(2)

    if not ping_ok:
        kill_all()
        return {"sent_mbps": -2, "recv_mbps": -2, "error": "tunnel_negotiation_failed"}

    # iperf3 TCP test
    try:
        iperf = subprocess.run(
            ["iperf3", "-c", "10.99.1.2", "-t", str(DURATION), "-P", "2", "-J"],
            capture_output=True, text=True, timeout=DURATION + 10)
        data = json.loads(iperf.stdout)
        end = data["end"]
        sent_bps = end["sum_sent"]["bits_per_second"]
        recv_bps = end["sum_received"]["bits_per_second"]
        retrans = end.get("sum_sent", {}).get("retransmits", 0)
        result = {
            "sent_mbps": round(sent_bps / 1e6, 2),
            "recv_mbps": round(recv_bps / 1e6, 2),
            "retrans": retrans,
            "error": None
        }
    except Exception as e:
        result = {"sent_mbps": -3, "recv_mbps": -3, "error": str(e)[:50]}

    kill_all()
    return result

def main():
    results = []
    total = len(ALGOS) * len(PATTERNS) * len(RATES)
    done = 0

    print(f"=== FEC Matrix Test: {total} combinations ===")
    print(f"Algorithms: {list(ALGO_NAMES.values())}")
    print(f"Patterns:   {list(PATT_NAMES.values())}")
    print(f"Rates:      {RATES}")
    print()

    with open(RESULTS_CSV, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(["algo", "algo_name", "pattern", "pattern_name", "rate",
                     "sent_mbps", "recv_mbps", "retrans", "error"])

    for algo in ALGOS:
        for pattern in PATTERNS:
            for rate in RATES:
                done += 1
                algo_name = ALGO_NAMES.get(algo, f"algo{algo}")
                patt_name = PATT_NAMES.get(pattern, f"patt{pattern}")
                pct = done / total * 100
                print(f"[{done}/{total} {pct:.0f}%] algo={algo}({algo_name}) "
                      f"patt={pattern}({patt_name}) rate={rate:.2f} ... ", end="", flush=True)

                r = run_one(algo, pattern, rate)
                results.append((algo, algo_name, pattern, patt_name, rate, r))
                status = f"sent={r['sent_mbps']}M recv={r['recv_mbps']}M"
                if r.get("error"):
                    status += f" ERR={r['error']}"
                print(status)

                with open(RESULTS_CSV, 'a', newline='') as f:
                    w = csv.writer(f)
                    w.writerow([algo, algo_name, pattern, patt_name, rate,
                                r['sent_mbps'], r['recv_mbps'], r.get('retrans', 0),
                                r.get('error', '')])

    print("\n=== Matrix test complete ===")
    print(f"Results saved to {RESULTS_CSV}")

    # Print summary table
    print_summary(results)

def print_summary(results):
    print("\n" + "="*80)
    print("FEC Adaptive Overhead Matrix Test — Summary")
    print("="*80)

    # Best algorithm per loss rate
    print("\n--- Best algorithm per loss rate (by throughput) ---")
    print(f"{'Rate':>6} {'Best Algo':>15} {'SentMbps':>10} {'RecvMbps':>10}")
    print("-"*50)
    for rate in RATES:
        best = None
        best_tp = 0
        for algo, aname, patt, pname, r, res in results:
            if r == rate and res['sent_mbps'] > best_tp:
                best_tp = res['sent_mbps']
                best = (aname, res)
        if best:
            print(f"{rate:>5.0%}  {best[0]:>15} {best[1]['sent_mbps']:>10.1f} {best[1]['recv_mbps']:>10.1f}")

    # Average throughput by algorithm (across all patterns and rates)
    print("\n--- Average throughput by algorithm (all patterns, all rates) ---")
    print(f"{'Algo':>15} {'AvgSent':>10} {'AvgRecv':>10} {'Failures':>8}")
    print("-"*50)
    algo_stats = {}
    for algo, aname, patt, pname, r, res in results:
        if aname not in algo_stats:
            algo_stats[aname] = {"sent": [], "recv": [], "fails": 0}
        if res['sent_mbps'] > 0:
            algo_stats[aname]["sent"].append(res['sent_mbps'])
            algo_stats[aname]["recv"].append(res['recv_mbps'])
        else:
            algo_stats[aname]["fails"] += 1

    for aname in sorted(algo_stats, key=lambda x: sum(algo_stats[x]["sent"])/max(len(algo_stats[x]["sent"]),1), reverse=True):
        s = algo_stats[aname]
        avg_s = sum(s["sent"]) / max(len(s["sent"]), 1)
        avg_r = sum(s["recv"]) / max(len(s["recv"]), 1)
        print(f"{aname:>15} {avg_s:>10.1f} {avg_r:>10.1f} {s['fails']:>8}")

if __name__ == "__main__":
    main()
