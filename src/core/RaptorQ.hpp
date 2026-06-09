#pragma once

#include <cstdint>
#include <memory>

namespace gh {

// RaptorQ Encoder/Decoder wrapper around lcrq C library
// Usage:
//   Encode:
//     RaptorQ rq(F, T);
//     rq.Encode(blob_data, blob_len);
//     for (uint32_t esi = 0; esi < rq.K() + extra; esi++) {
//       auto sym = rq.GenerateSymbol(esi);
//     }
//   Decode:
//     RaptorQ rq(F, T);
//     rq.SubmitSymbol(symbol_data, symbol_len, esi);
//     // When enough symbols received:
//     if (rq.TryDecode(output_buffer, F)) { ... }

class RaptorQ {
public:
    RaptorQ(uint64_t F, uint16_t T);
    ~RaptorQ();
    RaptorQ(const RaptorQ&) = delete;
    RaptorQ& operator=(const RaptorQ&) = delete;
    RaptorQ(RaptorQ&&) = delete;
    RaptorQ& operator=(RaptorQ&&) = delete;

    // Encode mode: set source blob
    void Encode(const uint8_t* blob, size_t blob_len);

    // Number of source symbols (K)
    uint32_t K() const;

    // Generate encoded symbol at given ESI, returns symbol data (T bytes)
    // Caller owns the returned buffer
    uint8_t* GenerateSymbol(uint32_t esi);

    // Decode mode: submit a received symbol
    void SubmitSymbol(const uint8_t* symbol, size_t symbol_len, uint32_t esi);

    // Try to decode. Returns true if decoding succeeded.
    // On success, blob contains the decoded data (F bytes).
    bool TryDecode(uint8_t* blob, size_t blob_len);

    uint64_t F() const { return _F; }
    uint16_t T() const { return _T; }

private:
    void* _rq;        // rq_t* from lcrq
    uint64_t _F;      // Transfer Length (blob size)
    uint16_t _T;      // Symbol Size
    uint32_t _K;      // Number of source symbols
    bool _encoded;    // Whether Encode() has been called

    // For decode mode
    std::unique_ptr<uint8_t[]> _enc;   // concatenated encoded symbols
    std::unique_ptr<uint32_t[]> _esi;  // ESI values
    uint32_t _nesi;                    // number of submitted symbols
};

} // namespace gh
