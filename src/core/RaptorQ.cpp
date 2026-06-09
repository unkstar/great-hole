#include "RaptorQ.hpp"

#include <cstring>
#include <stdexcept>

extern "C" {
#include <lcrq.h>
}

namespace gh {

RaptorQ::RaptorQ(uint64_t F, uint16_t T)
    : _rq(nullptr), _F(F), _T(T), _K(0), _encoded(false), _nesi(0) {
    if (F == 0 || T == 0) {
        throw std::invalid_argument("RaptorQ: F and T must be > 0");
    }
    _rq = rq_init(F, T);
    if (!_rq) {
        throw std::runtime_error("RaptorQ: rq_init failed");
    }
    _K = rq_K(static_cast<const rq_t*>(_rq));
}

RaptorQ::~RaptorQ() {
    if (_rq) {
        rq_free(static_cast<rq_t*>(_rq));
    }
}

void RaptorQ::Encode(const uint8_t* blob, size_t blob_len) {
    auto* rq = static_cast<rq_t*>(_rq);
    // rq_encode takes non-const data (it may modify in-place), but it doesn't actually modify the source
    if (rq_encode(rq, const_cast<uint8_t*>(blob), blob_len) != 0) {
        throw std::runtime_error("RaptorQ: rq_encode failed");
    }
    _encoded = true;
}

uint32_t RaptorQ::K() const {
    return _K;
}

uint8_t* RaptorQ::GenerateSymbol(uint32_t esi) {
    if (!_encoded) {
        throw std::logic_error("RaptorQ: must call Encode() before GenerateSymbol()");
    }
    auto* rq = static_cast<rq_t*>(_rq);
    rq_pid_t pid = rq_pidset(0, esi);
    auto* sym = new uint8_t[_T];
    auto* result = rq_symbol(rq, &pid, sym, 0);
    if (!result) {
        delete[] sym;
        throw std::runtime_error("RaptorQ: rq_symbol failed for ESI " + std::to_string(esi));
    }
    return sym;
}

void RaptorQ::SubmitSymbol(const uint8_t* symbol, size_t symbol_len, uint32_t esi) {
    if (symbol_len != _T) {
        throw std::invalid_argument("RaptorQ: symbol size mismatch, expected " + std::to_string(_T)
                                    + " got " + std::to_string(symbol_len));
    }

    // Reallocate if needed
    if (_nesi == 0) {
        // Preallocate for up to K + overhead symbols
        _enc = std::make_unique<uint8_t[]>(_K * _T * 2);
        _esi = std::make_unique<uint32_t[]>(_K * 2);
    }

    std::memcpy(_enc.get() + _nesi * _T, symbol, _T);
    _esi[_nesi] = esi;
    _nesi++;
}

bool RaptorQ::TryDecode(uint8_t* blob, size_t blob_len) {
    auto* rq = static_cast<rq_t*>(_rq);
    // rq_decode: args are (rq, decoded_output, encoded_input_concatenated, ESI_array, nesi)
    int ret = rq_decode(rq, blob, _enc.get(), _esi.get(), _nesi);
    return ret == 0;
}

} // namespace gh
