#include "FecStats.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <boost/log/trivial.hpp>

namespace gh {

namespace fs = std::filesystem;

FecStats::FecStats(FecStatsConfig cfg) : _Cfg(std::move(cfg)) {
    if (!_Cfg.enabled) return;
    if (_Cfg.interval_ms < 1000) _Cfg.interval_ms = 1000;
    if (_Cfg.csv) OpenCsv();
    _LastTick = std::chrono::steady_clock::now();
    _LastSystemSample = _LastTick;
    _LastRingTs = _LastTick;
    size_t ring_sz = std::max<size_t>(_Cfg.ring_buffer_sec, 60);
    _Ring.assign(ring_sz, RingEntry{});
}

FecStats::~FecStats() {
    if (_CsvFile) std::fclose(_CsvFile);
}

// ==================== 埋点 (非内联部分) ====================

void FecStats::WriteFail(int errno_val, size_t len) {
    _WriteFail.fetch_add(1, std::memory_order_relaxed);
    // errno 桶: 0-7 覆盖常见错误 (ENOMEM=12, EINVAL=22, EMSGSIZE=90 ...), 其余进桶 7
    size_t eb = 7;
    if (errno_val == 11 || errno_val == 12) eb = 0;  // EAGAIN/ENOMEM
    else if (errno_val == 22) eb = 1;                // EINVAL
    else if (errno_val == 90) eb = 2;                // EMSGSIZE
    else if (errno_val == 105) eb = 3;               // ENOBUFS
    else if (errno_val >= 1 && errno_val < 64) eb = 4;
    _WriteFailErrno[eb].fetch_add(1, std::memory_order_relaxed);
    size_t lb = len > 1300 ? 3 : (len > 1000 ? 2 : (len > 500 ? 1 : 0));
    _WriteFailLen[lb].fetch_add(1, std::memory_order_relaxed);
}

// ==================== 系统采样 ====================

uint64_t FecStats::ReadU64(const char* path) {
    std::ifstream f(path);
    if (!f) return 0;
    uint64_t v = 0;
    f >> v;
    return v;
}

void FecStats::SampleSystem() {
    if (!_Cfg.enabled) return;
    std::string base = "/sys/class/net/" + _Cfg.sys_interface + "/statistics/";
    _SysRxPkts = ReadU64((base + "rx_packets").c_str());
    _SysRxDropped = ReadU64((base + "rx_dropped").c_str());
    _SysTxPkts = ReadU64((base + "tx_packets").c_str());
    _SysTxDropped = ReadU64((base + "tx_dropped").c_str());
    if (_SysFirst) {
        // 首次采样: 接口计数是历史累计 (进程重启不归零), 直接建立基线,
        // 否则 CheckEvents/UpdateRing 会把全部历史当增量 -> 假阳性报警
        // (2026-08-29 实测: 升级重启后误报 rx_dropped 3474539/11104421 (31%))
        _SysRxPktsPrev = _SysRxPkts;
        _SysRxDroppedPrev = _SysRxDropped;
        _SysTxPktsPrev = _SysTxPkts;
        _SysTxDroppedPrev = _SysTxDropped;
    }

    // 进程 RSS (/proc/self/status VmRSS)
    {
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream ss(line.substr(7));
                ss >> _SysVmRssKb;
                break;
            }
        }
    }
    // 系统可用内存 (/proc/meminfo MemAvailable)
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("MemAvailable:", 0) == 0) {
                std::istringstream ss(line.substr(13));
                ss >> _SysMemAvailKb;
                break;
            }
        }
    }
    // CPU 使用率 (/proc/stat delta)
    {
        std::ifstream f("/proc/stat");
        std::string line;
        std::getline(f, line);
        std::istringstream ss(line);
        std::string cpu;
        ss >> cpu;
        uint64_t user = 0, nice = 0, sys = 0, idle = 0;
        ss >> user >> nice >> sys >> idle;
        uint64_t total = user + nice + sys + idle;
        if (!_SysFirst) {
            uint64_t dt = total - _CpuPrevTotal;
            uint64_t di = idle - _CpuPrevIdle;
            _SysCpuPct = dt > 0 ? 100.0 * (1.0 - static_cast<double>(di) / dt) : 0.0;
        }
        _CpuPrevTotal = total;
        _CpuPrevIdle = idle;
        _SysFirst = false;
    }
}

// ==================== CSV ====================

