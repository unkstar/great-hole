#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gh {

// FEC 隧道统计信息系统 (LLM-CSV 模式, 2026-08-28)
//
// 设计目标:
//   - 百万级事件 = 内存原子计数, 日志每分钟 <= 1 行
//   - 周期全量快照写 CSV (行=时间点, 列=维度), 事件后可离线归因
//   - 自包含: 系统计数器 (/sys /proc) 由本类自行采样, 无外部 cron
//   - 归因三元组: 内核丢 (sys_rx_dropped) vs 链路丢 (dec_missing) vs
//     FEC 能力不足 (recover_abandoned)
//
// 默认 disabled; 经 lua `stats` 配置开启。测试用独立配置, 不影响生产。

struct FecStatsConfig {
    bool enabled = false;
    uint32_t interval_ms = 60000;        // 汇总/CSV 周期
    bool csv = true;
    std::string csv_dir = "/var/log/great-hole/stats";
    uint32_t csv_keep_days = 7;
    std::string sys_interface = "fec-test";
    bool event_log = true;
    uint32_t ring_buffer_sec = 3600;     // 按秒历史
};

class FecStats {
public:
    explicit FecStats(FecStatsConfig cfg);
    ~FecStats();

    bool Enabled() const { return _Cfg.enabled; }

    // ==================== 埋点 (热路径原子计数) ====================

    // ---- FEC 解码侧 ----
    void DecSrc() { _DecSrc.fetch_add(1, std::memory_order_relaxed); }
    void DecMissing() { _DecMissing.fetch_add(1, std::memory_order_relaxed); }
    void DecSmall() { _DecSmall.fetch_add(1, std::memory_order_relaxed); }
    void DecCtrl() { _DecCtrl.fetch_add(1, std::memory_order_relaxed); }
    void DecDup() { _DecDup.fetch_add(1, std::memory_order_relaxed); }
    void RecoverAttempt() { _RecoverAttempts.fetch_add(1, std::memory_order_relaxed); }
    void RecoverSuccess(uint32_t shards) {
        _RecoverSuccess.fetch_add(1, std::memory_order_relaxed);
        _RecoverShards.fetch_add(shards, std::memory_order_relaxed);
    }
    void RecoverFailed() { _RecoverFailed.fetch_add(1, std::memory_order_relaxed); }
    void RecoverAbandoned() { _RecoverAbandoned.fetch_add(1, std::memory_order_relaxed); }
    void ReorderEarly() { _ReorderEarly.fetch_add(1, std::memory_order_relaxed); }
    void DecodeTimeoutCleanup() { _DecodeTimeoutCleanups.fetch_add(1, std::memory_order_relaxed); }
    void SetLossRate(float r) { _LossRate.store(r, std::memory_order_relaxed); }

