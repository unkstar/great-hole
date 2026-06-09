#include <gtest/gtest.h>
#include <cstring>
#include <vector>

#include "RaptorQ.hpp"

class RaptorQTest : public ::testing::Test {
protected:
    std::vector<uint8_t> MakeBlob(size_t size) {
        std::vector<uint8_t> blob(size);
        for (size_t i = 0; i < size; i++) {
            blob[i] = static_cast<uint8_t>(i * 7 + 13);
        }
        return blob;
    }
};

TEST_F(RaptorQTest, InitFailsOnZeroParams) {
    EXPECT_THROW(gh::RaptorQ(0, 128), std::invalid_argument);
    EXPECT_THROW(gh::RaptorQ(1024, 0), std::invalid_argument);
}

TEST_F(RaptorQTest, InitSucceedsWithValidParams) {
    EXPECT_NO_THROW(gh::RaptorQ(1024, 128));
    EXPECT_NO_THROW(gh::RaptorQ(1400, 128));
    EXPECT_NO_THROW(gh::RaptorQ(128, 128));
}

TEST_F(RaptorQTest, KIsCorrectForAlignedSize) {
    // 1024 / 128 = 8 symbols
    gh::RaptorQ rq(1024, 128);
    EXPECT_EQ(rq.K(), 8);
}

TEST_F(RaptorQTest, EncodeWithoutBlobThrows) {
    gh::RaptorQ rq(1024, 128);
    EXPECT_THROW(rq.GenerateSymbol(0), std::logic_error);
}

TEST_F(RaptorQTest, EncodedSymbolHasCorrectSize) {
    auto blob = MakeBlob(1024);
    gh::RaptorQ rq(1024, 128);
    rq.Encode(blob.data(), blob.size());

    auto* sym = rq.GenerateSymbol(0);
    // Symbol should have size T = 128
    EXPECT_NE(sym, nullptr);
    delete[] sym;

    // Generate multiple symbols
    for (uint32_t i = 1; i < rq.K(); i++) {
        sym = rq.GenerateSymbol(i);
        EXPECT_NE(sym, nullptr);
        delete[] sym;
    }
}

TEST_F(RaptorQTest, EncodeDecodeRoundTripNoLoss) {
    auto blob = MakeBlob(1024);
    gh::RaptorQ enc(1024, 128);
    enc.Encode(blob.data(), blob.size());

    // Simulate all source symbols being sent and received
    gh::RaptorQ dec(1024, 128);
    for (uint32_t i = 0; i < enc.K(); i++) {
        auto* sym = enc.GenerateSymbol(i);
        dec.SubmitSymbol(sym, 128, i);
        delete[] sym;
    }

    std::vector<uint8_t> decoded(1024);
    EXPECT_TRUE(dec.TryDecode(decoded.data(), decoded.size()));
    EXPECT_EQ(decoded, blob);
}

TEST_F(RaptorQTest, EncodeDecodeRoundTripWithRepairSymbols) {
    // Test with 8 source + 2 repair symbols
    auto blob = MakeBlob(1024);
    gh::RaptorQ enc(1024, 128);
    enc.Encode(blob.data(), blob.size());

    uint32_t K = enc.K();
    ASSERT_EQ(K, 8);

    // Send K+K symbols (K source + K repair)
    std::vector<std::vector<uint8_t>> symbols;
    for (uint32_t i = 0; i < K + K; i++) {
        auto* sym = enc.GenerateSymbol(i);
        symbols.emplace_back(sym, sym + 128);
        delete[] sym;
    }

    // Decode with just first K symbols (simulate no loss)
    {
        gh::RaptorQ dec(1024, 128);
        for (uint32_t i = 0; i < K; i++) {
            dec.SubmitSymbol(symbols[i].data(), 128, i);
        }
        std::vector<uint8_t> decoded(1024);
        EXPECT_TRUE(dec.TryDecode(decoded.data(), decoded.size()));
        EXPECT_EQ(decoded, blob);
    }

    // Decode with only repair symbols (ESI >= K)
    {
        gh::RaptorQ dec(1024, 128);
        // Submit K repair symbols (ESI K..2K-1) to decode entire block
        for (uint32_t i = K; i < K + K; i++) {
            dec.SubmitSymbol(symbols[i].data(), 128, i);
        }
        std::vector<uint8_t> decoded(1024);
        EXPECT_TRUE(dec.TryDecode(decoded.data(), decoded.size()));
        EXPECT_EQ(decoded, blob);
    }
}

TEST_F(RaptorQTest, EncodeDecodeWithMixedSymbols) {
    // Test recovery with mixed source + repair symbols
    auto blob = MakeBlob(1024);
    gh::RaptorQ enc(1024, 128);
    enc.Encode(blob.data(), blob.size());

    uint32_t K = enc.K();

    // Generate all symbols
    std::vector<std::vector<uint8_t>> symbols;
    for (uint32_t i = 0; i < K + 4; i++) {
        auto* sym = enc.GenerateSymbol(i);
        symbols.emplace_back(sym, sym + 128);
        delete[] sym;
    }

    // Submit K mixed symbols (some source, some repair)
    // Submit ESI 0,2,4,6 (source) + ESI 8,9,10,11 (repair) = 8 total
    gh::RaptorQ dec(1024, 128);
    std::vector<uint32_t> esis = {0, 2, 4, 6, 8, 9, 10, 11};
    for (auto esi : esis) {
        ASSERT_LT(esi, symbols.size());
        dec.SubmitSymbol(symbols[esi].data(), 128, esi);
    }
    std::vector<uint8_t> decoded(1024);
    EXPECT_TRUE(dec.TryDecode(decoded.data(), decoded.size()));
    EXPECT_EQ(decoded, blob);
}

TEST_F(RaptorQTest, SmallBlobSingleSymbol) {
    // Minimum blob: 1 symbol
    auto blob = MakeBlob(128);
    gh::RaptorQ enc(128, 128);
    enc.Encode(blob.data(), blob.size());
    EXPECT_EQ(enc.K(), 1);

    // Generate 3 symbols (1 source + 2 repair)
    auto* sym0 = enc.GenerateSymbol(0);
    auto* sym1 = enc.GenerateSymbol(1);
    auto* sym2 = enc.GenerateSymbol(2);

    // Decode from repair symbol (ESI=1)
    gh::RaptorQ dec(128, 128);
    dec.SubmitSymbol(sym1, 128, 1);
    std::vector<uint8_t> decoded(128);
    EXPECT_TRUE(dec.TryDecode(decoded.data(), decoded.size()));
    EXPECT_EQ(decoded, blob);

    delete[] sym0;
    delete[] sym1;
    delete[] sym2;
}