void FecStats::OpenCsv() {
    try {
        fs::create_directories(_Cfg.csv_dir);
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "FecStats: cannot create csv_dir " << _Cfg.csv_dir;
        _Cfg.csv = false;
        return;
    }
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    _CsvPath = _Cfg.csv_dir + "/fec-test-" + buf + ".csv";
    _CsvFile = std::fopen(_CsvPath.c_str(), "a");
    if (!_CsvFile) {
        BOOST_LOG_TRIVIAL(warning) << "FecStats: cannot open csv " << _CsvPath;
        _Cfg.csv = false;
        return;
    }
    // 空文件写表头
    std::fseek(_CsvFile, 0, SEEK_END);
    if (std::ftell(_CsvFile) == 0) {
        std::fprintf(_CsvFile,
            "ts,dec_src,dec_missing,dec_small,dec_ctrl,dec_dup,recover_attempts,"
            "recover_success,recover_shards,recover_failed,recover_abandoned,reorder_early,"
            "decode_timeouts,loss_rate,enc_src,enc_small,enc_repair,deadband_supp,overhead,"
            "overhead_max_hits,batch_full,batch_timeout,batch_size_avg,write_fail,"
            "write_eagain,write_partial,batch_abort,batch_abort_lost,q_depth_peak,"
            "out_batch_avg,rtt_us,peer_loss,ping_timeout,feedback_timeout,"
            "mux_state_changes,mux_invalid_channel,mux_session_timeout,"
            "sys_rx_pkts,sys_rx_dropped,sys_tx_pkts,sys_tx_dropped,vm_rss_kb,mem_avail_kb,cpu_pct\n");
        std::fflush(_CsvFile);
    }
}

void FecStats::CleanupOldCsv() {
    if (_Cfg.csv_keep_days == 0) return;
    try {
        auto now = std::chrono::system_clock::now();
        for (auto& entry : fs::directory_iterator(_Cfg.csv_dir)) {
            auto ft = fs::last_write_time(entry.path());
            // fs::file_time_type 基于 file_clock, 需显式转换到 system_clock 再相减
            auto age = std::chrono::duration_cast<std::chrono::hours>(
                           std::chrono::file_clock::to_sys(ft) - now)
                           .count();
            if (age > static_cast<std::int64_t>(_Cfg.csv_keep_days * 24)) {
                fs::remove(entry.path());
            }
        }
    } catch (...) {
        // 清理失败静默
    }
}

void FecStats::AppendCsv() {
    if (!_Cfg.csv || !_CsvFile) return;
    std::fprintf(_CsvFile,
        "%lld,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.1f,%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u,%.1f"
        ",%" PRIu64 ",%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%.1f\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count()),
        _DecSrc.load(), _DecMissing.load(), _DecSmall.load(), _DecCtrl.load(), _DecDup.load(),
        _RecoverAttempts.load(), _RecoverSuccess.load(), _RecoverShards.load(),
        _RecoverFailed.load(), _RecoverAbandoned.load(), _ReorderEarly.load(),
        _DecodeTimeoutCleanups.load(), _LossRate.load(),
        _EncSrc.load(), _EncSmall.load(), _EncRepair.load(), _DeadbandSuppressed.load(),
        _Overhead.load(), _OverheadMaxHits.load(),
        _BatchFlushFull.load(), _BatchFlushTimeout.load(),
        _BatchSizeCount.load() ? static_cast<double>(_BatchSizeSum.load()) /
                                     static_cast<double>(_BatchSizeCount.load())
                               : 0.0,
        _WriteFail.load(), _WriteEagain.load(), _WritePartial.load(),
        _BatchAbort.load(), _BatchAbortLost.load(), _QueueDepthPeak.load(),
        _OutBatchCount.load() ? static_cast<double>(_OutBatchSum.load()) /
                                    static_cast<double>(_OutBatchCount.load())
                              : 0.0,
        _RttUs.load(), _PeerLoss.load(),
        _PingTimeout.load(), _FeedbackTimeout.load(),
        _MuxStateChanges.load(), _MuxInvalidChannel.load(), _MuxSessionTimeout.load(),
        _SysRxPkts, _SysRxDropped, _SysTxPkts, _SysTxDropped,
        _SysVmRssKb, _SysMemAvailKb, _SysCpuPct);
    _CsvRows++;
    // 60s/行 频率: 每行落盘, 崩溃/重启不丢最近数据 (LLM-CSV 可追溯性要求)
    if (std::fflush(_CsvFile) != 0) {
        _DiskError++;
        if (_DiskError == 1)
            BOOST_LOG_TRIVIAL(warning) << "FecStats: csv flush failed (disk full?)";
    }
}

// ==================== 事件检测 ====================

