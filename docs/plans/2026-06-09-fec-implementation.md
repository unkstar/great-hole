# FEC Integration Implementation Plan

> **For implementer:** Use TDD throughout. Write failing test first. Watch it fail. Then implement.

**Goal:** Integrate RaptorQ-based Forward Error Correction (FEC) into great-hole's Pipeline with Lua configuration support.

**Architecture:** FecPipeline extends Pipeline with a batch-mode `Process()`. lcrq (RaptorQ) is added as a git submodule. FEC encode accumulates packets, encodes via RaptorQ, and writes headers; FEC decode reads headers, buffers shards, and decodes groups. Feedback loop with PING/RTT measurement adapts overhead dynamically.

**Tech Stack:** C++23, Boost.Asio, omni-fiber coroutines, lcrq (C, RFC 6330), Lua 5.4, CMake, GTest

---

## Phase 1: lcrq Integration + Packet LE Support

### Task 1.1: Add lcrq git submodule

**Files:**
- Modify: `.gitmodules`
- Create: `libs/lcrq/` (submodule)

**Step 1:** Add submodule
```bash
cd ~/great-hole
git submodule add https://git.sr.ht/~librecast/lcrq libs/lcrq
git submodule update --init --recursive
```

**Step 2:** Verify
```bash
ls libs/lcrq/src/rq.h         # Main header exists
ls libs/lcrq/configure        # Autotools build exists
```

**Step 3:** Commit
```bash
git add .gitmodules libs/lcrq
git commit -m "feat: add lcrq git submodule for RaptorQ FEC"
```

### Task 1.2: Integrate lcrq into CMake build

**Files:**
- Modify: `CMakeLists.txt` (root)
- Modify: `src/core/CMakeLists.txt`

**Step 1:** Write failing test — verify lcrq is NOT yet linkable
```bash
# Add to src/core/tests/CMakeLists.txt:
# add_executable(TestLcrq TestLcrq.cpp)
# target_link_libraries(TestLcrq gtest_main lcrq)

# Create src/core/tests/TestLcrq.cpp:
# TEST(LcrqTest, CompilesAndLinks) { FAIL() << "lcrq not integrated"; }
```
Expected: FAIL/compile error

**Step 2:** Add lcrq CMake integration in root `CMakeLists.txt`
```cmake
# In root CMakeLists.txt, before src subdirectory:
add_subdirectory(libs/lcrq EXCLUDE_FROM_ALL)
```

**Step 3:** Configure lcrq as CMake library
Create `libs/lcrq/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(lcrq C)

set(LCRQ_SRC
    src/rq.c
    src/rq_decode.c
    src/rq_encode.c
    src/rq_init.c
    src/rq_free.c
    # Add other .c files as needed
)

add_library(lcrq STATIC ${LCRQ_SRC})
target_include_directories(lcrq PUBLIC src)
target_compile_options(lcrq PRIVATE -O3)

# Enable SIMD
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(lcrq PRIVATE -mssse3)
endif()
```

**Step 4:** Link lcrq in src/core/CMakeLists.txt
```cmake
target_link_libraries(great-hole-core PUBLIC lcrq)
```

**Step 5:** Verify builds
```bash
cd ~/great-hole/build && cmake .. && make -j4
```

**Step 6:** Commit
```bash
git add CMakeLists.txt libs/lcrq/CMakeLists.txt src/core/CMakeLists.txt
git commit -m "build: integrate lcrq into CMake build system"
```

### Task 1.3: C++ wrapper for lcrq (rq_init/encode/decode)

**Files:**
- Create: `src/core/RaptorQ.hpp`
- Create: `src/core/RaptorQ.cpp`
- Modify: `src/core/CMakeLists.txt`

**Step 1:** Add TestLcrq test — encode/decode round-trip
```cpp
#include <gtest/gtest.h>
#include "RaptorQ.hpp"

TEST(RaptorQ, EncodeDecodeRoundTrip) {
    constexpr uint32_t F = 1024;  // blob size
    constexpr uint16_t T = 128;   // symbol size
    constexpr uint32_t K = F / T; // source symbols = 8
    
    gh::RaptorQ enc(F, T);
    std::vector<uint8_t> blob(F);
    for (uint32_t i = 0; i < F; i++) blob[i] = (uint8_t)(i & 0xFF);
    
    // Encode symbols 0..K+1 (2 extras as FEC)
    enc.Encode(blob.data(), F);
    std::vector<std::vector<uint8_t>> symbols(K + 2);
    for (uint32_t i = 0; i < K + 2; i++) {
        symbols[i].resize(T);
        enc.EncodeSymbol(i, symbols[i].data(), T);
    }
    
    // Decode using first K symbols (drop the 2 FEC symbols)
    gh::RaptorQ dec(F, T);
    uint32_t ids[] = {0, 1, 2, 3, 4, 5, 6, 7};
    dec.Decode(symbols[0].data(), T, ids[0], 0);
    // ... etc
    // Verify decoded blob matches original
}
```

