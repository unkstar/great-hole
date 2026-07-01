// Benchmark: RaptorQ encode/decode throughput
// Build: add to CMakeLists.txt test targets
// Usage: ./BenchLcrq [batch_packets=10] [symbol_size=1440] [overhead=0.5] [iterations=1000]
//
// Simulates realistic FEC pipeline:
//   1. N packets of ~1400 bytes each → BuildBlob → RaptorQ encode → K symbols
//   2. Generate K + ceil(K*overhead) symbols (simulates SendBatch)
//   3. Submit K symbols → decode (simulates ideal reception)
// Reports: encode Mbps, decode Mbps, symbols/sec

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <random>

#include "RaptorQ.hpp"

using namespace gh;

static std::vector<uint8_t> MakePacket(size_t size, uint8_t seed) {
    std::vector<uint8_t> pkt(size);
    for (size_t i = 0; i < size; i++) {
        pkt[i] = static_cast<uint8_t>((i * 7 + seed * 13) & 0xFF);
    }
    return pkt;
}

// Replicate BuildBlob logic exactly
static void BuildBlob(const std::vector<std::vector<uint8_t>>& packets, std::vector<uint8_t>& blob) {
    blob.clear();
    uint32_t count = static_cast<uint32_t>(packets.size());
    blob.push_back(static_cast<uint8_t>(count & 0xFF));
    blob.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    blob.push_back(static_cast<uint8_t>((count >> 16) & 0xFF));
    blob.push_back(static_cast<uint8_t>((count >> 24) & 0xFF));
    for (auto& pkt : packets) {
        uint16_t len = static_cast<uint16_t>(pkt.size());
        blob.push_back(static_cast<uint8_t>(len & 0xFF));
        blob.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        size_t pos = blob.size();
        blob.resize(pos + len);
        std::memcpy(blob.data() + pos, pkt.data(), len);
    }
}