void FecStats::CheckEvents() {
    if (!_Cfg.event_log) return;

    // 静默放弃 (丢包超 FEC 能力) — 关键事件
    if (_RecoverAbandoned.load() > _RecoverAbandonedPrev) {
        BOOST_LOG_TRIVIAL(warning)
            << "FecStats: recover_abandoned=" << _RecoverAbandoned.load()
            << " (loss exceeds FEC capacity), loss_rate=" << _LossRate.load()
            << " overhead=" << _Overhead.load();
    }
    _RecoverAbandonedPrev = _RecoverAbandoned.load();

    // 恢复失败
    if (_RecoverFailed.load() > _RecoverFailedPrev) {
        BOOST_LOG_TRIVIAL(warning) << "FecStats: recover_failed=" << _RecoverFailed.load();
    }
    _RecoverFailedPrev = _RecoverFailed.load();

    // overhead 饱和
    if (_OverheadMaxHits.load() > _OverheadMaxHitsPrev) {
        BOOST_LOG_TRIVIAL(warning)
            << "FecStats: overhead reached max, hits=" << _OverheadMaxHits.load();
    }
    _OverheadMaxHitsPrev = _OverheadMaxHits.load();

    // write_fail 高速率持续 (丢包风暴)
    uint64_t wf = _WriteFail.load();
    uint64_t delta = wf - _WriteFailPrev;
    uint64_t interval_s = std::max<uint64_t>(_Cfg.interval_ms / 1000, 1);
    uint64_t rate = delta / interval_s;
    if (rate > 1000) {
        _WriteFailHighStreak++;
        if (_WriteFailHighStreak == 5)
            BOOST_LOG_TRIVIAL(warning)
                << "FecStats: write_fail storm " << rate << "/s sustained (kernel drops)";
    } else {
        _WriteFailHighStreak = 0;
    }
    _WriteFailPrev = wf;

    // rx_dropped 异常增长
    uint64_t drop_delta = _SysRxDropped - _SysRxDroppedPrev;
    uint64_t pkt_delta = _SysRxPkts - _SysRxPktsPrev;
    if (pkt_delta > 0 && drop_delta * 100 > pkt_delta) {
        BOOST_LOG_TRIVIAL(warning)
            << "FecStats: sys rx_dropped " << drop_delta << "/" << pkt_delta
            << " (" << (drop_delta * 100 / pkt_delta) << "%)";
    }
    _SysRxDroppedPrev = _SysRxDropped;
    _SysRxPktsPrev = _SysRxPkts;
}

// ==================== 环形缓冲 ====================

void FecStats::UpdateRing(const RingEntry& e) {
    auto now = std::chrono::steady_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now - _LastRingTs).count();
    if (secs <= 0) return;
    // 每秒一个槽: 填最近一个间隔的速率
    size_t n = std::min<size_t>(static_cast<size_t>(secs), _Ring.size());
    for (size_t i = 0; i < n; i++) {
        _Ring[_RingPos] = e;
        _RingPos = (_RingPos + 1) % _Ring.size();
    }
    _LastRingTs = now;
}

// ==================== 周期汇总 ====================

void FecStats::Tick(std::chrono::steady_clock::time_point now) {
    if (!_Cfg.enabled) return;
    if (_LastTick.time_since_epoch().count() == 0) {
        _LastTick = now;
        return;
    }
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - _LastTick).count();
    if (elapsed < static_cast<std::int64_t>(_Cfg.interval_ms)) return;
    _LastTick = now;

    // 系统采样 (10s 粒度)
    if (std::chrono::duration_cast<std::chrono::seconds>(now - _LastSystemSample).count() >= 10) {
        SampleSystem();
        _LastSystemSample = now;
    }

    CheckEvents();
    AppendCsv();
    if (_CsvRows % 1440 == 0) CleanupOldCsv();

    // 环形缓冲 (速率快照)
    RingEntry e;
    e.loss_rate = _LossRate.load();
    uint64_t interval_s = std::max<uint64_t>(_Cfg.interval_ms / 1000, 1);
    uint64_t wf = _WriteFail.load(), wf_prev = _WriteFailPrev;
    e.fail_rate = static_cast<uint32_t>((wf - wf_prev) / interval_s);
    e.recover_rate =
        static_cast<uint32_t>((_RecoverAttempts.load() - _RecoverAttemptsPrev) / interval_s);
    e.rx_drop_rate = static_cast<uint32_t>((_SysRxDropped - _SysRxDroppedPrev) / interval_s);
    e.missing_rate = static_cast<uint32_t>((_DecMissing.load() - _DecMissingPrev) / interval_s);
    UpdateRing(e);
    _RecoverAttemptsPrev = _RecoverAttempts.load();
    _DecMissingPrev = _DecMissing.load();
}

} // namespace gh
