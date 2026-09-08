#pragma once

#include <cstdint>
#include <vector>

namespace gh {

// GF(256) Vandermonde systematic Reed-Solomon (RFC 5510 style).
//
// - Systematic: the first k shards are the raw source data, sent immediately
//   (zero batch delay on the wire).
// - Repair shards are independent GF linear combinations:
//       repair_j[b] = SUM_i x_j^i * src[i][b],  x_j = alpha^(k+j)
//   Any number 0..(255-k) of repair shards may be generated on demand, giving
//   the same adaptive redundancy freedom as a rateless code in that range.
// - Decode: given any k shards (source unit rows + Vandermonde repair rows),
//   invert the k x k coefficient matrix via GF(256) Gaussian elimination.
//
// Symbol size T is fixed for the whole batch (all shards T bytes).
class RS256 {
public:
    // Vandermonde coefficient row for repair index j of a k-source batch:
    // [1, x_j, x_j^2, ..., x_j^(k-1)], x_j = alpha^(k+j).  k bytes.
    static std::vector<uint8_t> RepairRow(uint32_t k, uint32_t j);

    // Coefficient rows for repair indices 0..m-1 (m*k bytes, row-major).
    static std::vector<uint8_t> BuildCoeffs(uint32_t k, uint32_t m);

    // Generate m repair shards from k source shards (each T bytes).
    // coeffs must be m*k bytes from BuildCoeffs. repairs[j] is T bytes.
    static void EncodeRepair(const std::vector<std::vector<uint8_t>>& src, uint32_t T,
                             const std::vector<uint8_t>& coeffs,
                             std::vector<std::vector<uint8_t>>& repairs);

    // Recover the k source shards from any k known shards.
    // known_rows[i] is the coefficient row of known shard i:
    //   source shard s   -> unit row e_s (k bytes, 1 at position s)
    //   repair shard j   -> RepairRow(k, j)
    // On success returns true and out_src[i] holds the i-th source shard (T bytes).
    static bool Decode(const std::vector<std::vector<uint8_t>>& known, uint32_t T,
                       const std::vector<std::vector<uint8_t>>& known_rows,
                       std::vector<std::vector<uint8_t>>& out_src);

private:
    static void InitTables();
    static inline uint8_t Mul(uint8_t a, uint8_t b);
};

} // namespace gh