int main(int argc, char* argv[]) {
    int batch_packets = (argc > 1) ? std::atoi(argv[1]) : 10;
    int symbol_size = (argc > 2) ? std::atoi(argv[2]) : 1440;
    float overhead = (argc > 3) ? std::atof(argv[3]) : 0.5f;
    int iterations = (argc > 4) ? std::atoi(argv[4]) : 1000;

    // Generate packets (~1400 bytes each, like typical TCP over tunnel)
    std::vector<std::vector<uint8_t>> packets;
    for (int i = 0; i < batch_packets; i++) {
        packets.push_back(MakePacket(1400, static_cast<uint8_t>(i)));
    }

    // Build blob (same as BuildBlob)
    std::vector<uint8_t> blob;
    BuildBlob(packets, blob);

    // Pad to symbol_size boundary
    uint64_t F = blob.size();
    if (F % symbol_size != 0) {
        size_t pad = symbol_size - (F % symbol_size);
        blob.insert(blob.end(), pad, 0);
        F = blob.size();
    }

    uint16_t T = static_cast<uint16_t>(symbol_size);

    std::cout << "=== RaptorQ Benchmark ===" << std::endl;
    std::cout << "Batch packets: " << batch_packets << std::endl;
    std::cout << "Symbol size: " << symbol_size << " bytes" << std::endl;
    std::cout << "Overhead: " << overhead << std::endl;
    std::cout << "Blob size: " << blob.size() << " bytes" << std::endl;
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << std::endl;

    // ========== ENCODE benchmark ==========
    {
        std::vector<double> encode_times_us;
        uint64_t total_sym = 0;

        for (int iter = 0; iter < iterations; iter++) {
            auto t0 = std::chrono::steady_clock::now();

            RaptorQ rq(F, T);
            rq.Encode(blob.data(), blob.size());
            uint32_t K = rq.K();
            uint32_t extra = static_cast<uint32_t>(std::ceil(K * overhead));
            uint32_t total = K + extra;

            auto sym_buf = std::make_unique<uint8_t[]>(T);
            for (uint32_t esi = 0; esi < total; esi++) {
                rq.GenerateSymbol(esi, sym_buf.get());
            }

            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            encode_times_us.push_back(us);
            total_sym += total;
        }

        // Stats
        std::sort(encode_times_us.begin(), encode_times_us.end());
        double avg_us = 0;
        for (auto t : encode_times_us) avg_us += t;
        avg_us /= encode_times_us.size();

        double p50 = encode_times_us[encode_times_us.size() / 2];
        double p99 = encode_times_us[encode_times_us.size() * 99 / 100];
        double total_sym_all = static_cast<double>(total_sym);
        double total_sec = avg_us * iterations / 1e6;
        double sym_per_sec = total_sym_all / total_sec;
        double blob_mbps = (static_cast<double>(blob.size()) * iterations * 8) / (avg_us * iterations);
        double wire_mbps = (static_cast<double>(total_sym_all * T) * 8) / (avg_us * iterations);

        std::cout << "--- ENCODE ---" << std::endl;
        std::cout << "K = " << (F / T) << ", extra = " << (total_sym / iterations - F/T) << ", total symbols/batch = " << (total_sym / iterations) << std::endl;
        printf("  avg: %.1f us  p50: %.1f us  p99: %.1f us\n", avg_us, p50, p99);
        printf("  Symbols/sec: %.0f\n", sym_per_sec);
        printf("  Input Mbps:  %.1f\n", blob_mbps);
        printf("  Wire Mbps:   %.1f\n", wire_mbps);
    }

    // ========== DECODE benchmark ==========
    {
        std::vector<double> decode_times_us;
        uint64_t total_submit = 0;

        for (int iter = 0; iter < iterations; iter++) {
            // Re-encode for this iteration
            RaptorQ enc(F, T);
            enc.Encode(blob.data(), blob.size());
            uint32_t K = enc.K();

            auto sym_buf = std::make_unique<uint8_t[]>(T);
            std::vector<std::pair<std::vector<uint8_t>, uint32_t>> symbols;
            for (uint32_t esi = 0; esi < K; esi++) {
                enc.GenerateSymbol(esi, sym_buf.get());
                symbols.emplace_back(std::vector<uint8_t>(sym_buf.get(), sym_buf.get() + T), esi);
            }

            auto t0 = std::chrono::steady_clock::now();

            RaptorQ dec(F, T);
            for (auto& [data, esi] : symbols) {
                dec.SubmitSymbol(data.data(), T, esi);
            }
            std::vector<uint8_t> decoded(F);
            bool ok = dec.TryDecode(decoded.data(), F);

            auto t1 = std::chrono::steady_clock::now();
            double us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            decode_times_us.push_back(us);
            total_submit += K;

            if (!ok) {
                std::cerr << "DECODE FAILED at iteration " << iter << std::endl;
                return 1;
            }
        }

        std::sort(decode_times_us.begin(), decode_times_us.end());
        double avg_us = 0;
        for (auto t : decode_times_us) avg_us += t;
        avg_us /= decode_times_us.size();

        double p50 = decode_times_us[decode_times_us.size() / 2];
        double p99 = decode_times_us[decode_times_us.size() * 99 / 100];
        double submit_mbps = (static_cast<double>(total_submit * T) * 8) / (avg_us * iterations);

        std::cout << "--- DECODE (K symbols submitted) ---" << std::endl;
        printf("  avg: %.1f us  p50: %.1f us  p99: %.1f us\n", avg_us, p50, p99);
        printf("  Submit+Decode Mbps: %.1f\n", submit_mbps);
    }

    // ========== Full pipeline simulation ==========
    {
        // Simulate 1 second of operation at various timeout values
        std::cout << "--- FULL PIPELINE SIMULATION (1 sec at 20 Mbps input) ---" << std::endl;
        double target_mbps = 20.0;
        double bytes_per_sec = target_mbps * 1e6 / 8;
        double pkts_per_sec = bytes_per_sec / 1400.0;  // ~1786 pkts/sec

        for (int timeout_ms : {4, 8, 10, 20}) {
            double pkts_per_batch = pkts_per_sec * timeout_ms / 1000.0;
            int n_pkts = std::max(1, static_cast<int>(pkts_per_batch));
            double batches_per_sec = pkts_per_sec / n_pkts;

            // Build blob for this batch size
            std::vector<std::vector<uint8_t>> sim_pkts;
            for (int i = 0; i < n_pkts; i++) {
                sim_pkts.push_back(MakePacket(1400, static_cast<uint8_t>(i)));
            }
            std::vector<uint8_t> sim_blob;
            BuildBlob(sim_pkts, sim_blob);
            uint64_t sim_F = sim_blob.size();
            if (sim_F % T != 0) {
                sim_blob.insert(sim_blob.end(), T - (sim_F % T), 0);
                sim_F = sim_blob.size();
            }

            // Measure one encode
            auto t0 = std::chrono::steady_clock::now();
            RaptorQ rq(sim_F, T);
            rq.Encode(sim_blob.data(), sim_blob.size());
            uint32_t K = rq.K();
            uint32_t extra = static_cast<uint32_t>(std::ceil(K * overhead));
            auto sym_buf = std::make_unique<uint8_t[]>(T);
            for (uint32_t esi = 0; esi < K + extra; esi++) {
                rq.GenerateSymbol(esi, sym_buf.get());
            }
            auto t1 = std::chrono::steady_clock::now();
            double encode_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

            double encode_mbps = (sim_F * 8.0) / encode_us;
            double cpu_util = (encode_us * batches_per_sec / 1e6) * 100.0;  // % of CPU time spent encoding
            double wire_mbps = (K + extra) * T * 8.0 * batches_per_sec / 1e6;
            double overhead_pct = ((K + extra) * T - sim_blob.size()) * 100.0 / sim_blob.size();

            printf("  timeout=%2dms: %2d pkts/batch, %2d src symbols, +%d extra, %.0f batches/sec, "
                   "encode=%.0fus (%.0f Mbps), cpu=%.1f%%, wire=%.1f Mbps, ovhd=%.0f%%\n",
                   timeout_ms, n_pkts, K, extra, batches_per_sec,
                   encode_us, encode_mbps, cpu_util, wire_mbps, overhead_pct);
        }
    }

    std::cout << "\nDone." << std::endl;
    return 0;
}
