#include "RS256.hpp"

#include <cstring>

namespace gh {

namespace {
// GF(256) tables over polynomial 0x11D
uint8_t g_exp[512];
uint8_t g_log[256];
uint8_t g_inv[256];
bool g_tables_ready = false;

void ensure_tables() {
    if (g_tables_ready) return;
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        g_exp[i] = static_cast<uint8_t>(x);
        g_log[x] = static_cast<uint8_t>(i);
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) g_exp[i] = g_exp[i - 255];
    for (int i = 1; i < 256; i++) g_inv[i] = g_exp[255 - g_log[i]];
    g_tables_ready = true;
}

inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    return (a == 0 || b == 0) ? 0 : g_exp[static_cast<int>(g_log[a]) + g_log[b]];
}

// Gaussian elimination on augmented [a | inv] over GF(256).
// a is k x k (in-place destroyed), inv starts as identity, ends as a^-1.
// Returns false if the matrix is singular.
bool invert_matrix(std::vector<std::vector<uint8_t>>& a, std::vector<std::vector<uint8_t>>& inv) {
    const size_t k = a.size();
    for (size_t col = 0; col < k; col++) {
        size_t pivot = k;
        for (size_t r = col; r < k; r++) {
            if (a[r][col] != 0) { pivot = r; break; }
        }
        if (pivot == k) return false;
        if (pivot != col) {
            std::swap(a[pivot], a[col]);
            std::swap(inv[pivot], inv[col]);
        }
        // normalize pivot row
        const uint8_t f = g_inv[a[col][col]];
        for (size_t c = col; c < k; c++) a[col][c] = gf_mul(a[col][c], f);
        for (size_t c = 0; c < k; c++) inv[col][c] = gf_mul(inv[col][c], f);
        // eliminate other rows
        for (size_t r = 0; r < k; r++) {
            if (r == col || a[r][col] == 0) continue;
            const uint8_t factor = a[r][col];
            for (size_t c = col; c < k; c++) a[r][c] ^= gf_mul(factor, a[col][c]);
            for (size_t c = 0; c < k; c++) inv[r][c] ^= gf_mul(factor, inv[col][c]);
        }
    }
    return true;
}

} // namespace

void RS256::InitTables() { ensure_tables(); }

uint8_t RS256::Mul(uint8_t a, uint8_t b) { return gf_mul(a, b); }

std::vector<uint8_t> RS256::RepairRow(uint32_t k, uint32_t j) {
    ensure_tables();
    std::vector<uint8_t> row(k);
    uint8_t x = g_exp[(k + j) % 255];  // evaluation point, distinct for all j < 255-k
    uint8_t p = 1;
    for (uint32_t i = 0; i < k; i++) {
        row[i] = p;
        p = gf_mul(p, x);
    }
    return row;
}

std::vector<uint8_t> RS256::BuildCoeffs(uint32_t k, uint32_t m) {
    std::vector<uint8_t> coeffs;
    coeffs.reserve((size_t)m * k);
    for (uint32_t j = 0; j < m; j++) {
        auto row = RepairRow(k, j);
        coeffs.insert(coeffs.end(), row.begin(), row.end());
    }
    return coeffs;
}

void RS256::EncodeRepair(const std::vector<std::vector<uint8_t>>& src, uint32_t T,
                         const std::vector<uint8_t>& coeffs,
                         std::vector<std::vector<uint8_t>>& repairs) {
    ensure_tables();
    const uint32_t k = static_cast<uint32_t>(src.size());
    const uint32_t m = coeffs.size() / k;
    repairs.resize(m);
    for (uint32_t j = 0; j < m; j++) {
        std::vector<uint8_t>& out = repairs[j];
        out.assign(T, 0);
        const uint8_t* c = coeffs.data() + (size_t)j * k;
        // find first nonzero coefficient for initialization
        int first = -1;
        for (uint32_t i = 0; i < k; i++) {
            if (c[i]) { first = static_cast<int>(i); break; }
        }
        if (first < 0) continue;
        std::memcpy(out.data(), src[first].data(), T);
        for (uint32_t i = static_cast<uint32_t>(first) + 1; i < k; i++) {
            const uint8_t y = c[i];
            if (y == 0) continue;
            const uint8_t* s = src[i].data();
            for (uint32_t b = 0; b < T; b++) {
                out[b] ^= gf_mul(y, s[b]);
            }
        }
    }
}

bool RS256::Decode(const std::vector<std::vector<uint8_t>>& known, uint32_t T,
                   const std::vector<std::vector<uint8_t>>& known_rows,
                   std::vector<std::vector<uint8_t>>& out_src) {
    ensure_tables();
    const size_t k = known.size();
    if (k == 0 || known_rows.size() != k) return false;
    // build matrix A from known_rows
    std::vector<std::vector<uint8_t>> a(k, std::vector<uint8_t>(k));
    for (size_t r = 0; r < k; r++) {
        if (known_rows[r].size() != k) return false;
        std::memcpy(a[r].data(), known_rows[r].data(), k);
    }
    std::vector<std::vector<uint8_t>> inv(k, std::vector<uint8_t>(k, 0));
    for (size_t i = 0; i < k; i++) inv[i][i] = 1;
    if (!invert_matrix(a, inv)) return false;
    // out_src[i] = SUM_r inv[i][r] * known[r]
    out_src.assign(k, std::vector<uint8_t>(T, 0));
    for (size_t i = 0; i < k; i++) {
        uint8_t* out = out_src[i].data();
        // initialize with first nonzero coefficient
        int first = -1;
        for (size_t r = 0; r < k; r++) {
            if (inv[i][r]) { first = static_cast<int>(r); break; }
        }
        if (first < 0) continue;
        std::memcpy(out, known[first].data(), T);
        for (size_t r = static_cast<size_t>(first) + 1; r < k; r++) {
            const uint8_t y = inv[i][r];
            if (y == 0) continue;
            const uint8_t* s = known[r].data();
            for (uint32_t b = 0; b < T; b++) {
                out[b] ^= gf_mul(y, s[b]);
            }
        }
    }
    return true;
}

} // namespace gh
