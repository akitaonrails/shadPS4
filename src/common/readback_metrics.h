// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>

namespace Common {

// Lightweight perf counters for the readbacks / GPU->CPU sync path.
// All counters are atomic and always live, but wall-clock timing and
// per-frame-time tracking are skipped when the harness is not enabled
// (controlled by SHADPS4_READBACKS_METRICS=<csv path> at startup).
//
// Design notes:
// * Atomic increments on hot paths are ~5 ns on x86_64 and branchless;
//   keeping them always-on simplifies the guard logic and avoids
//   surprising A/B deltas from the guard itself.
// * Time capture (`steady_clock::now`) is gated on IsEnabled() because
//   it is ~30-50 ns per call on Linux. For sub-microsecond hot paths
//   we don't want to pay that by default.
// * On process exit, Dump() writes a single CSV row per counter plus
//   frame-time percentiles computed from a bounded ring buffer.
class ReadbackMetrics {
public:
    static ReadbackMetrics& Instance();

    // Call once during startup. Reads SHADPS4_READBACKS_METRICS env var
    // and, if set, enables wall-clock capture + records the output path
    // for Dump().
    void Init();

    bool IsEnabled() const {
        return enabled.load(std::memory_order_relaxed);
    }

    // Counters. All are safe to call from any thread.

    void NoteSchedulerFinish(std::chrono::nanoseconds dur);
    void NoteSchedulerWait(std::chrono::nanoseconds dur);
    void NoteSchedulerSubmit();

    void NoteBufferReadMemory(std::chrono::nanoseconds dur);
    void NoteBufferDownload(uint64_t bytes, std::chrono::nanoseconds dur);

    void NoteImageDownload(uint64_t bytes, std::chrono::nanoseconds dur);
    void NoteImageDownloadBatch(uint32_t num_images);

    void NoteFaultDispatch();

    void NotePageProtect(std::chrono::nanoseconds dur);
    void NotePageInvalidate();

    // Called once per guest frame end. Adds a frame-time sample to the
    // ring buffer. `frame_ns` is wall-clock ns for that frame.
    void NoteFrame(std::chrono::nanoseconds frame_ns);

    // Dump counters + percentiles as CSV to the path recorded at Init.
    // No-op if metrics are disabled. Safe to call multiple times.
    void Dump();

    // Reset all counters to zero. Used between benchmark cells.
    void Reset();

private:
    ReadbackMetrics() = default;
    ReadbackMetrics(const ReadbackMetrics&) = delete;
    ReadbackMetrics& operator=(const ReadbackMetrics&) = delete;

    std::atomic<bool> enabled{false};
    std::filesystem::path out_path;

    std::atomic<uint64_t> finish_count{0};
    std::atomic<uint64_t> finish_ns{0};
    std::atomic<uint64_t> wait_count{0};
    std::atomic<uint64_t> wait_ns{0};
    std::atomic<uint64_t> submit_count{0};

    std::atomic<uint64_t> buffer_read_count{0};
    std::atomic<uint64_t> buffer_read_ns{0};
    std::atomic<uint64_t> buffer_download_count{0};
    std::atomic<uint64_t> buffer_download_bytes{0};
    std::atomic<uint64_t> buffer_download_ns{0};

    std::atomic<uint64_t> image_download_count{0};
    std::atomic<uint64_t> image_download_bytes{0};
    std::atomic<uint64_t> image_download_ns{0};
    std::atomic<uint64_t> image_download_batches{0};
    std::atomic<uint64_t> image_download_batched{0};

    std::atomic<uint64_t> fault_dispatch_count{0};

    std::atomic<uint64_t> page_protect_count{0};
    std::atomic<uint64_t> page_protect_ns{0};
    std::atomic<uint64_t> page_invalidate_count{0};

    std::atomic<uint64_t> frame_count{0};

    // Bounded ring of frame times. Percentiles computed on Dump.
    static constexpr size_t kFrameRingSize = 16384;
    std::atomic<uint64_t> frame_ring_next{0};
    uint32_t frame_ring[kFrameRingSize]{};  // ns, clamped to u32 range
};

// RAII helper that captures a duration and forwards it to the given
// callback on destruction. No-op when IsEnabled() is false at scope
// entry. Pattern:
//
//     Common::ScopedReadbackTimer t{[](auto ns) { ReadbackMetrics::Instance().NoteSchedulerFinish(ns); }};
//
// The lambda is heap-free; pass a capturing lambda by value.
template <typename F>
class ScopedReadbackTimer {
public:
    explicit ScopedReadbackTimer(F&& fn)
        : fn_(std::forward<F>(fn)),
          active_(ReadbackMetrics::Instance().IsEnabled()),
          start_(active_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}

    ~ScopedReadbackTimer() {
        if (!active_) {
            return;
        }
        const auto dur = std::chrono::steady_clock::now() - start_;
        fn_(std::chrono::duration_cast<std::chrono::nanoseconds>(dur));
    }

    ScopedReadbackTimer(const ScopedReadbackTimer&) = delete;
    ScopedReadbackTimer& operator=(const ScopedReadbackTimer&) = delete;

private:
    F fn_;
    bool active_;
    std::chrono::steady_clock::time_point start_;
};

template <typename F>
ScopedReadbackTimer(F&&) -> ScopedReadbackTimer<F>;

} // namespace Common
