## Why

great-hole 当前依赖 UDPspeeder 嵌套隧道提供 FEC 保护，但 UDPspeeder 使用 GF(256) Reed-Solomon 码，symbol 数上限 255，无 SIMD 加速，且反馈机制粗糙。本项目需要将 RaptorQ (RFC 6330) 前向纠错直接集成到 Pipeline 中，获得 rateless 码的灵活性、SIMD 加速的高吞吐（实测 450~1769 Mbps）、以及双向丢包反馈驱动的自适应冗余控制。

## What Changes

- 新增 `libs/lcrq` 子模块（RaptorQ C 库，SSSE3/SSE2/AVX2/AVX-512）
- 新增 `RaptorQ.hpp/cpp`：lcrq 的 C++ 封装（`Encode` / `GenerateSymbol` / `SubmitSymbol` / `TryDecode`）
- `Packet` 新增 `PushFrontLE` / `PopFrontLE` 支持小端序 DWORD 读写
- `Pipeline` 提取 `virtual Process()` 并引入 `io_context&`，支持 timer 集成
- 新增 `FecPipeline : public Pipeline`：batch 模式编码/解码，含 wire format、ring buffer、PING/FEEDBACK_ONLY/REPEAT 三种控制包
- 新增 `FecConfig` 结构体：统一 FEC 配置，Lua 可配置
- 新增 `AdaptiveOverhead`：8 种自适应算法（Static / EWMA+Static / EWMA+Dynamic / PI / MIMD / Quantile / BurstAware / Gradient）
- 新增 `LossPattern`：6 种可控丢包模型（Bernoulli / Gilbert / GilbertElliott / Sinusoidal / Step / CongestionWave）用于测试
- Lua API：`hole.fec_pipeline()` + `hole.fec_shared_state()`
- 新增 `fec_matrix_test.py`：自动化矩阵测试脚本，遍历 8×6×4=192 种组合

## Capabilities

### New Capabilities

- `fec-pipeline`: RaptorQ FEC 编码/解码 Pipeline，含 wire format、batch 积累、ring buffer、控制包、自适应 overhead
- `fec-adaptive-overhead`: 8 种自适应冗余算法，从双端反馈的丢包率动态调整 overhead
- `fec-loss-pattern`: 6 种丢包模型，用于可控测试验证自适应算法效果

### Modified Capabilities

<!-- No existing specs are being modified — FEC is a new capability -->

## Impact

- **新增文件**: `src/core/FecPipeline.hpp/cpp`, `src/core/FecConfig.hpp`, `src/core/RaptorQ.hpp/cpp`, `src/core/AdaptiveOverhead.hpp/cpp`, `src/core/LossPattern.hpp/cpp`, `tools/fec_matrix_test.py`
- **修改文件**: `src/core/Pipeline.hpp/cpp`（virtual Process + io_context）, `src/core/Packet.hpp`（PushFrontLE/PopFrontLE）, `src/LuaLib.cpp`（fec_pipeline binding）, `CMakeLists.txt`, `.gitmodules`
- **新增依赖**: lcrq (GPL-2.0, git submodule, autotools+ar 静态链接)
- **外部接口**: Lua 新增 `hole.fec_pipeline(in, filters, out, cfg, is_encoder, shared)` + `hole.fec_shared_state()`