Expected: FAIL (RaptorQ.hpp doesn't exist)

**Step 2:** Implement `src/core/RaptorQ.hpp`
```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace gh {
class RaptorQ {
public:
    RaptorQ(uint32_t F, uint16_t T);
    ~RaptorQ();
    
    void Encode(const uint8_t* blob, uint32_t blob_size);
    uint32_t EncodeSymbol(uint32_t esi, uint8_t* out, uint32_t out_len);
    void Decode(const uint8_t* symbol, uint32_t symbol_len, uint32_t esi, uint32_t count);
    bool TryDecode(uint8_t* blob, uint32_t blob_size);
    
private:
    void* _rq;
    uint32_t _F;
    uint16_t _T;
    uint32_t _K;
};
}
```

**Step 3:** Implement `src/core/RaptorQ.cpp` wrapping lcrq C API

**Step 4:** Verify tests pass

**Step 5:** Commit
```bash
git add src/core/RaptorQ.hpp src/core/RaptorQ.cpp src/core/tests/TestLcrq.cpp src/core/tests/CMakeLists.txt
git commit -m "feat: add C++ RaptorQ wrapper around lcrq"
```

### Task 1.4: Packet PushFrontLE / PopFrontLE for multi-byte LE

**Files:**
- Modify: `src/core/Packet.hpp`
- Create: `src/core/tests/TestPacket.cpp`
- Modify: `src/core/tests/CMakeLists.txt`

**Step 1:** Write failing test
```cpp
TEST(PacketTest, PushFrontLE_Uint16) {
    Packet p(100, 20);
    p.PushFrontLE(uint16_t(0xABCD));
    EXPECT_EQ(p.FrontSpace(), 18);
    // Verify LE on wire
}

TEST(PacketTest, PushFrontLE_Uint32) {
    Packet p(100, 20);
    p.PushFrontLE(uint32_t(0x12345678));
    EXPECT_EQ(p.FrontSpace(), 16);
    // Verify LE encoding
}

TEST(PacketTest, PopFrontLE_Uint16) {
    Packet p(100, 20);
    p.PushFrontLE(uint16_t(0xABCD));
    uint16_t v = p.PopFrontLE<uint16_t>();
    EXPECT_EQ(v, 0xABCD);
}
```

Expected: FAIL (methods don't exist)

**Step 2:** Add PushFrontLE / PopFrontLE templates to Packet.hpp
```cpp
template <typename T>
void PushFrontLE(T value) {
    for (size_t i = 0; i < sizeof(T); i++) {
        PushFront((uint8_t)(value & 0xFF));
        value >>= 8;
    }
}

template <typename T>
T PopFrontLE() {
    T value = 0;
    for (size_t i = 0; i < sizeof(T); i++) {
        value |= ((T)PopFront(1) << (i * 8));
    }
    return value;
}
```

**Step 3:** Verify tests pass

**Step 4:** Commit
```bash
git add src/core/Packet.hpp src/core/tests/TestPacket.cpp
git commit -m "feat: add PushFrontLE/PopFrontLE for LE multi-byte Packet ops"
```

## Phase 2: Pipeline Refactor + FecConfig + Lua

### Task 2.1: Extract Pipeline::Process() virtual method

**Files:**
- Modify: `src/core/Pipeline.hpp`
- Modify: `src/core/Pipeline.cpp`

**Step 1:** Refactor — move the read-filter-write loop into a virtual `Process()` method. The Start() spawns fiber that calls Process().
```cpp
// Pipeline.hpp
protected:
  virtual Omni::Fiber::Coroutine<void> Process();

// Pipeline.cpp
Omni::Fiber::Coroutine<void> Pipeline::Process() {
    while (!_Stop.IsTriggered()) {
        Packet p;
        auto err_read = co_await _In->Read(p, _Stop);
        // ... existing logic ...
        for (auto& i : _Filters) { ... }
        auto err_write = co_await _Out->Write(p, _Stop);
        // ...
    }
}
```

**Step 2:** Verify existing tests still pass

**Step 3:** Commit
```bash
git add src/core/Pipeline.hpp src/core/Pipeline.cpp
git commit -m "refactor: extract Pipeline::Process() as virtual method"
```

### Task 2.2: Add io_context& to Pipeline constructor

**Files:**
- Modify: `src/core/Pipeline.hpp`
- Modify: `src/core/Pipeline.cpp`
- Modify: `src/LuaLib.cpp`

**Step 1:** Add `boost::asio::io_context&` parameter to Pipeline constructor, store as `_IO`
```cpp
class Pipeline {
protected:
    boost::asio::io_context& _IO;
public:
    Pipeline(boost::asio::io_context& io, ...);
};
```

**Step 2:** Update LuaLib `pipeline_new` to pass io_context from LuaInterface

**Step 3:** Verify build and tests

**Step 4:** Commit
```bash
git add src/core/Pipeline.hpp src/core/Pipeline.cpp src/LuaLib.cpp
git commit -m "refactor: add io_context reference to Pipeline constructor"
```

### Task 2.3: FecConfig struct

**Files:**
- Create: `src/core/FecConfig.hpp`

```cpp
#pragma once
#include <cstdint>

namespace gh {
struct FecConfig {
    uint32_t timeout_ms = 4;
    float overhead = 0.15f;
    float max_overhead = 0.50f;
    float repeat_ratio = 4.0f;
    uint32_t symbol_size = 0;
    uint32_t mtu = 1500;
    uint32_t max_batch = 200;
    bool obfuscate = true;
    uint8_t iv_len = 4;
    uint32_t decode_window = 64;
    uint32_t ping_interval_ms = 1000;
    uint32_t feedback_timeout_ms = 2000;
    uint32_t feedback_stale_ms = 10000;
    uint32_t ping_loss_threshold = 5;
    uint32_t decode_timeout_ms = 200;
};
} // namespace gh
```

**Step 1:** Commit
```bash
git add src/core/FecConfig.hpp
git commit -m "feat: add FecConfig struct for FEC pipeline parameters"
```

### Task 2.4: FecPipeline skeleton + Lua binding

**Files:**
- Create: `src/core/FecPipeline.hpp`
- Create: `src/core/FecPipeline.cpp`
- Modify: `src/LuaLib.cpp`
- Modify: `src/core/CMakeLists.txt`

**Step 1:** Write failing test for Lua API
```cpp
TEST(FecPipeline, LuaBindingExists) {
    // Verify hole.fec_pipeline is callable from Lua
}
```

**Step 2:** Create FecPipeline.hpp skeleton
```cpp
#pragma once
#include "Pipeline.hpp"
#include "FecConfig.hpp"

namespace gh {
class FecPipeline : public Pipeline {
public:
    FecPipeline(boost::asio::io_context& io,
                std::shared_ptr<EndpointInput> in,
                const std::vector<std::shared_ptr<Filter>>& filters,
                std::shared_ptr<EndpointOutput> out,
                const FecConfig& cfg);
    
protected:
    Omni::Fiber::Coroutine<void> Process() override;
    
private:
    FecConfig _Cfg;
    // ... internal state ...
};
} // namespace gh
```

**Step 3:** Wire Lua API `hole.fec_pipeline(channel, {filters}, channel, cfg)`
- Add `fec_pipeline_new` function in LuaLib.cpp
- Add metatable registration
- Parse FecConfig from Lua table

**Step 4:** Verify build

**Step 5:** Commit
```bash
git add src/core/FecPipeline.hpp src/core/FecPipeline.cpp src/LuaLib.cpp
git commit -m "feat: add FecPipeline skeleton and Lua fec_pipeline binding"
```

## Phase 3: FEC Encode / Decode

### Task 3.1: FEC Encode — batch accumulation + blob + RaptorQ

**Files:**
- Modify: `src/core/FecPipeline.cpp`

Implement the full encode path:
1. accumulate packets from _In with timeout
2. apply filter chain to batch
3. select optimal pkt_count
4. blob concat: [u32 count][u16 len][data] × pkt_count
5. zero-pad to symbol_size
6. rq_init, rq_encode for esi 0..K+M-1
7. write headers + XOR (if obfuscate) + PushFrontLE
8. co_await _Out->Write for each shard

**Step 1:** Write test with mock endpoints

**Step 2:** Implement encode path

**Step 3:** Verify tests

**Step 4:** Commit

### Task 3.2: FEC Decode — ring buffer + RaptorQ

**Files:**
- Modify: `src/core/FecPipeline.cpp`
- Create: `src/core/FecRingBuffer.hpp`

Implement the full decode path:
1. read packet from _In
2. PopFrontLE group_seq+flags, feedback, echo
3. dispatch by flags: PING / FEEDBACK_ONLY / REPEAT / FEC data
4. for FEC data: store shard in ring buffer
5. when source_count shards received → decode
6. rq_init, recover blob
7. split blob: [u32 count][u16 len][data]×count
8. filter chain on each recovered packet
9. _Out->Write each

**Step 1:** Write test with mock shards

**Step 2:** Implement decode path

**Step 3:** Verify tests

**Step 4:** Commit

### Task 3.3: PING / FEEDBACK_ONLY / REPEAT packet handling

**Files:**
- Modify: `src/core/FecPipeline.cpp`

Implement:
- PING: send timestamp, set pending_echo on receive
- REPEAT: copy raw packet with repeat_ratio, dedupe by group_seq
- FEEDBACK_ONLY: carry feedback byte

**Step 1:** Write tests

**Step 2:** Implement

**Step 3:** Verify

**Step 4:** Commit

### Task 3.4: Timer-based sending (Select pattern)

**Files:**
- Modify: `src/core/FecPipeline.cpp`

Implement the Select pattern for read vs timer:
```cpp
Omni::Fiber::Awaitable<...> Select(read_awaitable, timer_awaitable);
```

Each ping_interval_ms: send PING
Each feedback_timeout_ms with no data: send FEEDBACK_ONLY

**Step 1:** Write tests

**Step 2:** Implement timer integration with steady_timer + AsioUseFiber

**Step 3:** Verify

**Step 4:** Commit

## Phase 4: Obfuscation + Feedback + RTT

### Task 4.1: IV XOR obfuscation

**Files:**
- Modify: `src/core/FecPipeline.cpp`

Implement IV generation + XOR in encode/decode paths.

**Step 1:** Write tests for IV XOR round-trip

**Step 2:** Implement IV XOR in encode and decode

**Step 3:** Verify

**Step 4:** Commit

### Task 4.2: Loss rate statistics + feedback loop

**Files:**
- Modify: `src/core/FecPipeline.cpp`

Implement:
- loss rate calculation after decode (received vs expected shards)
- feedback = loss_rate × 250
- write feedback byte in all outgoing packets
- EWMA smoothing on receive (α=0.3)
- adaptive overhead calculation

**Step 1:** Write tests

**Step 2:** Implement

**Step 3:** Verify

**Step 4:** Commit

### Task 4.3: RTT measurement via PING echo

**Files:**
- Modify: `src/core/FecPipeline.cpp`

Implement:
- PING sends timestamp
- receiver stores as pending_echo
- next outgoing packet carries echo
- sender computes rtt_sample = now - echo
- EWMA smoothing (α=0.3)
- adaptive decode_timeout from RTT

**Step 1:** Write tests

**Step 2:** Implement

**Step 3:** Verify

**Step 4:** Commit

### Task 4.4: FecConfig validation at Pipeline Start

**Files:**
- Modify: `src/core/FecPipeline.cpp`

Implement:
- symbol_size + max_header ≤ mtu
- symbol_size ≤ kCapacity - 32
- iv_len ∈ 1..8 (if obfuscate)
- overhead ≤ max_overhead (clamp, don't terminate)
- terminate on hard violations

**Step 1:** Write tests

**Step 2:** Implement

**Step 3:** Verify

**Step 4:** Commit

## Phase 5: Integration + Deployment + Testing

### Task 5.1: Loopback integration test

**Files:**
- Create: `src/core/tests/TestFecIntegration.cpp`

Create a loopback test with:
- FecPipeline encode → simulated packet loss → FecPipeline decode
- Verify data integrity under loss

**Step 1:** Write test

**Step 2:** Verify test passes under various loss rates

**Step 3:** Commit

### Task 5.2: Build binary on osaka

```bash
cd ~/great-hole/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4
```

### Task 5.3: Deploy to ali and osaka

Copy great-hole binary and example FEC config Lua script to both machines:
```bash
scp build/great-hole ali:~/
scp build/great-hole osaka:~/
scp example-fec.lua ali:~/
scp example-fec.lua osaka:~/
```

### Task 5.4: Test connectivity ali ↔ osaka

- Start great-hole on ali (port 11451) and osaka
- Run bidirectional throughput test (e.g., iperf3 over UDP tunnel)
- Verify:
  - FEC packets are encoded/decoded correctly
  - RTT measurement works
  - Adaptive overhead adjusts
  - Connection survives under packet loss

### Task 5.5: Push all commits

```bash
cd ~/great-hole && git push origin master
```

---
