#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "AdaptiveOverhead.hpp"
#include "RS256.hpp"
#include "RsCodec.hpp"

namespace {

using Bytes = std::vector<uint8_t>;

Bytes MakePayload(size_t length, unsigned seed) {
  Bytes bytes(length);
  for (size_t i = 0; i < length; ++i) {
    bytes[i] = static_cast<uint8_t>(i * 13 + seed * 47);
  }
  return bytes;
}

gh::Packet MakePacket(const Bytes& bytes) {
  gh::Packet packet(0);
  packet._Data.resize(gh::Packet::kReservedFront + bytes.size());
  packet.PushBack(bytes);
  return packet;
}

void DrainRepairs(gh::RsCodec& encoder, std::vector<gh::Packet>& wire, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    encoder.Tick(wire);
  }
}

TEST(RS256Test, AppliesNonUnitFirstInverseCoefficient) {
  constexpr unsigned kSymbols = 4;
  constexpr unsigned kSize = 600;
  std::vector<Bytes> source;
  for (unsigned i = 0; i < kSymbols; ++i) {
    source.push_back(MakePayload(kSize, i));
  }
  std::vector<Bytes> repairs;
  gh::RS256::EncodeRepair(source, kSize, gh::RS256::BuildCoeffs(kSymbols, 2), repairs);
  std::vector<Bytes> known, rows, recovered;
  for (unsigned i : {0u, 2u, 3u}) {
    known.push_back(source[i]);
    rows.emplace_back(kSymbols, 0);
    rows.back()[i] = 1;
  }
  known.push_back(repairs[0]);
  rows.push_back(gh::RS256::RepairRow(kSymbols, 0));
  ASSERT_TRUE(gh::RS256::Decode(known, kSize, rows, recovered));
  EXPECT_EQ(recovered, source);
}

class RsRecoveryTest : public ::testing::TestWithParam<std::tuple<int, bool, bool>> {};

TEST_P(RsRecoveryTest, PreservesExactPayloads) {
  const auto [drop, boundary, highRepairIndex] = GetParam();
  gh::FecConfig cfg;
  cfg.symbol_size = 1440;
  cfg.max_batch = 4;
  cfg.overhead = highRepairIndex ? 1.5f : 0.5f;
  cfg.max_overhead = 1.5f;
  cfg.timeout_ms = 60000;
  gh::RsCodec encoder(cfg, true, nullptr, nullptr, nullptr);
  gh::RsCodec decoder(cfg, false, nullptr, nullptr, nullptr);
  std::vector<Bytes> expected;
  std::vector<gh::Packet> wire, decoded;
  for (unsigned i = 0; i < cfg.max_batch; ++i) {
    expected.push_back(MakePayload(boundary ? cfg.symbol_size - 2 : 300 + i * 311, i));
    encoder.OnPacket(MakePacket(expected.back()), wire);
  }
  ASSERT_EQ(wire.size(), cfg.max_batch);
  for (const auto& packet : wire) {
    // Five wire bytes precede the length-prefixed RS symbol.
    EXPECT_EQ(packet.DataSize(), cfg.symbol_size + 5);
  }
  DrainRepairs(encoder, wire, highRepairIndex ? 6 : 2);
  ASSERT_EQ(wire.size(), cfg.max_batch + (highRepairIndex ? 6 : 2));
  for (auto& packet : wire) {
    const auto data = packet.Data();
    const unsigned seq = data[0] | (unsigned(data[1]) << 8) | (unsigned(data[2]) << 16);
    const bool repair = data[3] & gh::fec_wire::kRsRepair;
    if ((!repair && int(seq) == drop + 1) || (repair && highRepairIndex && data[9] < cfg.max_batch)) {
      continue;
    }
    decoder.OnPacket(std::move(packet), decoded);
  }
  std::vector<Bytes> actual;
  for (const auto& packet : decoded) {
    actual.emplace_back(packet.Data().begin(), packet.Data().end());
  }
  std::sort(expected.begin(), expected.end());
  std::sort(actual.begin(), actual.end());
  EXPECT_EQ(actual, expected);
}

INSTANTIATE_TEST_SUITE_P(RoundTrip, RsRecoveryTest,
                        ::testing::Values(std::tuple{-1, false, false}, std::tuple{0, false, false},
                                          std::tuple{1, false, false}, std::tuple{3, false, false},
                                          std::tuple{-1, true, false}, std::tuple{1, true, false},
                                          std::tuple{0, false, true}, std::tuple{1, false, true},
                                          std::tuple{3, false, true}));

TEST(RsCodecTest, RejectsSourceLargerThanSymbol) {
  gh::FecConfig cfg;
  cfg.symbol_size = 300;
  gh::RsCodec decoder(cfg, false, nullptr, nullptr, nullptr);
  auto packet = MakePacket(MakePayload(299, 1));
  packet.PushFrontLE(uint16_t{299});
  packet.PushFrontLE(uint8_t{0});
  packet.PushFrontLE(uint32_t{1});
  std::vector<gh::Packet> decoded;
  decoder.OnPacket(std::move(packet), decoded);
  EXPECT_TRUE(decoded.empty());
}

