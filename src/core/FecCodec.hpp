#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Packet.hpp"

namespace gh {

class FecConfig;
class AdaptiveOverhead;
class LossPattern;

// Shared state between encoder and decoder on the same side.
// Used for PING/FEEDBACK loop and RTT measurement.
struct FecSharedState {
    // Decoder writes when PING received, encoder reads & sends FEEDBACK then clears
    uint64_t pending_feedback_echo = 0;
    // Decoder updates when FEEDBACK received (RTT EWMA in microseconds)
    uint64_t rtt_ewma_us = 0;
    // Encoder writes timestamp of last sent PING
    uint64_t last_ping_sent_us = 0;
    // Consecutive PINGs lost (encoder increments on timeout)
    uint32_t consecutive_ping_lost = 0;
    // Latest measured loss rate from decoder [0..1] (encoder reads for feedback byte)
    float latest_loss_rate = 0.0f;
    // Peer's measured loss rate [0..1], decoded from the fb byte on incoming
    // packets. This is the loss on the direction the LOCAL encoder protects,
    // so the encoder's overhead controller and loss_deadband gate read this —
    // NOT latest_loss_rate (which is the local decoder's measurement of the
    // opposite direction; mixing both in one field let a clean reverse
    // direction zero out the lossy direction's measurement on every packet).
    float peer_loss_rate = 0.0f;
};

// Wire format flags (shared by all codecs and the transport's PING/FEEDBACK)
namespace fec_wire {
enum Flags : uint8_t {
    kWidth1B = 0,
    kWidth2B = 1 << 0,
    kFlagsMask = 0x0F,
    kPing = 1 << 4,
    kFeedback = 1 << 5,
    kRepeat = 1 << 6,
    kEcho = 1 << 7,
};
// RS codec flags: bit3 = direct small packet (no RS), bit6 reused as RS
// repair shard marker (kRepeat never set in RS mode).
enum RsFlags : uint8_t {
    kRsSmall = 1 << 3,
    kRsRepair = 1 << 6,
};
} // namespace fec_wire

// Codec strategy: a pure synchronous processor. It performs no I/O, never
// suspends (no co_await), and owns no fibers; the transport layer owns all
// reads/writes and fiber scheduling. Outbound packets are appended to the
// caller-provided `out` vector, whose capacity is reused across calls.
//
// Container discipline: codecs SHALL NOT use associative containers or
// content-based linear scans in the per-packet path; sequence-indexed state
// uses fixed slot rings (seq & (N-1) with in-slot validation), and payload
// buffers retain their capacity across packets.
class FecCodec {
public:
    virtual ~FecCodec() = default;
    virtual const char* Name() const = 0;

    // Process one inbound packet (encode side: TUN packet; decode side: wire
    // packet). Outbound wire/delivery packets are appended to `out`.
    virtual void OnPacket(Packet&& p, std::vector<Packet>& out) = 0;

    // Idle poll callback (transport 100us timer): flushes deferred encoder
    // output (repair batches) and advances decoder timeouts (watermark
    // recovery, stale-slot eviction).
    virtual void Tick(std::vector<Packet>& out) = 0;

    virtual size_t MaxBatch() const = 0;

    static std::unique_ptr<FecCodec> Create(const FecConfig& cfg, bool is_encoder,
                                            std::shared_ptr<FecSharedState> shared,
                                            AdaptiveOverhead* overhead_ctrl,
                                            LossPattern* loss_pattern);
};

} // namespace gh
