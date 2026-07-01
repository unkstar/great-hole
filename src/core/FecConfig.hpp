#pragma once

#include <cstdint>

namespace gh {

struct FecConfig {
    uint32_t timeout_ms = 4;           // batch maximum wait time (min 1)
    float overhead = 0.15f;            // initial redundancy ratio
    float max_overhead = 0.50f;        // adaptive overhead upper limit
    float repeat_ratio = 4.0f;         // single-packet REPEAT multiplier = 1+ceil(ratio)
    uint32_t symbol_size = 0;          // RaptorQ symbol size, 0 = auto-calculate from MTU
    uint32_t mtu = 1500;               // used for symbol_size calculation
    uint32_t max_batch = 200;          // symbol_count upper limit
    bool obfuscate = true;             // enable IV XOR obfuscation
    uint8_t iv_len = 4;                // IV byte count 1~8
    uint32_t decode_window = 64;       // ring buffer capacity (max 256)
    uint32_t ping_interval_ms = 1000;  // PING send interval
    uint32_t feedback_timeout_ms = 2000; // max interval without data for feedback
    uint32_t feedback_stale_ms = 10000;   // no-feedback fallback overhead timeout
    uint32_t ping_loss_threshold = 5;     // consecutive PING loss threshold
    uint32_t decode_timeout_ms = 200;     // initial decode timeout (RTT-calibrated override)

    // Adaptive overhead algorithm selection
    uint8_t algo = 1;                  // algorithm 0~7 (see fec-spec.md)
    uint32_t loss_window_groups = 50;  // groups per loss-rate update (~500ms-1s)
    float loss_alpha = 0.1f;           // IIR smoothing factor for loss rate

    // Controllable packet loss for testing
    uint8_t test_drop_pattern = 0;     // loss model 0~6 (0=disabled)
    float test_drop_rate = 0.0f;       // primary loss rate parameter
    float test_drop_rate2 = 0.0f;      // secondary parameter (model-dependent)
    uint32_t test_drop_burst = 1;      // burst length / period (model-dependent)
};

} // namespace gh