TEST(RsCodecTest, ClearsPaddingWhenSourceSlotsAreReused) {
  gh::FecConfig cfg;
  cfg.symbol_size = 1440;
  cfg.max_batch = 4;
  cfg.overhead = 0.5f;
  cfg.timeout_ms = 60000;
  gh::RsCodec encoder(cfg, true, nullptr, nullptr, nullptr);
  gh::RsCodec decoder(cfg, false, nullptr, nullptr, nullptr);
  std::vector<gh::Packet> wire, decoded;
  std::vector<Bytes> expected, actual;
  for (unsigned i = 0; i < cfg.max_batch; ++i) {
    auto old = MakePacket(MakePayload(cfg.symbol_size - 2, i));
    old.PushFrontLE(static_cast<uint16_t>(cfg.symbol_size - 2));
    old.PushFrontLE(uint8_t{0});
    old.PushFrontLE(i + 1);
    decoder.OnPacket(std::move(old), decoded);
    expected.push_back(MakePayload(300 + i * 50, i));
    encoder.OnPacket(MakePacket(expected.back()), wire);
  }
  decoded.clear();
  DrainRepairs(encoder, wire, 2);
  for (auto& packet : wire) {
    auto data = packet.Data();
    const bool repair = data[3] & gh::fec_wire::kRsRepair;
    if (!repair && data[0] == 2) {
      continue;
    }
    // Advance the batch by one 4096-slot source-ring cycle.
    data[1] = 0x10;
    if (repair) {
      data[6] = 0x10;
    }
    decoder.OnPacket(std::move(packet), decoded);
  }
  for (const auto& packet : decoded) {
    actual.emplace_back(packet.Data().begin(), packet.Data().end());
  }
  std::sort(expected.begin(), expected.end());
  std::sort(actual.begin(), actual.end());
  EXPECT_EQ(actual, expected);
}

class RsOverheadTest : public ::testing::TestWithParam<int> {};

TEST_P(RsOverheadTest, KeepsConfiguredRepairFloorAfterCleanSamples) {
  gh::FecConfig cfg;
  cfg.symbol_size = 1300;
  cfg.max_batch = 20;
  cfg.timeout_ms = 60000;
  cfg.overhead = 1.05f;
  cfg.max_overhead = 1.5f;
  auto shared = std::make_shared<gh::FecSharedState>();
  auto controller = gh::AdaptiveOverhead::Create(GetParam(), cfg.overhead, cfg.max_overhead);
  for (int i = 0; i < 2000; ++i) {
    controller->Update(0);
  }
  gh::RsCodec encoder(cfg, true, shared, controller.get(), nullptr);
  std::vector<gh::Packet> wire;
  for (unsigned i = 0; i < cfg.max_batch; ++i) {
    encoder.OnPacket(MakePacket(MakePayload(1000, i)), wire);
  }
  DrainRepairs(encoder, wire, 31);
  const auto repairs = std::count_if(wire.begin(), wire.end(), [](const gh::Packet& packet) {
    return packet.Data()[3] & gh::fec_wire::kRsRepair;
  });
  EXPECT_GE(repairs, 21);
  EXPECT_LE(repairs, 30);
}

INSTANTIATE_TEST_SUITE_P(AllAlgorithms, RsOverheadTest, ::testing::Range(0, 8));

TEST(RsCodecTest, ClampsSmallPacketRedundancyToConfiguredFloor) {
  gh::FecConfig cfg;
  cfg.overhead = 0.8f;
  cfg.max_overhead = 1.5f;
  gh::AlgoStatic controller(0);
  gh::RsCodec encoder(cfg, true, nullptr, &controller, nullptr);
  std::vector<gh::Packet> wire;
  encoder.OnPacket(MakePacket(MakePayload(100, 1)), wire);
  EXPECT_EQ(wire.size(), 5);
}

TEST(RsCodecTest, ClampsRepairCountToConfiguredMaximum) {
  gh::FecConfig cfg;
  cfg.symbol_size = 1300;
  cfg.max_batch = 4;
  cfg.overhead = 0.5f;
  cfg.max_overhead = 1.5f;
  cfg.timeout_ms = 60000;
  gh::AlgoStatic controller(3.0f);
  gh::RsCodec encoder(cfg, true, nullptr, &controller, nullptr);
  std::vector<gh::Packet> wire;
  for (unsigned i = 0; i < cfg.max_batch; ++i) {
    encoder.OnPacket(MakePacket(MakePayload(1000, i)), wire);
  }
  DrainRepairs(encoder, wire, 12);
  EXPECT_EQ(wire.size(), 10);
}

TEST(RsCodecTest, ExplicitLossDeadbandStillSuppressesRepairs) {
  gh::FecConfig cfg;
  cfg.symbol_size = 1300;
  cfg.max_batch = 4;
  cfg.overhead = 0.5f;
  cfg.loss_deadband = 0;
  cfg.timeout_ms = 60000;
  auto shared = std::make_shared<gh::FecSharedState>();
  gh::RsCodec encoder(cfg, true, shared, nullptr, nullptr);
  std::vector<gh::Packet> wire;
  for (unsigned i = 0; i < cfg.max_batch; ++i) {
    encoder.OnPacket(MakePacket(MakePayload(1000, i)), wire);
  }
  DrainRepairs(encoder, wire, 3);
  EXPECT_EQ(wire.size(), cfg.max_batch);
}

} // namespace
