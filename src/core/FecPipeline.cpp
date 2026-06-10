#include "FecPipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>

#include <boost/log/trivial.hpp>

#include "Cancel.hpp"
#include "Asio.hpp"
#include "Coroutine.hpp"
#include "ErrorCode.hpp"
#include "GetCurrentFiber.hpp"
#include "Packet.hpp"
#include "RaptorQ.hpp"

namespace gh {

// ==================== Constructor ====================

FecPipeline::FecPipeline(boost::asio::io_context& io, std::shared_ptr<EndpointInput> in,
                         const std::vector<std::shared_ptr<Filter>>& filters, std::shared_ptr<EndpointOutput> out,
                         FecConfig cfg, bool is_encoder)
    : Pipeline(io, in, filters, out), _Cfg(cfg), _IsEncoder(is_encoder) {
    if (!is_encoder) {
        uint32_t window = static_cast<uint32_t>(std::bit_ceil(cfg.decode_window));
        _RingBuffer.resize(window);
        _RingMask = window - 1;
    }
}

// ==================== Wire Format Helpers ====================

uint8_t FecPipeline::BuildFlags() const {
    uint8_t flags = 0;
    if (_Cfg.iv_len > 0 && _Cfg.obfuscate) {
        flags |= (_Cfg.iv_len & 0x07) << 1;
    }
    return flags;
}

uint32_t FecPipeline::BuildDword(uint32_t group_seq, uint8_t flags) const {
    return (group_seq & 0xFFFFFF) | (static_cast<uint32_t>(flags) << 24);
}

// ==================== Process (Entry Point) ====================

Omni::Fiber::Coroutine<void> FecPipeline::Process() {
    auto& fiber = co_await Omni::Fiber::GetCurrentFiber();

    if (_IsEncoder) {
        auto batch_queue = std::make_shared<std::vector<Packet>>();
        auto reader_done = std::make_shared<bool>(false);

        fiber.Spawn("fec-reader", [this, batch_queue, reader_done]() -> Omni::Fiber::Coroutine<void> {
            while (!_Stop.IsTriggered()) {
                Packet p;
                auto err = co_await _In->Read(p, _Stop);
                if (err) {
                    if (err == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) { *reader_done = true; break; }
                    if (err == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) continue;
                    if (IsCritical(err)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline read error: " << err.message(); throw SystemError(err, "FecPipeline read error"); }
                    BOOST_LOG_TRIVIAL(warning) << "FecPipeline read warning: " << err.message(); continue;
                }
                if (_Filters.empty()) {
                    batch_queue->push_back(std::move(p));
                    while (batch_queue->size() < _Cfg.max_batch && !_Stop.IsTriggered()) {
                        Packet p2; if (_In->TryRead(p2)) break;
                        batch_queue->push_back(std::move(p2));
                    }
                } else {
                    for (auto& f : _Filters) { auto fe = co_await f->Pipe(p, _Stop); if (fe) { if (fe == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) break; if (IsCritical(fe)) { BOOST_LOG_TRIVIAL(error) << "FecPipeline filter error: " << fe.message(); throw SystemError(fe, "FecPipeline filter error"); } break; } }
                    batch_queue->push_back(std::move(p));
                }
            }
        });

        while (!_Stop.IsTriggered()) {
            auto timer = std::make_shared<boost::asio::steady_timer>(_Io);
            timer->expires_after(std::chrono::milliseconds(_Cfg.timeout_ms));
            co_await timer->async_wait(Omni::Fiber::AsioUseFiber);
            if (!batch_queue->empty() && !_Stop.IsTriggered()) {
                std::vector<Packet> batch; size_t n = std::min(batch_queue->size(), (size_t)_Cfg.max_batch);
                batch.reserve(n); std::move(batch_queue->begin(), batch_queue->begin()+n, std::back_inserter(batch));
                batch_queue->erase(batch_queue->begin(), batch_queue->begin()+n);
                co_await SendBatch(batch);
            }
            if (*reader_done && batch_queue->empty()) break;
        }
    } else {
        // ============ DECODE PATH ============
        while (!_Stop.IsTriggered()) {
            Packet p;
            auto err = co_await _In->Read(p, _Stop);
            if (err) {
                if (err == ErrorCode{AppErrorCategory::kOperationAborted, kAppError}) {
                    continue;
                }
                if (err == ErrorCode{AppErrorCategory::kEndOfStream, kAppError}) {
                    break;
                }
                if (IsCritical(err)) {
                    BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") decode read error: " << err.message();
                    throw SystemError(err, "FecPipeline decode read error");
                }
                BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this
                                           << ") decode read warning: " << err.message();
                continue;
            }

            if (p.DataSize() < 5) {
                continue; // minimum: DWORD(4B) + fb(1B)
            }

            // Parse DWORD header
            uint32_t dw = p.PopFrontLE<uint32_t>();
            uint32_t group_seq = dw & 0xFFFFFF;
            uint8_t f = (dw >> 24) & 0xFF;
            uint8_t fb = p.PopFrontLE<uint8_t>();

            uint64_t echo = 0;
            bool has_echo = (f & kEcho) != 0;
            if (has_echo) {
                if (p.DataSize() < 8) continue;
                echo = p.PopFrontLE<uint64_t>();
            }

            bool is_ping = (f & kPing) != 0;
            bool is_feedback = (f & kFeedback) != 0;
            bool is_repeat = (f & kRepeat) != 0;

            if (is_ping) {
                // PING: remaining 8B is payload (echo timestamp)
                if (p.DataSize() >= 8) {
                    uint64_t ping_echo = p.PopFrontLE<uint64_t>();
                    _PendingEcho = ping_echo;
                }
                continue;
            }

            if (is_feedback) {
                continue;
            }

            if (is_repeat) {
                // REPEAT: [IV:1~8B][XORed raw packet]
                uint8_t iv_len = (f >> 1) & 0x07;
                if (iv_len > 0 && _Cfg.obfuscate) {
                    if (p.DataSize() < iv_len) continue;
                    auto iv_span = p.PopFront(iv_len);
                    for (size_t i = 0; i < p.DataSize(); i++) {
                        p._Data[p._Offset + i] ^= iv_span[i % iv_len];
                    }
                }
                auto err_write = co_await _Out->Write(p, _Stop);
                if (err_write && IsCritical(err_write)) {
                    BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this
                                             << ") write error: " << err_write.message();
                    throw SystemError(err_write, "FecPipeline write error");
                }
                continue;
            }

            // FEC data shard: [shard_index][source_count][IV][symbol data]
            uint8_t width = (f & kWidth2B) ? 2 : 1;
            uint32_t shard_index, source_count;

            if (width == 1) {
                if (p.DataSize() < 2) continue;
                shard_index = p.PopFrontLE<uint8_t>();
                source_count = p.PopFrontLE<uint8_t>();
            } else {
                if (p.DataSize() < 4) continue;
                shard_index = p.PopFrontLE<uint16_t>();
                source_count = p.PopFrontLE<uint16_t>();
            }

            uint8_t iv_len = (f >> 1) & 0x07;
            if (iv_len > 0 && _Cfg.obfuscate) {
                if (p.DataSize() < iv_len) continue;
                auto iv_span = p.PopFront(iv_len);
                for (size_t i = 0; i < p.DataSize(); i++) {
                    p._Data[p._Offset + i] ^= iv_span[i % iv_len];
                }
            }

            // Store in ring buffer
            auto& slot = FindSlot(group_seq);
            if (slot.group_seq == 0) {
                slot.group_seq = group_seq;
                slot.source_count = source_count;
                slot.first_time = std::chrono::steady_clock::now();
            }

            if (slot.source_count != source_count) {
                slot = RingSlot{};
                slot.group_seq = group_seq;
                slot.source_count = source_count;
                slot.first_time = std::chrono::steady_clock::now();
            }

            // Store the shard
            RingSlot::ShardEntry entry;
            entry.data.assign(p._Data.data() + p._Offset, p._Data.data() + p._Offset + p._Length);
            entry.esi = shard_index;
            slot.shards.push_back(std::move(entry));
            slot.symbol_count = static_cast<uint32_t>(slot.shards.size());

            // Check if we can decode
            if (slot.symbol_count >= slot.source_count) {
                uint32_t symbol_size = _Cfg.symbol_size;
                if (symbol_size == 0) {
                    symbol_size = _Cfg.mtu - 28 - 20;
                }
                uint64_t F = static_cast<uint64_t>(source_count) * symbol_size;

                RaptorQ rq(F, static_cast<uint16_t>(symbol_size));

                for (auto& shard : slot.shards) {
                    if (shard.data.size() < symbol_size) {
                        shard.data.resize(symbol_size, 0);
                    }
                    rq.SubmitSymbol(shard.data.data(), symbol_size, shard.esi);
                }

                std::vector<uint8_t> decoded(F);
                if (rq.TryDecode(decoded.data(), F)) {
                    BOOST_LOG_TRIVIAL(info) << "FecPipeline(" << this
                                            << ") decoded group " << group_seq
                                            << " with " << slot.symbol_count << " symbols";

                    // Split blob into individual packets
                    size_t pos = 0;
                    if (pos + 4 > decoded.size()) {
                        slot = RingSlot{};
                        continue;
                    }
                    uint32_t pkt_count = 0;
                    std::memcpy(&pkt_count, decoded.data() + pos, 4);
                    pos += 4;

                    for (uint32_t i = 0; i < pkt_count && pos + 2 <= decoded.size(); i++) {
                        uint16_t pkt_len = 0;
                        std::memcpy(&pkt_len, decoded.data() + pos, 2);
                        pos += 2;
                        if (pos + pkt_len > decoded.size()) break;

                        Packet out;
                        out.PushBack(std::span<const uint8_t>(decoded.data() + pos, pkt_len));
                        pos += pkt_len;

                        auto err_write = co_await _Out->Write(out, _Stop);
                        if (err_write && IsCritical(err_write)) {
                            BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this
                                                     << ") decoded write error: " << err_write.message();
                            throw SystemError(err_write, "FecPipeline decoded write error");
                        }
                    }
                } else {
                    BOOST_LOG_TRIVIAL(warning) << "FecPipeline(" << this
                                               << ") decode failed for group " << group_seq
                                               << " with " << slot.symbol_count << " symbols";
                }

                slot = RingSlot{};
            }
        }
    }
    co_return;
}

// ==================== Encode Helpers ====================

Omni::Fiber::Coroutine<void> FecPipeline::SendBatch(std::vector<Packet>& batch) {
    assert(!batch.empty());

    uint32_t pkt_count = static_cast<uint32_t>(batch.size());
    uint32_t group_seq = ++_GroupSeq;
    uint32_t flags = BuildFlags();
    uint32_t dw = BuildDword(group_seq, static_cast<uint8_t>(flags));

    if (pkt_count == 1 && _Cfg.repeat_ratio > 0.0f) {
        // REPEAT mode: send multiple copies with repeat flag
        uint32_t copies = static_cast<uint32_t>(std::ceil(_Cfg.repeat_ratio)) + 1;
        auto& pkt = batch[0];

        // Generate IV for obfuscation if enabled
        std::vector<uint8_t> iv;
        if (flags & 0x0E) {
            iv.resize(_Cfg.iv_len);
            for (auto& b : iv) {
                b = static_cast<uint8_t>(std::rand() & 0xFF);
            }
        }

        for (uint32_t i = 0; i < copies && !_Stop.IsTriggered(); i++) {
            Packet out;
            out._Length = 0;
            // Copy packet data
            out.PushBack(pkt.Data());

            // XOR with IV if obfuscation enabled
            if (!iv.empty()) {
                for (size_t j = 0; j < out.DataSize(); j++) {
                    out._Data[out._Offset + j] ^= iv[j % iv.size()];
                }
                out.PushFront(std::span<const uint8_t>(iv));
            }

            // fb byte
            out.PushFrontLE(static_cast<uint8_t>(0));

            // DWORD header with REPEAT flag
            uint32_t repeat_dw = (group_seq & 0xFFFFFF) | static_cast<uint32_t>(flags | kRepeat) << 24;
            out.PushFrontLE(repeat_dw);

            auto err = co_await _Out->Write(out, _Stop);
            if (err && IsCritical(err)) {
                BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") REPEAT write error: " << err.message();
                throw SystemError(err, "FecPipeline REPEAT write error");
            }
        }
    } else {
        // FEC mode: build blob, encode with RaptorQ
        std::vector<uint8_t> blob;
        BuildBlob(batch, blob);

        // Calculate symbol size
        uint32_t symbol_size = _Cfg.symbol_size;
        if (symbol_size == 0) {
            symbol_size = _Cfg.mtu - 28 - 20;
            if (symbol_size < 16) symbol_size = 16;
        }

        // Pad blob to symbol_size boundary
        uint64_t F = blob.size();
        if (F % symbol_size != 0) {
            size_t pad = symbol_size - (F % symbol_size);
            blob.insert(blob.end(), pad, 0);
            F = blob.size();
        }

        uint16_t T = static_cast<uint16_t>(symbol_size);
        RaptorQ rq(F, T);
        rq.Encode(blob.data(), blob.size());
        uint32_t K = rq.K();

        uint32_t extra = static_cast<uint32_t>(std::ceil(K * _Cfg.overhead));
        uint32_t total_symbols = K + extra;
        if (total_symbols > 65535) {
            total_symbols = 65535;
        }

        // Determine field width
        bool use_2b = (total_symbols > 255) || (K > 255);
        uint8_t final_flags = static_cast<uint8_t>(flags);
        if (use_2b) {
            final_flags |= kWidth2B;
        }

        // Generate IV for obfuscation
        std::vector<uint8_t> iv;
        if (final_flags & 0x0E) {
            iv.resize(_Cfg.iv_len);
            for (auto& b : iv) {
                b = static_cast<uint8_t>(std::rand() & 0xFF);
            }
        }

        uint32_t final_dw = BuildDword(group_seq, final_flags);

        for (uint32_t esi = 0; esi < total_symbols && !_Stop.IsTriggered(); esi++) {
            auto* sym_data = rq.GenerateSymbol(esi);

            Packet out;
            out._Length = 0;
            // Push symbol data
            out.PushBack(std::span<const uint8_t>(sym_data, T));

            // XOR obfuscate
            if (!iv.empty()) {
                for (size_t j = 0; j < out.DataSize(); j++) {
                    out._Data[out._Offset + j] ^= iv[j % iv.size()];
                }
                out.PushFront(std::span<const uint8_t>(iv));
            }

            // Header fields (push front in reverse order)
            if (use_2b) {
                out.PushFrontLE(static_cast<uint16_t>(K));
                out.PushFrontLE(static_cast<uint16_t>(esi));
            } else {
                out.PushFrontLE(static_cast<uint8_t>(K));
                out.PushFrontLE(static_cast<uint8_t>(esi));
            }

            out.PushFrontLE(static_cast<uint8_t>(0)); // fb = 0
            out.PushFrontLE(final_dw);

            delete[] sym_data;

            auto err = co_await _Out->Write(out, _Stop);
            if (err && IsCritical(err)) {
                BOOST_LOG_TRIVIAL(error) << "FecPipeline(" << this << ") FEC write error: " << err.message();
                throw SystemError(err, "FecPipeline FEC write error");
            }
        }
    }
    co_return;
}

void FecPipeline::BuildBlob(const std::vector<Packet>& batch, std::vector<uint8_t>& blob) {
    // Format: [u32 count][u16 len0][data0][u16 len1][data1]...
    blob.clear();

    uint32_t count = static_cast<uint32_t>(batch.size());
    blob.resize(4);
    std::memcpy(blob.data(), &count, 4);

    for (auto& pkt : batch) {
        uint16_t len = static_cast<uint16_t>(pkt.DataSize());
        size_t pos = blob.size();
        blob.resize(pos + 2);
        std::memcpy(blob.data() + pos, &len, 2);

        pos = blob.size();
        blob.resize(pos + len);
        std::memcpy(blob.data() + pos, pkt.Data().data(), len);
    }
}

// ==================== Decode Helpers ====================

FecPipeline::RingSlot& FecPipeline::FindSlot(uint32_t group_seq) {
    // Linear search for existing slot
    for (size_t i = 0; i < _RingBuffer.size(); i++) {
        if (_RingBuffer[i].group_seq == group_seq) {
            return _RingBuffer[i];
        }
    }

    // Check decode timeout for existing slots
    EvictStaleSlot();

    // Allocate new slot
    return _RingBuffer[_RingNext++ & _RingMask];
}

bool FecPipeline::EvictStaleSlot() {
    auto now = std::chrono::steady_clock::now();
    uint64_t decode_timeout = _Cfg.decode_timeout_ms;
    if (_RttEwma > 0) {
        uint64_t rtt_based = (3 * _RttEwma / 8) + _Cfg.timeout_ms;
        decode_timeout = std::max(rtt_based, static_cast<uint64_t>(50));
    }

    for (size_t i = 0; i < _RingBuffer.size(); i++) {
        if (_RingBuffer[i].group_seq != 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - _RingBuffer[i].first_time)
                               .count();
            if (static_cast<uint64_t>(elapsed) > decode_timeout) {
                _RingBuffer[i] = RingSlot{};
                return true;
            }
        }
    }
    return false;
}

} // namespace gh
