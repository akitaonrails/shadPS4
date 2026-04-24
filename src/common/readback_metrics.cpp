// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/readback_metrics.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

namespace Common {

namespace {
std::mutex g_dump_mutex;

uint32_t ClampNsToU32(std::chrono::nanoseconds ns) {
    const auto v = static_cast<uint64_t>(ns.count());
    return static_cast<uint32_t>(std::min<uint64_t>(v, std::numeric_limits<uint32_t>::max()));
}

double Percentile(std::vector<uint32_t>& samples, double p) {
    if (samples.empty()) {
        return 0.0;
    }
    const size_t idx = std::clamp<size_t>(static_cast<size_t>(p * (samples.size() - 1)),
                                          0, samples.size() - 1);
    std::nth_element(samples.begin(), samples.begin() + idx, samples.end());
    return static_cast<double>(samples[idx]);
}
} // namespace

ReadbackMetrics& ReadbackMetrics::Instance() {
    static ReadbackMetrics instance;
    return instance;
}

void ReadbackMetrics::Init() {
    const char* env = std::getenv("SHADPS4_READBACKS_METRICS");
    if (env == nullptr || env[0] == '\0') {
        return;
    }
    out_path = env;
    enabled.store(true, std::memory_order_relaxed);

    // Emulator::Run ends in std::quick_exit(0); register for both that
    // path (at_quick_exit) and normal main-return (atexit). The
    // benchmark harness kills via SIGTERM, so also chain a signal
    // handler that re-raises the default action after dumping.
    static bool hooks_registered = false;
    if (!hooks_registered) {
        hooks_registered = true;
        std::atexit([] { ReadbackMetrics::Instance().Dump(); });
        std::at_quick_exit([] { ReadbackMetrics::Instance().Dump(); });

        // The fstream write below is not strictly async-signal-safe,
        // but for a dev-only benchmark harness it is acceptable — the
        // alternative is to buffer and flush via ::write which costs
        // more complexity than the harness warrants. We chain rather
        // than swallow the signal so the parent's exit code reflects
        // the termination cause.
        const auto handler = +[](int sig) {
            ReadbackMetrics::Instance().Dump();
            std::signal(sig, SIG_DFL);
            std::raise(sig);
        };
        std::signal(SIGTERM, handler);
        std::signal(SIGINT, handler);
    }
}

void ReadbackMetrics::NoteSchedulerFinish(std::chrono::nanoseconds dur) {
    finish_count.fetch_add(1, std::memory_order_relaxed);
    finish_ns.fetch_add(static_cast<uint64_t>(dur.count()), std::memory_order_relaxed);
}

void ReadbackMetrics::NoteSchedulerWait(std::chrono::nanoseconds dur) {
    wait_count.fetch_add(1, std::memory_order_relaxed);
    wait_ns.fetch_add(static_cast<uint64_t>(dur.count()), std::memory_order_relaxed);
}

void ReadbackMetrics::NoteSchedulerSubmit() {
    submit_count.fetch_add(1, std::memory_order_relaxed);
}

void ReadbackMetrics::NoteBufferReadMemory(std::chrono::nanoseconds dur) {
    buffer_read_count.fetch_add(1, std::memory_order_relaxed);
    buffer_read_ns.fetch_add(static_cast<uint64_t>(dur.count()), std::memory_order_relaxed);
}

void ReadbackMetrics::NoteBufferDownload(uint64_t bytes, std::chrono::nanoseconds dur) {
    buffer_download_count.fetch_add(1, std::memory_order_relaxed);
    buffer_download_bytes.fetch_add(bytes, std::memory_order_relaxed);
    buffer_download_ns.fetch_add(static_cast<uint64_t>(dur.count()), std::memory_order_relaxed);
}

void ReadbackMetrics::NoteImageDownload(uint64_t bytes, std::chrono::nanoseconds dur) {
    image_download_count.fetch_add(1, std::memory_order_relaxed);
    image_download_bytes.fetch_add(bytes, std::memory_order_relaxed);
    image_download_ns.fetch_add(static_cast<uint64_t>(dur.count()), std::memory_order_relaxed);
}

void ReadbackMetrics::NoteImageDownloadBatch(uint32_t num_images) {
    image_download_batches.fetch_add(1, std::memory_order_relaxed);
    image_download_batched.fetch_add(num_images, std::memory_order_relaxed);
}

void ReadbackMetrics::NoteFaultDispatch() {
    fault_dispatch_count.fetch_add(1, std::memory_order_relaxed);
}

void ReadbackMetrics::NotePageProtect(std::chrono::nanoseconds dur) {
    page_protect_count.fetch_add(1, std::memory_order_relaxed);
    page_protect_ns.fetch_add(static_cast<uint64_t>(dur.count()), std::memory_order_relaxed);
}

void ReadbackMetrics::NotePageInvalidate() {
    page_invalidate_count.fetch_add(1, std::memory_order_relaxed);
}

void ReadbackMetrics::NoteFrame(std::chrono::nanoseconds frame_ns) {
    const uint64_t slot = frame_ring_next.fetch_add(1, std::memory_order_relaxed);
    frame_ring[slot % kFrameRingSize] = ClampNsToU32(frame_ns);
    frame_count.fetch_add(1, std::memory_order_relaxed);
}

void ReadbackMetrics::Dump() {
    if (!IsEnabled()) {
        return;
    }
    std::scoped_lock lk{g_dump_mutex};

    const uint64_t total_frames = frame_count.load(std::memory_order_relaxed);
    const uint64_t ring_next = frame_ring_next.load(std::memory_order_relaxed);
    const uint64_t ring_count = std::min<uint64_t>(ring_next, kFrameRingSize);

    std::vector<uint32_t> samples;
    samples.reserve(ring_count);
    for (uint64_t i = 0; i < ring_count; ++i) {
        const size_t idx = static_cast<size_t>((ring_next - ring_count + i) % kFrameRingSize);
        samples.push_back(frame_ring[idx]);
    }
    const double p50_ns = Percentile(samples, 0.50);
    const double p95_ns = Percentile(samples, 0.95);
    const double p99_ns = Percentile(samples, 0.99);

    std::ofstream out{out_path, std::ios::trunc};
    if (!out) {
        return;
    }
    out << "metric,value\n";
    out << "finish_count," << finish_count.load() << "\n";
    out << "finish_ns," << finish_ns.load() << "\n";
    out << "wait_count," << wait_count.load() << "\n";
    out << "wait_ns," << wait_ns.load() << "\n";
    out << "submit_count," << submit_count.load() << "\n";
    out << "buffer_read_count," << buffer_read_count.load() << "\n";
    out << "buffer_read_ns," << buffer_read_ns.load() << "\n";
    out << "buffer_download_count," << buffer_download_count.load() << "\n";
    out << "buffer_download_bytes," << buffer_download_bytes.load() << "\n";
    out << "buffer_download_ns," << buffer_download_ns.load() << "\n";
    out << "image_download_count," << image_download_count.load() << "\n";
    out << "image_download_bytes," << image_download_bytes.load() << "\n";
    out << "image_download_ns," << image_download_ns.load() << "\n";
    out << "image_download_batches," << image_download_batches.load() << "\n";
    out << "image_download_batched," << image_download_batched.load() << "\n";
    out << "fault_dispatch_count," << fault_dispatch_count.load() << "\n";
    out << "page_protect_count," << page_protect_count.load() << "\n";
    out << "page_protect_ns," << page_protect_ns.load() << "\n";
    out << "page_invalidate_count," << page_invalidate_count.load() << "\n";
    out << "frame_count," << total_frames << "\n";
    out << "frame_ring_count," << ring_count << "\n";
    out << "frame_p50_ns," << static_cast<uint64_t>(p50_ns) << "\n";
    out << "frame_p95_ns," << static_cast<uint64_t>(p95_ns) << "\n";
    out << "frame_p99_ns," << static_cast<uint64_t>(p99_ns) << "\n";
}

void ReadbackMetrics::Reset() {
    finish_count.store(0);
    finish_ns.store(0);
    wait_count.store(0);
    wait_ns.store(0);
    submit_count.store(0);
    buffer_read_count.store(0);
    buffer_read_ns.store(0);
    buffer_download_count.store(0);
    buffer_download_bytes.store(0);
    buffer_download_ns.store(0);
    image_download_count.store(0);
    image_download_bytes.store(0);
    image_download_ns.store(0);
    image_download_batches.store(0);
    image_download_batched.store(0);
    fault_dispatch_count.store(0);
    page_protect_count.store(0);
    page_protect_ns.store(0);
    page_invalidate_count.store(0);
    frame_count.store(0);
    frame_ring_next.store(0);
    std::memset(frame_ring, 0, sizeof(frame_ring));
}

} // namespace Common