    // ---- FEC 编码侧 ----
    void EncSrc() { _EncSrc.fetch_add(1, std::memory_order_relaxed); }
    void EncSmall() { _EncSmall.fetch_add(1, std::memory_order_relaxed); }
    void EncRepair() { _EncRepair.fetch_add(1, std::memory_order_relaxed); }
    void DeadbandSuppressed() { _DeadbandSuppressed.fetch_add(1, std::memory_order_relaxed); }
    void SetOverhead(float oh) { _Overhead.store(oh, std::memory_order_relaxed); }
    void OverheadMaxHit() { _OverheadMaxHits.fetch_add(1, std::memory_order_relaxed); }
    void BatchFlushFull() { _BatchFlushFull.fetch_add(1, std::memory_order_relaxed); }
    void BatchFlushTimeout() { _BatchFlushTimeout.fetch_add(1, std::memory_order_relaxed); }
    void AddBatchSize(uint32_t n) {
        _BatchSizeSum.fetch_add(n, std::memory_order_relaxed);
        _BatchSizeCount.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- 传输层 ----
    void WriteFail(int errno_val, size_t len);
    void WriteEagain() { _WriteEagain.fetch_add(1, std::memory_order_relaxed); }
    void WritePartial() { _WritePartial.fetch_add(1, std::memory_order_relaxed); }
    void BatchAbort(uint32_t lost_pkts) {
        _BatchAbort.fetch_add(1, std::memory_order_relaxed);
        _BatchAbortLost.fetch_add(lost_pkts, std::memory_order_relaxed);
    }
    void SetQueueDepthPeak(uint32_t n) {
        uint32_t cur = _QueueDepthPeak.load(std::memory_order_relaxed);
        while (n > cur &&
               !_QueueDepthPeak.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {
        }
    }
    void OutBatch(uint32_t n) {
        _OutBatchSum.fetch_add(n, std::memory_order_relaxed);
        _OutBatchCount.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- 协商/反馈 ----
    void PingTimeout() { _PingTimeout.fetch_add(1, std::memory_order_relaxed); }
    void FeedbackTimeout() { _FeedbackTimeout.fetch_add(1, std::memory_order_relaxed); }
    void MuxStateChange() { _MuxStateChanges.fetch_add(1, std::memory_order_relaxed); }
    void MuxInvalidChannel() { _MuxInvalidChannel.fetch_add(1, std::memory_order_relaxed); }
    void MuxSessionTimeout() { _MuxSessionTimeout.fetch_add(1, std::memory_order_relaxed); }
    void SetRttUs(uint64_t rtt) { _RttUs.store(rtt, std::memory_order_relaxed); }
    void SetPeerLoss(float r) { _PeerLoss.store(r, std::memory_order_relaxed); }

    // ==================== 周期驱动 ====================

    // 系统采样 (/sys /proc), 由周期 tick 调用 (10s 粒度)
    void SampleSystem();

    // 周期汇总: 事件检测 + CSV 追加 + 环形缓冲; 由 FecPipeline tick 挂载
    void Tick(std::chrono::steady_clock::time_point now);

private:
    FecStatsConfig _Cfg;

    // ---- 原子计数 (热路径) ----
    std::atomic<uint64_t> _DecSrc{0}, _DecMissing{0}, _DecSmall{0}, _DecCtrl{0}, _DecDup{0};
    std::atomic<uint64_t> _RecoverAttempts{0}, _RecoverSuccess{0}, _RecoverShards{0},
        _RecoverFailed{0}, _RecoverAbandoned{0}, _ReorderEarly{0}, _DecodeTimeoutCleanups{0};
    std::atomic<uint64_t> _EncSrc{0}, _EncSmall{0}, _EncRepair{0}, _DeadbandSuppressed{0};
    std::atomic<uint64_t> _OverheadMaxHits{0}, _BatchFlushFull{0}, _BatchFlushTimeout{0};
    std::atomic<uint64_t> _BatchSizeSum{0}, _BatchSizeCount{0};
    std::atomic<uint64_t> _WriteFail{0}, _WriteEagain{0}, _WritePartial{0};
    std::atomic<uint64_t> _BatchAbort{0}, _BatchAbortLost{0};
    std::atomic<uint64_t> _OutBatchSum{0}, _OutBatchCount{0};
    std::atomic<uint64_t> _PingTimeout{0}, _FeedbackTimeout{0};
    std::atomic<uint64_t> _MuxStateChanges{0}, _MuxInvalidChannel{0}, _MuxSessionTimeout{0};
    std::atomic<float> _LossRate{0.0f}, _Overhead{0.0f}, _PeerLoss{0.0f};
    std::atomic<uint64_t> _RttUs{0};
    std::atomic<uint32_t> _QueueDepthPeak{0};

    // write 失败 errno 分布 (常见 errno 桶)
    std::array<std::atomic<uint64_t>, 8> _WriteFailErrno{};
    // 包长桶: <500 / 500-1000 / 1000-1300 / >1300
    std::array<std::atomic<uint64_t>, 4> _WriteFailLen{};

    // ---- 系统计数器 (采样快照 + 增量) ----
    uint64_t _SysRxPkts = 0, _SysRxDropped = 0, _SysTxPkts = 0, _SysTxDropped = 0;
    uint64_t _SysRxPktsPrev = 0, _SysRxDroppedPrev = 0, _SysTxPktsPrev = 0, _SysTxDroppedPrev = 0;
    uint64_t _SysVmRssKb = 0, _SysMemAvailKb = 0;
    double _SysCpuPct = 0.0;
    uint64_t _CpuPrevTotal = 0, _CpuPrevIdle = 0;
    bool _SysFirst = true;

    // ---- 周期/事件状态 ----
    std::chrono::steady_clock::time_point _LastTick{};
    std::chrono::steady_clock::time_point _LastSystemSample{};
    uint64_t _WriteFailPrev = 0;       // 上次汇总的 write_fail (速率计算)
    uint64_t _RecoverAbandonedPrev = 0;
    uint64_t _RecoverFailedPrev = 0;
    uint64_t _OverheadMaxHitsPrev = 0;
    uint64_t _RecoverAttemptsPrev = 0;
    uint64_t _DecMissingPrev = 0;
    int _LastFailErrno = 0;
    uint32_t _WriteFailHighStreak = 0;  // 高速率持续计数 (事件检测)

    // ---- 环形缓冲 (按秒, 关键速率) ----
    struct RingEntry {
        float loss_rate = 0.0f;
        uint32_t fail_rate = 0;        // write_fail/s
        uint32_t recover_rate = 0;     // recover_attempts/s
        uint32_t rx_drop_rate = 0;     // sys_rx_dropped delta/s
        uint32_t missing_rate = 0;     // dec_missing/s
    };
    std::vector<RingEntry> _Ring;
    size_t _RingPos = 0;
    std::chrono::steady_clock::time_point _LastRingTs{};

    // ---- CSV ----
    std::string _CsvPath;
    FILE* _CsvFile = nullptr;
    uint64_t _CsvRows = 0;
    uint64_t _DiskError = 0;

    void OpenCsv();
    void AppendCsv();
    void CleanupOldCsv();
    void CheckEvents();
    void UpdateRing(const RingEntry& e);
    std::string SysFile(const char* path);
    uint64_t ReadU64(const char* path);
};

} // namespace gh
