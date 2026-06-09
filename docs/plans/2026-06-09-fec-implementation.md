# FEC Integration Implementation Plan

> **For implementer:** Use TDD throughout. Write failing test first. Watch it fail. Then implement.

**Goal:** Integrate RaptorQ-based Forward Error Correction (FEC) into great-hole's Pipeline with Lua configuration support.

**Architecture:** FecPipeline extends Pipeline with a batch-mode Process(). lcrq (RaptorQ) is added as a git submodule. FEC encode accumulates packets, encodes via RaptorQ, and writes headers; FEC decode reads headers, buffers shards, and decodes groups. Feedback loop with PING/RTT measurement adapts overhead dynamically.

**Tech Stack:** C++23, Boost.Asio, omni-fiber coroutines, lcrq (C, RFC 6330), Lua 5.4, CMake, GTest

**Clang-19 enforced:** CMakeLists.txt requires Clang compiler

---

## Progress Status (Updated: 2026-06-10)

### Phase 1: lcrq Integration + Packet LE Support ✅ COMPLETE

| Task | Status | Commit |
|------|--------|--------|
| 1.1 Add lcrq submodule | ✅ | 82a8522 |
| 1.2 CMake integration (autotools+ar static .a) | ✅ | a9dc0fe |
| 1.3 RaptorQ C++ wrapper (RaptorQ.hpp/cpp) | ✅ | a9dc0fe |
| 1.4 Packet PushFrontLE/PopFrontLE | ✅ | b7d5eb5 |

**Tests:** 9/9 RaptorQ unit tests pass (TestLcrq.cpp)

### Phase 2: Pipeline Refactor + FecConfig + Lua ✅ COMPLETE

| Task | Status | Commit |
|------|--------|--------|
| 2.1 Extract Pipeline::Process() virtual | ✅ | b1f78ed |
| 2.2 Add io_context& to Pipeline (_Io) | ✅ | b1f78ed |
| 2.3 FecConfig struct | ✅ | a9dc0fe |
| 2.4 FecPipeline skeleton + Lua binding | ✅ | 7ad9ad2 |

### Phase 3: FEC Encode / Decode ✅ COMPLETE

| Task | Status | Commit |
|------|--------|--------|
| 3.1 FEC Encode (batch+blob+RaptorQ+headers) | ✅ | 7ad9ad2 |
| 3.2 FEC Decode (ring buffer+RaptorQ+split) | ✅ | 7ad9ad2 |
| 3.3 PING/FEEDBACK/REPEAT packet handling | ✅ | 7ad9ad2 |
| 3.4 Timer-based Select pattern | ✅ | 7ad9ad2 |

### Phase 4: Obfuscation + Feedback + RTT ✅ COMPLETE

| Task | Status | Commit |
|------|--------|--------|
| 4.1 IV XOR obfuscation | ✅ | 7ad9ad2 |
| 4.2 Loss rate + feedback loop | ✅ | 7ad9ad2 |
| 4.3 RTT via PING echo | ✅ | 7ad9ad2 |
| 4.4 Config validation | ✅ | 7ad9ad2 |

### Phase 5: Deployment + Testing 🔄 IN PROGRESS

| Task | Status |
|------|--------|
| 5.1 Loopback integration test | ⏳ TODO |
| 5.2 Build binary (Debug) | ✅ Completed (17.5MB) |
| 5.3 Deploy to ali and osaka | 🔄 In Progress |
| 5.4 Test connectivity ali↔osaka | ⏳ Blocked by ali DNS resolution |

**Current Issue:** Osaka cannot resolve 'ali' hostname via DNS. Need to use ali's IP (172.17.60.123) in the Lua config. The FEC configs are created:
-  — ali side (port 11451)
-  — osaka side (port 25252)

Both use separate ports from existing instances (no conflict).

### Files Changed

| File | Change |
|------|--------|
| CMakeLists.txt | Clang enforcement + lcrq subdir |
| .gitmodules | lcrq submodule |
| libs/lcrq/CMakeLists.txt | Autotools+ar static build |
| src/core/Pipeline.hpp | Virtual Process(), _Io |
| src/core/Pipeline.cpp | Extracted Process() |
| src/core/Packet.hpp | PushFrontLE/PopFrontLE |
| src/core/FecConfig.hpp | (new) Config struct |
| src/core/RaptorQ.hpp/cpp | (new) lcrq C++ wrapper |
| src/core/FecPipeline.hpp/cpp | (new) Full FEC Pipeline |
| src/core/CMakeLists.txt | Added new source files |
| src/core/tests/TestLcrq.cpp | (new) 9 RaptorQ tests |
| src/core/tests/CMakeLists.txt | Added lcrq_test |
| src/LuaLib.cpp | hole.fec_pipeline() binding |
