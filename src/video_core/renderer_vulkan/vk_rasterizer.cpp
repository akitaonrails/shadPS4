// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_set>

#include "common/debug.h"
#include "common/memory_patcher.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {

namespace {

constexpr std::array<u64, 12> kDriveclubLaterRaceGatePipelines{
    0x967922c49cee2dd1ull, 0xe72e555cdb85af86ull, 0xb98e78a7a28007ceull,
    0x3ff0fc8f05bc302dull, 0xd14226a181106f7eull, 0x1bb555896c9e247eull,
    0xff9e11acb5a72dffull, 0x5148c06fb63b96e0ull, 0x660a29eb5e92b0adull,
    0xfe66b45b84a7dd16ull, 0x02c197f768d8d430ull, 0xf6e5670be11b0009ull,
};

constexpr std::array<u64, 2> kDriveclubEarlyGatePipelines{
    0x1cdd747ee89204c0ull,
    0x6bde71906ac1af18ull,
};

constexpr u32 kDriveclubRaceWindowSubmits = 32;
constexpr u32 kDriveclubRaceGateMinHashes = 3;
constexpr u32 kDriveclubGammaHintWindowSubmits = 96;

struct DriveclubRaceGateSubmitState {
    u64 submit_index{};
    std::array<u64, 8> hashes{};
    u32 num_hashes{};
    bool has_depth{};
    bool has_visible_target{};
    bool has_hdr_target{};
};

std::atomic<u64> g_driveclub_submit_index{};
std::atomic<u32> g_driveclub_race_window{};
// Hash of the pipeline currently being issued through Draw()/DrawIndirect().
// Set at Draw entry, read by BindTextures/texdump so each dumped texture
// can be attributed to the pipeline that bound it. GpuComm is single-
// threaded so atomic ordering is not a concern here.
std::atomic<u64> g_driveclub_current_pipeline_hash{};
// Counts every time the race-window gate latches. Menus/panorama also
// arm the gate, so the true race-start is typically arm #3 or #4. The
// texnuke probe reads this to optionally skip earlier arms.
std::atomic<u32> g_driveclub_arm_count{};
// Submit index at the most recent gate arm. Lets timing-based probes
// count how many submits ago the race kicked off.
std::atomic<u64> g_driveclub_last_arm_submit{};
// Set by the 1936-byte pin when it observes a light-flag slot flip
// to 1.0 in the scene light UBO (headlights turned on). Shared with
// the 224-byte pin so both expire on the same event.
std::atomic<bool> g_driveclub_headlights_on{};

// Classification of the draw currently in flight based on its RT set.
// Set by the Draw/DrawIndirect entry points so BindTextures-time hooks
// can decide whether to skip a texture write.
enum class DriveclubDrawKind : u8 {
    Other = 0,   // unclassified / doesn't match scene or post-fx patterns
    Scene = 1,   // writes HDR or G-buffer with depth
    PostFx = 2,  // single-RT fullscreen post-fx or tonemap target
    Ui = 3,      // draws into the visible composite alone (HUD/UI)
    Compute = 4, // compute dispatch (bloom, exposure, luma, tonemap, ...)
};
std::atomic<u8> g_driveclub_current_draw_kind{static_cast<u8>(DriveclubDrawKind::Other)};

DriveclubDrawKind ClassifyDriveclubDraw(const AmdGpu::Regs& regs) {
    // Count live color buffers + collect addresses.
    std::array<VAddr, 8> rts{};
    u32 rt_count = 0;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col = regs.color_buffers[cb];
        if (col) {
            rts[rt_count++] = col.Address();
        }
    }
    const bool has_depth = regs.depth_buffer.DepthValid();

    if (rt_count == 0) {
        return DriveclubDrawKind::Other;
    }

    // MRT G-buffer: 3+ RTs including the visible composite + friends,
    // with depth on. Produces scene data.
    if (rt_count >= 3 && has_depth) {
        bool has_visible = false;
        bool has_abe = false;
        bool has_d5c = false;
        for (u32 i = 0; i < rt_count; ++i) {
            has_visible |= (rts[i] == 0x500cdd0000ull);
            has_abe     |= (rts[i] == 0x500abe0000ull);
            has_d5c     |= (rts[i] == 0x500d5c8000ull);
        }
        if (has_visible && has_abe && has_d5c) {
            return DriveclubDrawKind::Scene;
        }
    }

    if (rt_count == 1) {
        const VAddr addr = rts[0];
        // Known scene-HDR targets with depth on -> scene lighting /
        // deferred passes. Without depth -> post-fx fullscreen.
        if (addr == 0x5009688000ull || addr == 0x5008130000ull) {
            return has_depth ? DriveclubDrawKind::Scene : DriveclubDrawKind::PostFx;
        }
        // Visible composite alone = HUD / UI compositor.
        if (addr == 0x500cdd0000ull) {
            return DriveclubDrawKind::Ui;
        }
        // All other single-RT fullscreen addresses are post-fx buffers
        // (0x500fdd0000, 0x5000108000, 0x5000900000, 0x509a400000,
        // 0x5015770000, 0x501abf8000, etc).
        return DriveclubDrawKind::PostFx;
    }

    return DriveclubDrawKind::Other;
}
std::atomic<u64> g_driveclub_gamma_hint_until_submit{};
std::mutex g_driveclub_gate_mutex;
DriveclubRaceGateSubmitState g_driveclub_gate_state{};

bool IsDriveclubGuardEnabled() {
    static const bool enabled = MemoryPatcher::g_game_serial == "CUSA00003";
    return enabled;
}

bool IsDriveclubLaterRaceGatePipeline(u64 pipeline_hash) {
    return std::ranges::find(kDriveclubLaterRaceGatePipelines, pipeline_hash) !=
           kDriveclubLaterRaceGatePipelines.end();
}

bool IsDriveclubEarlyGatePipeline(u64 pipeline_hash) {
    return std::ranges::find(kDriveclubEarlyGatePipelines, pipeline_hash) !=
           kDriveclubEarlyGatePipelines.end();
}

void ResetDriveclubGateState(DriveclubRaceGateSubmitState& state, u64 submit_index) {
    state.submit_index = submit_index;
    state.hashes.fill(0);
    state.num_hashes = 0;
    state.has_depth = false;
    state.has_visible_target = false;
    state.has_hdr_target = false;
}

void InsertDriveclubGateHash(DriveclubRaceGateSubmitState& state, u64 pipeline_hash) {
    for (u32 i = 0; i < state.num_hashes; ++i) {
        if (state.hashes[i] == pipeline_hash) {
            return;
        }
    }
    if (state.num_hashes < state.hashes.size()) {
        state.hashes[state.num_hashes++] = pipeline_hash;
    }
}

void NoteDriveclubRaceGateCandidate(const GraphicsPipeline* pipeline, const AmdGpu::Regs& regs) {
    if (!IsDriveclubGuardEnabled()) {
        return;
    }

    const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());
    if (IsDriveclubEarlyGatePipeline(pipeline_hash) ||
        !IsDriveclubLaterRaceGatePipeline(pipeline_hash)) {
        return;
    }

    bool has_visible_target = false;
    bool has_hdr_target = false;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf) {
            continue;
        }
        switch (col_buf.Address()) {
        case 0x500cdd0000ull:
            has_visible_target = true;
            break;
        case 0x5009688000ull:
        case 0x5008130000ull:
        case 0x500fdd0000ull:
            has_hdr_target = true;
            break;
        default:
            break;
        }
    }

    if (!regs.depth_buffer.DepthValid() || !has_visible_target || !has_hdr_target) {
        return;
    }

    const u64 submit_index = g_driveclub_submit_index.load();
    const u64 gamma_hint_until_submit = g_driveclub_gamma_hint_until_submit.load();
    if (submit_index > gamma_hint_until_submit) {
        return;
    }
    std::lock_guard lock{g_driveclub_gate_mutex};
    auto& state = g_driveclub_gate_state;
    if (state.submit_index != submit_index) {
        ResetDriveclubGateState(state, submit_index);
    }
    state.has_depth = true;
    state.has_visible_target = true;
    state.has_hdr_target = true;
    InsertDriveclubGateHash(state, pipeline_hash);

    if (state.num_hashes >= kDriveclubRaceGateMinHashes &&
        g_driveclub_race_window.exchange(kDriveclubRaceWindowSubmits) == 0) {
        const u32 n = g_driveclub_arm_count.fetch_add(1) + 1;
        g_driveclub_last_arm_submit.store(submit_index);
        LOG_INFO(Render_Vulkan,
                 "[dc-gate] armed#{} submit={} hashes={} visible=true hdr=true depth=true",
                 n, submit_index, state.num_hashes);
    }
}

// Driveclub per-draw trace. Enable with env SHADPS4_DC_DRAWLOG=1.
//
// For every graphics Draw / DrawIndirect, log one line capturing:
//   - the current submit index from the existing race-gate machinery
//   - the pipeline hash
//   - every bound color-buffer address + the depth-buffer address
//
// Rate-limited to one log line per unique (submit_index, pipeline_hash)
// tuple. A given pipeline that runs many times within a submit logs once
// per submit, so the log length stays proportional to how many distinct
// render operations the frame is doing. Cross-correlate with the
// [dc-timeline] heartbeat in vk_presenter.cpp to pin each log line to a
// wall-clock window, and with the existing [dc-gate] armed events to know
// when we are inside the race-start window.
//
// Goal: compare the pipeline / render-target signatures emitted during a
// dim blackout plateau vs the recovered-scene frame directly before
// recovery. Whichever pipeline appears only in the dim phase (or only in
// the bright phase) is a strong candidate for the downstream multiplier
// the animlib scalars cannot reach.

bool IsDcDrawlogEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_DRAWLOG");
        const bool on = env != nullptr && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan, "[dc-drawlog] enabled (SHADPS4_DC_DRAWLOG=1)");
        }
        return on;
    }();
    return enabled;
}

void NoteDriveclubDrawlog(const GraphicsPipeline* pipeline, const AmdGpu::Regs& regs) {
    if (!IsDcDrawlogEnabled()) {
        return;
    }

    // Only log while codex's race-window guard is armed. The guard is
    // latched when the later-race pipeline cluster first appears (see
    // kDriveclubLaterRaceGatePipelines) and stays armed for
    // kDriveclubRaceWindowSubmits submits afterwards. That window
    // covers the prerace blackout + the first second or two of race,
    // which is exactly what we want to diff. Without this filter, a
    // full session flooded the log with ~5000 lines/second.
    if (g_driveclub_race_window.load() == 0) {
        return;
    }

    const u64 submit_index = g_driveclub_submit_index.load();
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());

    // Rate-limit: one log line per (submit, pipeline_hash) tuple.
    // The dedup window is the CURRENT submit; once the submit index
    // advances, the set is reset so recurrent pipelines are logged again
    // in the new submit. That gives us per-submit pipeline fingerprints
    // suitable for frame-to-frame diffing.
    static u64 dedup_submit = ~0ull;
    static std::unordered_set<u64> seen_in_submit;
    static std::mutex dedup_mutex;
    {
        std::lock_guard lock{dedup_mutex};
        if (submit_index != dedup_submit) {
            dedup_submit = submit_index;
            seen_in_submit.clear();
        }
        if (!seen_in_submit.insert(pipeline_hash).second) {
            return;
        }
    }

    // Collect bound render targets for this draw. Most draws use 0-2
    // MRT slots; the prerace blackout draws we care about target the
    // four well-known scene addresses identified in Phase 10-11.
    std::string rts;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf) {
            continue;
        }
        if (!rts.empty()) {
            rts += ",";
        }
        rts += fmt::format("{:#x}", col_buf.Address());
    }
    if (rts.empty()) {
        rts = "<none>";
    }

    const bool has_depth = regs.depth_buffer.DepthValid();
    LOG_INFO(Render_Vulkan, "[dc-drawlog] submit={} pipeline={:#018x} rts={} depth={}",
             submit_index, pipeline_hash, rts, has_depth ? "yes" : "no");
}

// Driveclub uniform-buffer logger. Enable with env SHADPS4_DC_UBOLOG=1.
//
// For each Draw that targets one of a hand-picked set of race-window
// pipelines, dumps the first 128 bytes of every non-special uniform
// buffer the shader reads (CB0, CB1, ...). Rate-limited to one dump per
// (submit_index, pipeline_hash) tuple. The hex bytes plus their float
// interpretation give us the runtime uniform values the game is feeding
// into the scene-material shaders during the blackout; we can diff
// across submits to find which exact float changes from dim to bright
// at race-start.
//
// The pipeline filter avoids dumping all ~400 pipelines. We only log the
// four MRT-writing "blackout" pipelines plus their "recovery" counterparts
// that Phase 12's drawlog bisect flagged as the likely candidates.

constexpr std::array<u64, 13> kDcUboLogPipelines{
    0x3ff0fc8f05bc302dull, // blackout MRT-writer, early-only (from Phase 12 diff)
    0xd14226a181106f7eull, // recovery counterpart, late-only
    0xb98e78a7a28007ceull, // race-window MRT-writer (both phases)
    0x1cdd747ee89204c0ull, // race-window MRT-writer (both phases)
    0x6bde71906ac1af18ull, // race-window MRT-writer (both phases)
    0xf6e5670be11b0009ull, // extra race-window candidate from kDriveclubLaterRaceGatePipelines
    // Phase 14: single-RT composition passes targeting the visible
    // composite 0x500cdd0000 with depth=yes. These fire continuously
    // through the race window and are strong candidates for the
    // fullscreen fade / tonemap that produces the user-visible dim.
    0x2bd7c53265cadae2ull,
    0xadd2ec4587065da9ull,
    0xe6e42747689b5d99ull,
    // Phase 15 Round 15d — binders of the five user-picked Bc1RgbaSrgb
    // vignette-mask candidates. The first two draw into the HDR alt
    // primary (0x5008130000) while sampling a small Bc1 texture, which
    // is the signature of a vignette / lens-dirt / mask compositor
    // layering onto the scene before tonemap. The other two are MRT
    // G-buffer writers that sample a 1024x1024 Bc1 — probably a
    // material sampler (less likely dim drivers, kept for parity).
    0xc7fd16555c9913a5ull, // binds 0x5003831e00  128x128 Bc1 -> HDR alt
    0xaab6634a8d4573f0ull, // binds 0x509b821a00  256x256 Bc1 -> HDR alt
    0x145c84476cb9391cull, // binds 0x50b95d6c00  256x256 Bc1 -> MRT G-buf
    0x5fccbdcf0d968d11ull, // binds 0x5029e8f100 1024x1024 Bc1 -> MRT G-buf
};

bool IsDcUboLogEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_UBOLOG");
        const bool on = env != nullptr && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan, "[dc-ubolog] enabled (SHADPS4_DC_UBOLOG=1)");
        }
        return on;
    }();
    return enabled;
}

bool IsDcUboLogPipeline(u64 hash) {
    for (auto p : kDcUboLogPipelines) {
        if (p == hash) {
            return true;
        }
    }
    return false;
}

void NoteDriveclubUboLog(const GraphicsPipeline* pipeline, const AmdGpu::Regs& regs) {
    if (!IsDcUboLogEnabled()) {
        return;
    }
    if (g_driveclub_race_window.load() == 0) {
        return;
    }

    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());
    if (!IsDcUboLogPipeline(pipeline_hash)) {
        return;
    }

    const u64 submit_index = g_driveclub_submit_index.load();

    // Dedup per (submit, pipeline). Within one submit, one pipeline may
    // draw many times with identical uniforms — we only want the first
    // dump.
    static u64 dedup_submit = ~0ull;
    static std::unordered_set<u64> seen_in_submit;
    static std::mutex dedup_mutex;
    {
        std::lock_guard lock{dedup_mutex};
        if (submit_index != dedup_submit) {
            dedup_submit = submit_index;
            seen_in_submit.clear();
        }
        if (!seen_in_submit.insert(pipeline_hash).second) {
            return;
        }
    }

    // Walk every stage and dump each of its non-special uniform buffers.
    // Reading from `vsharp.base_address` is safe here because the shader
    // is about to do the same read via the Vulkan descriptor we're
    // building from the very same pointer. If it weren't mapped, the
    // draw itself would have faulted.
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        u32 cb_idx = 0;
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) {
                cb_idx++;
                continue;
            }
            const auto vsharp = desc.GetSharp(*stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                cb_idx++;
                continue;
            }
            const auto* bytes = reinterpret_cast<const u8*>(vsharp.base_address);
            const size_t len = std::min<size_t>(128, vsharp.GetSize());
            std::string hex;
            hex.reserve(len * 2);
            for (size_t j = 0; j < len; ++j) {
                fmt::format_to(std::back_inserter(hex), "{:02x}", bytes[j]);
            }
            LOG_INFO(Render_Vulkan,
                     "[dc-ubolog] submit={} pipeline={:#018x} stage={} cb{} "
                     "addr={:#x} sz={} bytes={}",
                     submit_index, pipeline_hash, static_cast<u32>(stage->stage), cb_idx,
                     vsharp.base_address, vsharp.GetSize(), hex);
            cb_idx++;
        }
    }
}

// Driveclub luminance clamp. Enable with env SHADPS4_DC_LUM_CLAMP=<float>.
//
// Phase 13 found that pipeline 0xf6e5670be11b0009 consumes a 48-byte UBO
// at stage=0/stage=2 cb0 whose 4th float is the only value that moves
// across the race window. Its layout is fully invariant otherwise:
//   [0]=1.0  [1]=1.0  [2]=0.3  [3]=LUM
//   [4]=1.0  [5]=0.25 [6]=1.0  [7]=0.25
//   [8]=0.25 [9]=1.0  [10]=0.25 [11]=1.0
// During the blackout, LUM climbs monotonically (seen 5.35 -> 13.98 over
// ~2 s) without ever converging — classic runaway auto-exposure. Vanilla
// PS4 tops out near 6.5, so anything >6.5 is the emulator-specific bug.
//
// This knob lets us prove the UBO is the dim driver by overwriting the
// 4th float at bind time. The exact signature is matched before the
// write so we never corrupt some other coincidentally-shaped UBO.
//
// Usage:
//   SHADPS4_DC_LUM_CLAMP=2.0 scripts/run_driveclub_overlay.sh
// Zero or unset disables the clamp.
float GetDcLumClampValue() {
    static const float value = [] {
        const char* env = std::getenv("SHADPS4_DC_LUM_CLAMP");
        if (env == nullptr || env[0] == '\0') {
            return 0.0f;
        }
        char* end = nullptr;
        const float v = std::strtof(env, &end);
        if (end == env || !std::isfinite(v) || v <= 0.0f) {
            LOG_WARNING(Render_Vulkan,
                        "[dc-lumclamp] ignored invalid SHADPS4_DC_LUM_CLAMP='{}'", env);
            return 0.0f;
        }
        LOG_INFO(Render_Vulkan, "[dc-lumclamp] enabled (SHADPS4_DC_LUM_CLAMP={})", v);
        return v;
    }();
    return value;
}

void MaybeClampDriveclubLuminanceUbo(const GraphicsPipeline* pipeline,
                                     const AmdGpu::Regs& regs) {
    const float clamp = GetDcLumClampValue();
    if (clamp <= 0.0f) {
        return;
    }
    if (!IsDriveclubGuardEnabled()) {
        return;
    }
    if (g_driveclub_race_window.load() == 0) {
        return;
    }
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());
    if (pipeline_hash != 0xf6e5670be11b0009ull) {
        return;
    }

    // Expected invariant layout (12 floats). Match exact bit-patterns;
    // all constants are representable. Offset 3 is the dynamic slot we
    // overwrite — it is excluded from the match check.
    constexpr std::array<float, 12> kExpected{
        1.0f,  1.0f,  0.3f,  0.0f /* ignored */,
        1.0f,  0.25f, 1.0f,  0.25f,
        0.25f, 1.0f,  0.25f, 1.0f,
    };

    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) {
                continue;
            }
            const auto vsharp = desc.GetSharp(*stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() < 48) {
                continue;
            }
            auto* floats = reinterpret_cast<float*>(vsharp.base_address);
            bool layout_ok = true;
            for (size_t i = 0; i < kExpected.size(); ++i) {
                if (i == 3) {
                    continue;
                }
                if (floats[i] != kExpected[i]) {
                    layout_ok = false;
                    break;
                }
            }
            if (!layout_ok) {
                continue;
            }
            if (floats[3] > clamp) {
                // Log first clamp per submit so we can correlate with
                // the drawlog without flooding the journal.
                static std::atomic<u64> last_logged_submit{~0ull};
                const u64 submit_index = g_driveclub_submit_index.load();
                const u64 prev = last_logged_submit.exchange(submit_index);
                if (prev != submit_index) {
                    LOG_INFO(Render_Vulkan,
                             "[dc-lumclamp] submit={} pipeline={:#018x} lum={} -> {}",
                             submit_index, pipeline_hash, floats[3], clamp);
                }
                floats[3] = clamp;
            }
        }
    }
}

// Driveclub exposure-UBO restorer. Enable with env SHADPS4_DC_EXPO_RESTORE=1.
//
// Phase 13b: pipelines 0x6bde71906ac1af18 and 0x1cdd747ee89204c0 (both in
// kDriveclubEarlyGatePipelines) bind a 32-float UBO at stage=1 cb=4 that
// shows a clean two-phase signature coinciding with the race start:
//
//   Bright (pre-race) phase — offsets 24..31 carry:
//     (292.6, 288.3, 271.3, 13.6, 0.31, 0.31, 0.30, 0.015)
//   plus off[0]=0.4, off[12]=0, off[16]=0.001.
//
//   Dim (post-race-start) phase — offsets 24..31 flip to all zero;
//   off[0] climbs to 1.0, off[12] flips to 1, off[16] climbs to ~0.59.
//
// The transition aligns exactly with the gate re-arm at race start, so
// whatever the shader does with these fields is almost certainly inside
// the scene-dim feedback. This probe restores the bright-phase values
// for offsets 24..31 whenever the UBO looks like it is in the Z phase,
// leaving off[0]/off[12]/off[16] alone (those flip-flags may be required
// by other consumers).
//
// If the blackout visibly softens with this env on, the dim driver lives
// in this UBO's "scene scale" block. If not, the UBO is downstream of a
// broader state change and we pivot.
bool IsDcExpoRestoreEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_EXPO_RESTORE");
        const bool on = env != nullptr && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan, "[dc-exporestore] enabled (SHADPS4_DC_EXPO_RESTORE=1)");
        }
        return on;
    }();
    return enabled;
}

void MaybeRestoreDriveclubExposureUbo(const GraphicsPipeline* pipeline,
                                      const AmdGpu::Regs& regs) {
    if (!IsDcExpoRestoreEnabled()) {
        return;
    }
    if (!IsDriveclubGuardEnabled()) {
        return;
    }
    if (g_driveclub_race_window.load() == 0) {
        return;
    }
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());
    if (pipeline_hash != 0x6bde71906ac1af18ull &&
        pipeline_hash != 0x1cdd747ee89204c0ull) {
        return;
    }

    // Bright-phase reference values captured from submits 423..1465 of
    // the Phase 13 clamp run. Offsets 24..27 are the RGB+alpha scale
    // triple and offsets 28..31 are their small-magnitude companion set
    // that tracks them 1:1 through both phases.
    constexpr float kNzOff24 = 292.6075f;
    constexpr float kNzOff25 = 288.2520f;
    constexpr float kNzOff26 = 271.3029f;
    constexpr float kNzOff27 = 13.6001f;
    constexpr float kNzOff28 = 0.3185f;
    constexpr float kNzOff29 = 0.3137f;
    constexpr float kNzOff30 = 0.2953f;
    constexpr float kNzOff31 = 0.0148f;

    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        u32 cb_idx = 0;
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) {
                cb_idx++;
                continue;
            }
            if (cb_idx != 4) {
                cb_idx++;
                continue;
            }
            const auto vsharp = desc.GetSharp(*stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() < 128) {
                cb_idx++;
                continue;
            }
            auto* floats = reinterpret_cast<float*>(vsharp.base_address);
            // Only intervene when we see the Z-phase signature: the RGB
            // scale triple all exactly zero.
            if (floats[24] != 0.0f || floats[25] != 0.0f ||
                floats[26] != 0.0f || floats[27] != 0.0f) {
                cb_idx++;
                continue;
            }
            floats[24] = kNzOff24;
            floats[25] = kNzOff25;
            floats[26] = kNzOff26;
            floats[27] = kNzOff27;
            floats[28] = kNzOff28;
            floats[29] = kNzOff29;
            floats[30] = kNzOff30;
            floats[31] = kNzOff31;
            static std::atomic<u64> last_logged_submit{~0ull};
            const u64 submit_index = g_driveclub_submit_index.load();
            const u64 prev = last_logged_submit.exchange(submit_index);
            if (prev != submit_index) {
                LOG_INFO(Render_Vulkan,
                         "[dc-exporestore] submit={} pipeline={:#018x} restored off24..31",
                         submit_index, pipeline_hash);
            }
            cb_idx++;
        }
    }
}

// Driveclub UBO propagation test. Enable with env SHADPS4_DC_UBO_NUKE=1.
//
// Writes a very loud garbage pattern (NaN-like bit-pattern 0x7fc0dead,
// and large magnitude floats) into the entire 32-float UBO at stage=1
// cb=4 of pipelines 0x6bde71906ac1af18 / 0x1cdd747ee89204c0. The goal
// is *not* to fix anything — just to confirm whether writes to the
// guest UBO address at Draw-time actually reach the shader on the GPU.
//
// Expected outcomes when this env is on:
//   - scene glitches catastrophically during race window (write works,
//     value range matters for any future fix)
//   - scene unchanged → memory_tracker / buffer_cache snapshotted the
//     UBO before our hook, and in-place writes at this point are
//     simply never re-uploaded. In that case the fix has to move to a
//     different layer (bind-side override, or hook the upstream writer).
void MaybeNukeDriveclubExposureUbo(const GraphicsPipeline* pipeline,
                                   const AmdGpu::Regs& regs) {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_UBO_NUKE");
        const bool on = env != nullptr && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan, "[dc-ubonuke] enabled (SHADPS4_DC_UBO_NUKE=1)");
        }
        return on;
    }();
    if (!enabled) {
        return;
    }
    if (!IsDriveclubGuardEnabled()) {
        return;
    }
    if (g_driveclub_race_window.load() == 0) {
        return;
    }
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());
    if (pipeline_hash != 0x6bde71906ac1af18ull &&
        pipeline_hash != 0x1cdd747ee89204c0ull) {
        return;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        u32 cb_idx = 0;
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) {
                cb_idx++;
                continue;
            }
            if (cb_idx != 4) {
                cb_idx++;
                continue;
            }
            const auto vsharp = desc.GetSharp(*stage);
            if (vsharp.base_address == 0 || vsharp.GetSize() < 128) {
                cb_idx++;
                continue;
            }
            auto* floats = reinterpret_cast<float*>(vsharp.base_address);
            // Binary search landed on off[16] as the sole race-time
            // scene-brightness driver. NZ-phase values sit at 0.001;
            // Z-phase climbs to ~0.59 mean, up to 5.75. Pin it to the
            // bright value to kill the blackout.
            floats[16] = 0.001f;
            static std::atomic<u64> last_logged{~0ull};
            const u64 submit_index = g_driveclub_submit_index.load();
            if (last_logged.exchange(submit_index) != submit_index) {
                LOG_INFO(Render_Vulkan,
                         "[dc-ubonuke] submit={} pipeline={:#018x} nuked cb4",
                         submit_index, pipeline_hash);
            }
            cb_idx++;
        }
    }
}

// Driveclub torture probe. Enable with env SHADPS4_DC_TORTURE=1.
// For draws whose color attachments target one of the critical full-res
// HDR/composite surfaces, substitute specific sampled source textures with
// a null binding. The goal is to isolate the in-game eye-adaptation /
// luma-history feedback that drives the race-start fade by nulling one
// suspect source at a time and observing which one shifts the fade.
//
// Current target: the 80x48 R32G32B32A32Sfloat buffer at 0x501d630000 — its
// aspect ratio matches a per-tile luma-average grid, which is the classic
// source shape for HDR eye adaptation.
constexpr std::array<VAddr, 8> kTortureSourceAddrs{
    0x501d630000ull, // 80x48 R32G32B32A32Sfloat  — luma per-tile grid
    0x501df20000ull, // 364x276 R32G32B32A32Sfloat — medium luma grid
    0x50ba598400ull, // 128x2 R16G16B16A16Sfloat   — odd-shape HDR (histogram?)
    0x5003896400ull, // 32x32 R8G8B8A8Unorm        — tiny lookup
    0x5065f2dc00ull, // 32x32 R16G16B16A16Sfloat   — probe-grid
    0x5065f2bc00ull, // 32x32 R16G16B16A16Sfloat   — probe-grid
    0x5065f31c00ull, // 32x32 R16G16B16A16Sfloat   — probe-grid
    0x50239c0000ull, // 32x32 R16G16B16A16Sfloat   — probe-grid
};

constexpr std::array<VAddr, 4> kTortureRenderTargets{
    0x500cdd0000ull, // 1920x1080 R8G8B8A8Srgb  visible composite target
    0x5009688000ull, // 1920x1080 B10G11R11UfloatPack32 HDR main branch
    0x5008130000ull, // 1920x1080 B10G11R11UfloatPack32 HDR alt primary
    0x500fdd0000ull, // 1920x1080 B10G11R11UfloatPack32 HDR alt composite
};

bool IsTortureEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_TORTURE");
        const bool on = env != nullptr && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan, "[dc-torture] enabled (SHADPS4_DC_TORTURE=1)");
        }
        return on;
    }();
    return enabled;
}

// Driveclub texture dumper. Enable with env SHADPS4_DC_TEX_DUMP=1.
//
// Phase 14: the race-start blackout visually manifests as a specific
// "dark car static image" overlay. We still don't know which pipeline
// draws it — our UBO probes keep landing on scene-material state
// rather than the overlay composition. So: during the race window,
// on first bind of each unique guest-texture address, dump the raw
// guest bytes and a metadata line describing format/size/tiling.
//
// Output goes to `$HOME/.local/share/shadPS4/texdump/` (inside the
// distrobox that's /home/akitaonrails/.local/...). Each dump is:
//   s<submit>_a<addr>_<w>x<h>_<fmt>_tile<mode>.bin
// plus one `[dc-texdump]` log line per dump so the log acts as an
// index. De-swizzle happens offline — the bytes are what the game
// asked the GPU to sample; we just ship them to disk.
bool IsDcTexDumpEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_TEX_DUMP");
        const bool on = env != nullptr && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan, "[dc-texdump] enabled (SHADPS4_DC_TEX_DUMP=1)");
        }
        return on;
    }();
    return enabled;
}

const std::string& DcTexDumpDir() {
    static const std::string dir = [] {
        const char* home = std::getenv("HOME");
        std::string d = fmt::format("{}/.local/share/shadPS4/texdump",
                                     home ? home : "/tmp");
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        if (ec) {
            LOG_WARNING(Render_Vulkan,
                        "[dc-texdump] failed to create {}: {}", d, ec.message());
        }
        return d;
    }();
    return dir;
}

// Driveclub texture nuker. Enable with either of:
//   SHADPS4_DC_TEX_NUKE_ADDRS=0xAAAAA,0xBBBBB,...
//   SHADPS4_DC_TEX_NUKE_PIPES=0xHHHH,0xIIII,...
// (both accepted together; any match triggers the nuke.)
//
// For matching textures, overwrites the full guest-size range with a
// repeating 0xDEADBEEF pattern every time the texture binds during the
// race window. That's "every frame" effectively — the write survives
// any engine-side re-upload because we clobber at bind-time, after
// the game's own writes. The nuke propagates through the same
// page-fault invalidation path that UBO nuke proved works.
//
// Use case: binary-search the set of scene textures to find the
// blackout/vignette mask. Batch several addresses at once; if the
// blackout is unchanged the mask is not in the batch.
const std::unordered_set<u64>& GetDcTexNukeAddrs() {
    static const std::unordered_set<u64> set = [] {
        std::unordered_set<u64> s;
        const char* env = std::getenv("SHADPS4_DC_TEX_NUKE_ADDRS");
        if (!env || !env[0]) return s;
        std::string in = env;
        size_t pos = 0;
        while (pos < in.size()) {
            const size_t end = in.find(',', pos);
            std::string tok = in.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!tok.empty()) {
                const u64 v = std::strtoull(tok.c_str(), nullptr, 0);
                if (v) s.insert(v);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        if (!s.empty()) {
            LOG_INFO(Render_Vulkan,
                     "[dc-texnuke] enabled for {} addresses (SHADPS4_DC_TEX_NUKE_ADDRS)",
                     s.size());
        }
        return s;
    }();
    return set;
}

const std::unordered_set<u64>& GetDcTexNukeSkipPipes() {
    static const std::unordered_set<u64> set = [] {
        std::unordered_set<u64> s;
        const char* env = std::getenv("SHADPS4_DC_NUKE_SKIP_PIPES");
        if (!env || !env[0]) return s;
        std::string in = env;
        size_t pos = 0;
        while (pos < in.size()) {
            const size_t end = in.find(',', pos);
            std::string tok = in.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!tok.empty()) {
                const u64 v = std::strtoull(tok.c_str(), nullptr, 0);
                if (v) s.insert(v);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        if (!s.empty()) {
            LOG_INFO(Render_Vulkan,
                     "[dc-texnuke] skip list has {} pipelines (SHADPS4_DC_NUKE_SKIP_PIPES)",
                     s.size());
        }
        return s;
    }();
    return set;
}

const std::unordered_set<u64>& GetDcTexNukePipes() {
    static const std::unordered_set<u64> set = [] {
        std::unordered_set<u64> s;
        const char* env = std::getenv("SHADPS4_DC_TEX_NUKE_PIPES");
        if (!env || !env[0]) return s;
        std::string in = env;
        size_t pos = 0;
        while (pos < in.size()) {
            const size_t end = in.find(',', pos);
            std::string tok = in.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!tok.empty()) {
                const u64 v = std::strtoull(tok.c_str(), nullptr, 0);
                if (v) s.insert(v);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        if (!s.empty()) {
            LOG_INFO(Render_Vulkan,
                     "[dc-texnuke] enabled for {} pipelines (SHADPS4_DC_TEX_NUKE_PIPES)",
                     s.size());
        }
        return s;
    }();
    return set;
}

bool IsDcTexNukeAllEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("SHADPS4_DC_TEX_NUKE_ALL");
        const bool v = env && env[0] == '1' && env[1] == '\0';
        if (v) {
            LOG_INFO(Render_Vulkan,
                     "[dc-texnuke] ALL-texture nuke enabled (SHADPS4_DC_TEX_NUKE_ALL=1)");
        }
        return v;
    }();
    return on;
}

// Selects which draw kinds (as classified by ClassifyDriveclubDraw) are
// eligible for nuking. Default is "scene" — G-buffer + HDR-with-depth.
// Post-fx, UI, and unclassified draws are skipped by default so the
// tonemap/UI path stays intact.
u32 GetDcNukeKindMask() {
    static const u32 mask = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_KIND");
        if (!env || !env[0]) {
            return 1u << static_cast<u32>(DriveclubDrawKind::Scene);
        }
        u32 m = 0;
        std::string s = env;
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto add = [&](const char* tok, DriveclubDrawKind k) {
            if (s.find(tok) != std::string::npos) m |= 1u << static_cast<u32>(k);
        };
        if (s.find("all") != std::string::npos) {
            m = 0xFFFFFFFFu;
        } else {
            add("scene",  DriveclubDrawKind::Scene);
            add("postfx", DriveclubDrawKind::PostFx);
            add("ui",      DriveclubDrawKind::Ui);
            add("other",   DriveclubDrawKind::Other);
            add("compute", DriveclubDrawKind::Compute);
        }
        LOG_INFO(Render_Vulkan, "[dc-texnuke] kind mask = 0x{:x} ({})", m, env);
        return m;
    }();
    return mask;
}

// Runtime knobs to relax each of the defensive filters. Default values
// produce the "safe" behaviour we converged on (colour scene, nothing
// else touched). Raising a knob re-includes that category so we can
// see whether the blackout driver lives there.
u32 GetDcNukeMinDim() {
    static const u32 v = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_MIN_DIM");
        const u32 def = 128u;
        if (!env || !env[0]) return def;
        return static_cast<u32>(std::strtoul(env, nullptr, 0));
    }();
    return v;
}

// Maximum width/height cap; default 0 means unlimited. Combined with
// SHADPS4_DC_NUKE_MIN_DIM this brackets a size range: tests like
//   MIN_DIM=32 MAX_DIM=63 → only 32..63-px textures get nuked
// so we can rotate in small sub-batches without collapsing the scene.
u32 GetDcNukeMaxDim() {
    static const u32 v = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_MAX_DIM");
        if (!env || !env[0]) return 0u;
        return static_cast<u32>(std::strtoul(env, nullptr, 0));
    }();
    return v;
}

// Format substring match. Empty = no filter. Any texture whose vk::Format
// name contains this substring is nuked; everything else is skipped
// (when this knob is set). e.g. SHADPS4_DC_NUKE_FORMAT=Bc4 targets
// BC4 normal-maps; SHADPS4_DC_NUKE_FORMAT=Sfloat targets float data
// textures once INCLUDE_DATA=1 is also set.
const std::string& GetDcNukeFormatFilter() {
    static const std::string s = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_FORMAT");
        return std::string(env ? env : "");
    }();
    return s;
}

u32 GetDcNukeMaxAspect() {
    static const u32 v = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_MAX_ASPECT");
        const u32 def = 8u;
        if (!env || !env[0]) return def;
        return static_cast<u32>(std::strtoul(env, nullptr, 0));
    }();
    return v;
}

bool GetDcNukeIncludeData() {
    static const bool v = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_INCLUDE_DATA");
        return env && env[0] == '1' && env[1] == '\0';
    }();
    return v;
}

bool GetDcNukeIncludeGpuMod() {
    static const bool v = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_INCLUDE_GPU_MOD");
        return env && env[0] == '1' && env[1] == '\0';
    }();
    return v;
}

u32 GetDcNukeAfterArm() {
    static const u32 n = [] {
        const char* env = std::getenv("SHADPS4_DC_NUKE_AFTER_ARM");
        if (!env || !env[0]) return 0u;
        const auto v = static_cast<u32>(std::strtoul(env, nullptr, 0));
        if (v > 0) {
            LOG_INFO(Render_Vulkan,
                     "[dc-texnuke] gated to fire only after gate arm #{}", v);
        }
        return v;
    }();
    return n;
}

bool MaybeNukeDriveclubTexture(const VideoCore::Image& image) {
    const auto& addr_set = GetDcTexNukeAddrs();
    const auto& pipe_set = GetDcTexNukePipes();
    const bool nuke_all = IsDcTexNukeAllEnabled();
    if (addr_set.empty() && pipe_set.empty() && !nuke_all) {
        return false;
    }
    if (!IsDriveclubGuardEnabled()) {
        return false;
    }
    if (g_driveclub_race_window.load() == 0) {
        return false;
    }
    // Optional: skip early gate arms (menu/panorama) and only fire once
    // we're past the N-th arm.
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    const auto& info = image.info;
    if (info.guest_address == 0 || info.guest_size == 0) {
        return false;
    }

    // SHADPS4_DC_NUKE_INCLUDE_GPU_MOD=1 re-includes images the GPU has
    // written to (render targets being re-sampled as textures). Safe
    // default is to skip them — nuking breaks the tonemap / post-fx
    // sampling.
    if (!GetDcNukeIncludeGpuMod() &&
        True(image.flags & VideoCore::ImageFlagBits::GpuModified)) {
        return false;
    }

    // SHADPS4_DC_NUKE_MIN_DIM / _MAX_DIM bracket the size range.
    // Defaults: min=128, max=unlimited. Set MAX_DIM to narrow the
    // upper bound and rotate through small sub-batches.
    const u32 min_dim = GetDcNukeMinDim();
    if (min_dim > 0 &&
        (info.size.width < min_dim || info.size.height < min_dim)) {
        return false;
    }
    const u32 max_dim = GetDcNukeMaxDim();
    if (max_dim > 0 &&
        (info.size.width > max_dim || info.size.height > max_dim)) {
        return false;
    }
    // SHADPS4_DC_NUKE_MAX_ASPECT caps long/short edge ratio (default 8).
    // Anything more extreme is usually a data strip. Set to 0 to
    // disable the check.
    const u32 max_aspect = GetDcNukeMaxAspect();
    if (max_aspect > 0) {
        const u32 long_edge = std::max(info.size.width, info.size.height);
        const u32 short_edge = std::min(info.size.width, info.size.height);
        if (short_edge > 0 && long_edge / short_edge > max_aspect) {
            return false;
        }
    }
    // SHADPS4_DC_NUKE_INCLUDE_DATA=1 re-includes non-colour float/int
    // formats (R16Sfloat, HDR packed, etc). Default skip because these
    // usually feed tonemap / exposure / metering paths and nuking
    // them collapses the output.
    // SHADPS4_DC_NUKE_FORMAT=<substr> — when set, only nuke textures
    // whose vk::Format name contains this substring. Disables all
    // other format-based filters.
    const std::string& fmt_filter = GetDcNukeFormatFilter();
    if (!fmt_filter.empty()) {
        const std::string fmt_name = vk::to_string(info.pixel_format);
        if (fmt_name.find(fmt_filter) == std::string::npos) {
            return false;
        }
    } else if (!GetDcNukeIncludeData()) {
        switch (info.pixel_format) {
        case vk::Format::eR16Sfloat:
        case vk::Format::eR32Sfloat:
        case vk::Format::eR16Unorm:
        case vk::Format::eR16Snorm:
        case vk::Format::eR16Uint:
        case vk::Format::eR16Sint:
        case vk::Format::eR32Uint:
        case vk::Format::eR32Sint:
        case vk::Format::eR16G16Sfloat:
        case vk::Format::eR32G32Sfloat:
        case vk::Format::eR16G16Unorm:
        case vk::Format::eR16G16B16A16Sfloat:
        case vk::Format::eR32G32B32A32Sfloat:
        case vk::Format::eR32G32B32A32Uint:
        case vk::Format::eB10G11R11UfloatPack32:
            return false;
        default:
            break;
        }
    }

    const auto& skip_pipe_set = GetDcTexNukeSkipPipes();
    const u64 cur_pipe = g_driveclub_current_pipeline_hash.load();
    if (skip_pipe_set.count(cur_pipe) != 0) {
        return false;
    }

    // Only fire on draw kinds we've been asked to target.
    const u32 kind_bit = 1u << g_driveclub_current_draw_kind.load();
    if ((GetDcNukeKindMask() & kind_bit) == 0) {
        return false;
    }

    const bool hit_addr = addr_set.count(info.guest_address) != 0;
    const bool hit_pipe = !pipe_set.empty() && pipe_set.count(cur_pipe) != 0;
    if (!hit_addr && !hit_pipe && !nuke_all) {
        return false;
    }

    // Dedup per guest address: each texture is nuked at most once per
    // session. This prevents the per-frame multi-megabyte-write
    // pileup that stalls the emulator when nuke-all is on. If the game
    // refreshes a texture after our nuke, the fresh copy will be seen
    // — we accept that tradeoff for speed.
    static std::mutex seen_mutex;
    static std::unordered_set<u64> seen_addrs;
    {
        std::lock_guard lock{seen_mutex};
        if (!seen_addrs.insert(info.guest_address).second) {
            return false;
        }
    }

    // Skip likely normal-map / single-channel formats so lighting
    // direction stays intact and we can still see scene shape.
    const auto fmt = info.pixel_format;
    const bool is_normal_like =
        fmt == vk::Format::eBc4UnormBlock || fmt == vk::Format::eBc4SnormBlock ||
        fmt == vk::Format::eBc5UnormBlock || fmt == vk::Format::eBc5SnormBlock ||
        fmt == vk::Format::eBc6HUfloatBlock || fmt == vk::Format::eBc6HSfloatBlock;
    if (is_normal_like) {
        return false;
    }

    // Per-texture random tint via splitmix64 avalanche on the guest
    // address. Better distribution than a single multiply → colours
    // spread across the RGB cube instead of clustering in one hue.
    u64 h = info.guest_address;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    h = h ^ (h >> 31);
    const u32 r = static_cast<u32>((h >> 0) & 0xFF);
    const u32 g = static_cast<u32>((h >> 8) & 0xFF);
    const u32 b = static_cast<u32>((h >> 16) & 0xFF);
    const u32 rgba = 0xFF000000u | (b << 16) | (g << 8) | r;

    // RGB565 packed word + its duplicated u32 — the BC1 block layout
    // stores two colours in the first 4 bytes, then 4 bytes of indices.
    // Setting color_0 == color_1 makes the block decode to a solid
    // fill regardless of index values.
    const u16 rgb565 =
        static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    const u32 c565_pair =
        (static_cast<u32>(rgb565) << 16) | static_cast<u32>(rgb565);

    auto* dst8 = reinterpret_cast<u8*>(info.guest_address);
    const size_t size = info.guest_size;

    auto fill_u32 = [&](u32 v) {
        auto* p = reinterpret_cast<u32*>(dst8);
        const size_t n32 = size / 4;
        for (size_t i = 0; i < n32; ++i) p[i] = v;
    };

    const bool is_bc1 = fmt == vk::Format::eBc1RgbUnormBlock ||
                        fmt == vk::Format::eBc1RgbSrgbBlock ||
                        fmt == vk::Format::eBc1RgbaUnormBlock ||
                        fmt == vk::Format::eBc1RgbaSrgbBlock;
    const bool is_bc3 = fmt == vk::Format::eBc3UnormBlock ||
                        fmt == vk::Format::eBc3SrgbBlock;

    if (is_bc1) {
        // 8-byte blocks: [c565 lo, c565 hi, c565 lo, c565 hi, idx0..3]
        // color_0 == color_1 → entire block decodes to that colour
        // no matter what indices say.
        struct Bc1 { u32 colors; u32 indices; } block = {c565_pair, 0u};
        const size_t blocks = size / 8;
        auto* p = reinterpret_cast<Bc1*>(dst8);
        for (size_t i = 0; i < blocks; ++i) p[i] = block;
    } else if (is_bc3) {
        // 16-byte blocks: 8-byte alpha + 8-byte BC1 colour.
        // alpha a0=255, a1=255, any indices → solid 255 alpha.
        struct Bc3 { u16 a; u16 a_idx_lo; u32 a_idx_hi; u32 colors; u32 indices; };
        const Bc3 block = {0xFFFFu, 0u, 0u, c565_pair, 0u};
        const size_t blocks = size / 16;
        auto* p = reinterpret_cast<Bc3*>(dst8);
        for (size_t i = 0; i < blocks; ++i) p[i] = block;
    } else {
        // Uncompressed 32-bit & BC7 / other compressed formats: fall
        // back to repeating 4-byte pattern. Not a true solid for BC7
        // but still produces a per-texture distinctive result.
        fill_u32(rgba);
    }

    LOG_INFO(Render_Vulkan,
             "[dc-texnuke] nuked addr={:#x} size={}x{} fmt={} pipeline={:#018x} "
             "submit={} arm={}",
             info.guest_address, info.size.width, info.size.height,
             vk::to_string(info.pixel_format),
             g_driveclub_current_pipeline_hash.load(),
             g_driveclub_submit_index.load(),
             g_driveclub_arm_count.load());
    return true;
}

// Driveclub UBO smasher. Phase 17 — same philosophy as texnuke, but
// scoped to uniform buffers bound to scene draws. Overwrites the full
// UBO guest-memory range with a per-address splitmix64 hash pattern
// so every bound UBO becomes noise; any shader uniform that drives
// the blackout should therefore stop doing so.
//
// Toggles:
//   SHADPS4_DC_UBO_SMASH=1           master enable
//   SHADPS4_DC_UBO_SMASH_CB=0,1,2    cb index include list (default: any)
//   SHADPS4_DC_UBO_SMASH_MIN_SIZE=N  byte size lower bound (default 0)
//   SHADPS4_DC_UBO_SMASH_MAX_SIZE=N  byte size upper bound (default 0 = unlimited)
//   SHADPS4_DC_UBO_SMASH_ONLY_READ=1 only nuke read-only buffers
//                                     (real UBOs, skip SSBOs and formatted)
//
// Shares the broader gates with texnuke: SHADPS4_DC_NUKE_AFTER_ARM,
// SHADPS4_DC_NUKE_KIND (scene by default), plus the race-window guard.
bool IsDcUboSmashEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("SHADPS4_DC_UBO_SMASH");
        const bool v = env && env[0] == '1' && env[1] == '\0';
        if (v) {
            LOG_INFO(Render_Vulkan, "[dc-ubosmash] enabled (SHADPS4_DC_UBO_SMASH=1)");
        }
        return v;
    }();
    return on;
}

const std::unordered_set<u32>& GetDcUboSmashCbs() {
    static const std::unordered_set<u32> set = [] {
        std::unordered_set<u32> s;
        const char* env = std::getenv("SHADPS4_DC_UBO_SMASH_CB");
        if (!env || !env[0]) return s;
        std::string in = env;
        size_t pos = 0;
        while (pos < in.size()) {
            const size_t end = in.find(',', pos);
            std::string tok = in.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!tok.empty()) {
                s.insert(static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 0)));
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        if (!s.empty()) {
            LOG_INFO(Render_Vulkan, "[dc-ubosmash] cb filter: {} entries", s.size());
        }
        return s;
    }();
    return set;
}

u64 GetDcUboSmashMinSize() {
    static const u64 v = [] {
        const char* env = std::getenv("SHADPS4_DC_UBO_SMASH_MIN_SIZE");
        if (!env || !env[0]) return 0ull;
        return std::strtoull(env, nullptr, 0);
    }();
    return v;
}

u64 GetDcUboSmashMaxSize() {
    static const u64 v = [] {
        const char* env = std::getenv("SHADPS4_DC_UBO_SMASH_MAX_SIZE");
        if (!env || !env[0]) return 0ull;
        return std::strtoull(env, nullptr, 0);
    }();
    return v;
}

bool GetDcUboSmashOnlyRead() {
    static const bool v = [] {
        const char* env = std::getenv("SHADPS4_DC_UBO_SMASH_ONLY_READ");
        return env && env[0] == '1' && env[1] == '\0';
    }();
    return v;
}

// Writes the per-draw UBO at `base` with a per-address random tint.
// Returns true if a write happened (caller should then invalidate the
// buffer_cache range so the Vulkan UBO actually picks up the dirty
// bytes on the next ObtainBuffer).
// Driveclub tonemap exposure intercept. Phase 23.
//
// The tonemap compute pipeline 0x000002c995517e7f reads its
// per-frame exposure scalar from ssbo_5 at offset +16 dwords (byte
// offset 64). The PS4 race engine animates that value over the first
// ~30 s of a race — scene starts bright, game writes a dip, game
// writes the ramp-back. That dip is what the user sees as blackout.
//
// We already proved (Phase 22) that the consumer side can be
// mitigated by clamping / boosting inside the tonemap shader. This
// probe attacks the value at the *source*: every time the tonemap
// compute binds ssbo_5, we overwrite byte 64..67 of the guest buffer
// with `floatBitsToUint(1.0f)` and invalidate the buffer_cache range
// so the Vulkan upload picks it up. The game's animation write still
// lands but gets clobbered back to 1.0f before the compute reads.
//
// Enable with env SHADPS4_DC_EXPOSURE_PIN=1. Zero-cost when unset.
bool IsDcExposurePinEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("SHADPS4_DC_EXPOSURE_PIN");
        const bool v = env && env[0] == '1' && env[1] == '\0';
        if (v) {
            LOG_INFO(Render_Vulkan,
                     "[dc-exppin] enabled (SHADPS4_DC_EXPOSURE_PIN=1)");
        }
        return v;
    }();
    return on;
}

float GetDcExposurePinValue() {
    static const float v = [] {
        const char* env = std::getenv("SHADPS4_DC_EXPOSURE_PIN_VALUE");
        if (!env || !env[0]) return 1.0f;
        char* end = nullptr;
        const float f = std::strtof(env, &end);
        return end == env ? 1.0f : f;
    }();
    return v;
}


// SHADPS4_DC_RECORD=1 — consolidated recording harness for the
// "find the missing ambient light" investigation. One env var turns
// on the full capture set:
//
//   (a) Persistent session directory ~/Pictures/driveclub-runs/run-<wallclock>/
//       so multiple runs don't overwrite each other.
//   (b) Every ~120 submits, snapshot the three known lighting UBOs
//       (1936 byte scene-light, 1008 byte sky-dome, 224 byte sun) to
//       disk inside the session dir.
//   (c) Every ~60 submits, log wall-clock-timestamped slot values for
//       the known lighting offsets (1936: [24..31]; 1008: [84..90];
//       224: [48..51]).
//   (d) Every tonemap compute dispatch, log which images are bound and
//       their gpu_modified flag (to check whether upstream produced
//       HDR data at all).
//
// Correlate screenshot wall-clock against `[dc-record]` log lines to
// establish exact value-domains for dim / pitch-black / calibrated.
bool IsDcRecordEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_RECORD");
        const bool on = env && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan, "[dc-record] enabled (SHADPS4_DC_RECORD=1)");
        }
        return on;
    }();
    return enabled;
}

const std::string& DcRecordDir() {
    static const std::string dir = [] {
        const char* home = std::getenv("HOME");
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));
        std::string d = fmt::format("{}/Pictures/driveclub-runs/run-{}",
                                     home ? home : "/tmp", stamp);
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        LOG_INFO(Render_Vulkan, "[dc-record] session dir: {}", d);
        return d;
    }();
    return dir;
}

// Helper — wall-clock timestamp formatted HH:MM:SS.mmm.
std::string DcRecordTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    char time_buf[16];
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&tt));
    return fmt::format("{}.{:03d}", time_buf, static_cast<int>(ms));
}

// Per-submit periodic recorder: dumps 1936/1008/224 UBOs every N
// submits to disk, and logs slot values every M submits.
void MaybeRecordDriveclubLighting(VAddr base, u64 size, u32 cb_idx) {
    if (!IsDcRecordEnabled()) return;
    if (!IsDriveclubGuardEnabled()) return;
    if (g_driveclub_race_window.load() == 0) return;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return;
    }
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Scene)) return;
    if (base == 0) return;

    const u64 submit = g_driveclub_submit_index.load();
    const auto* bytes = reinterpret_cast<const u8*>(base);
    auto rf = [&](size_t slot) -> float {
        return *reinterpret_cast<const float*>(bytes + slot * 4);
    };

    // Recognize each known UBO by (size, signature) and extract its
    // key slot values. Throttle per submit.
    static std::mutex m;
    static u64 last_sample_submit_1936 = 0;
    static u64 last_sample_submit_1008 = 0;
    static u64 last_sample_submit_224 = 0;
    static u64 last_snapshot_submit = 0;

    const bool do_snapshot = (submit - last_snapshot_submit >= 120);

    if (size == 1936) {
        const float a24 = rf(24), a25 = rf(25), a26 = rf(26), a27 = rf(27);
        if (!(a24 >= 0.f && a24 < 10000.f)) return;  // signature
        // Log every 60 submits (once per tick)
        bool do_log = false;
        {
            std::lock_guard l{m};
            if (submit - last_sample_submit_1936 >= 60) {
                last_sample_submit_1936 = submit;
                do_log = true;
            }
        }
        if (do_log) {
            LOG_INFO(Render_Vulkan,
                     "[dc-record-1936] wc={} submit={} ambient[24..27]="
                     "{:.3g}/{:.3g}/{:.3g}/{:.3g} fade[38]={:.3g} "
                     "fade[48]={:.3g} fade[50]={:.3g}",
                     DcRecordTimestamp(), submit, a24, a25, a26, a27,
                     rf(38), rf(48), rf(50));
        }
        if (do_snapshot) {
            std::lock_guard l{m};
            if (submit - last_snapshot_submit >= 120) {
                last_snapshot_submit = submit;
                const std::string path = fmt::format(
                    "{}/snap_{}_sub{:06}_1936.bin", DcRecordDir(),
                    DcRecordTimestamp(), submit);
                std::FILE* f = std::fopen(path.c_str(), "wb");
                if (f) {
                    std::fwrite(bytes, 1, size, f);
                    std::fclose(f);
                }
            }
        }
    } else if (size == 1008) {
        const float v84 = rf(84), v85 = rf(85), v86 = rf(86);
        if (!(v84 >= 0.f && v84 < 10000.f)) return;
        bool do_log = false;
        {
            std::lock_guard l{m};
            if (submit - last_sample_submit_1008 >= 60) {
                last_sample_submit_1008 = submit;
                do_log = true;
            }
        }
        if (do_log) {
            LOG_INFO(Render_Vulkan,
                     "[dc-record-1008] wc={} submit={} sky[84..90]="
                     "{:.3g}/{:.3g}/{:.3g}/_/{:.3g}/{:.3g}/{:.3g}",
                     DcRecordTimestamp(), submit, v84, v85, v86,
                     rf(88), rf(89), rf(90));
        }
        if (do_snapshot) {
            const std::string path = fmt::format(
                "{}/snap_{}_sub{:06}_1008.bin", DcRecordDir(),
                DcRecordTimestamp(), submit);
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (f) {
                std::fwrite(bytes, 1, size, f);
                std::fclose(f);
            }
        }
    } else if (size == 224) {
        const float f0 = rf(0), f4 = rf(4);
        if (!(f0 > 4.5f && f0 < 7.0f)) return;  // sun UBO signature
        if (!(f4 > 2.5f && f4 < 3.5f)) return;
        bool do_log = false;
        {
            std::lock_guard l{m};
            if (submit - last_sample_submit_224 >= 60) {
                last_sample_submit_224 = submit;
                do_log = true;
            }
        }
        if (do_log) {
            LOG_INFO(Render_Vulkan,
                     "[dc-record-224] wc={} submit={} sun_fade[48..51]="
                     "{:.3g}/{:.3g}/{:.3g}/{:.3g}",
                     DcRecordTimestamp(), submit, rf(48), rf(49), rf(50),
                     rf(51));
        }
        if (do_snapshot) {
            const std::string path = fmt::format(
                "{}/snap_{}_sub{:06}_224.bin", DcRecordDir(),
                DcRecordTimestamp(), submit);
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (f) {
                std::fwrite(bytes, 1, size, f);
                std::fclose(f);
            }
        }
    }
}

// Phase 29 — "calibrate at arm" pin.
//
// The 1936-byte scene-lighting UBO has ~30 one-shot slots in
// [144..295] that stay denormal/uninitialised for the first ~90s of
// a Canada race and produce the visibly-dim-then-pitch-black arc.
// The recalibration moment is the game finally writing real values
// into those slots. We captured the recalibrated state as a byte-
// perfect snapshot in tools/driveclub_pin_snapshots/.
//
// SHADPS4_DC_CALIBRATE_AT_ARM=1 + SHADPS4_DC_CALIBRATE_FILE=<path>
// loads the snapshot and, on the very first race-arm-qualified
// BindBuffers call for a 1936-byte scene UBO, memcpys the snapshot
// over the game's (partially-initialised) bytes. After that single
// write, the game takes over naturally — continuously-animated slots
// update every frame; the one-shot slots were populated by our memcpy
// so the scene renders correctly from the start.
//
// Fires once per race-arm bump. Re-entering a race bumps arm_count,
// resets the "did I fire yet" latch, and re-applies.
const std::array<u8, 1936>* GetDcCalibrateSnapshot() {
    static const std::array<u8, 1936>* snap = [] () -> const std::array<u8, 1936>* {
        const char* env = std::getenv("SHADPS4_DC_CALIBRATE_FILE");
        if (!env || !env[0]) return nullptr;
        static std::array<u8, 1936> buf{};
        std::FILE* fp = std::fopen(env, "rb");
        if (!fp) {
            LOG_WARNING(Render_Vulkan,
                        "[dc-calibrate] snapshot file {} missing", env);
            return nullptr;
        }
        const size_t n = std::fread(buf.data(), 1, buf.size(), fp);
        std::fclose(fp);
        if (n != buf.size()) {
            LOG_WARNING(Render_Vulkan,
                        "[dc-calibrate] snapshot file {} short read {}/{}",
                        env, n, buf.size());
            return nullptr;
        }
        LOG_INFO(Render_Vulkan,
                 "[dc-calibrate] snapshot loaded from {}", env);
        return &buf;
    }();
    return snap;
}

bool IsDcCalibrateAtArmEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_CALIBRATE_AT_ARM");
        const bool on = env && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan,
                     "[dc-calibrate] enabled (SHADPS4_DC_CALIBRATE_AT_ARM=1)");
        }
        return on;
    }();
    return enabled;
}

bool MaybeCalibrateAtArmDriveclub(VAddr base, u64 size, u32 cb_idx) {
    if (!IsDcCalibrateAtArmEnabled()) return false;
    if (!IsDriveclubGuardEnabled()) return false;
    // Note: no race_window check — that counter decrements to 0 between
    // race-gate pipeline fires, which would block the pin for most of
    // the race. Once armed above threshold, pin fires continuously.
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    if (size != 1936) return false;
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Scene)) return false;
    if (base == 0) return false;
    (void)cb_idx;

    const auto* snap = GetDcCalibrateSnapshot();
    if (!snap) return false;

    auto* b = reinterpret_cast<u8*>(base);

    // Signature: ambient slots at [24..26] must look like non-negative
    // intensity magnitudes. Other 1936-byte buffers (flags, etc.)
    // would fail this check.
    auto rf = [&](size_t slot) -> float {
        return *reinterpret_cast<const float*>(b + slot * 4);
    };
    const float a24 = rf(24), a25 = rf(25), a26 = rf(26);
    if (!(std::isfinite(a24) && a24 >= 0.f && a24 < 10000.f)) return false;
    if (!(std::isfinite(a25) && a25 >= 0.f && a25 < 10000.f)) return false;
    if (!(std::isfinite(a26) && a26 >= 0.f && a26 < 10000.f)) return false;

    // Selective pin — classified via timeline analysis across 49
    // snapshots. These 11 slots stay at placeholder / near-zero /
    // uninitialised values for the entire first ~90s of a Canada
    // race, then jump to massively different values at the 1:30
    // recalibration moment. They are the one-shot initialisation
    // state the emulator fails to populate early. Pinning them to
    // the calibrated snapshot's values gives the scene the
    // initial state the game expects to have had from race-start,
    // without touching the 247 continuously-animated slots that
    // drive natural TOD evolution.
    // 89 slots that differ structurally between the pitch-black and
    // recalibrated snapshots, excluding the well-known continuously-
    // animated slots (ambient [24..31], fade [38][48][50], sun
    // directions, alpha factors). These are the actual "stuck at
    // placeholder" slots that the game populates during the 1:30
    // recalibration event and that scene shaders need from race-start
    // on real PS4.
    constexpr size_t kStuckJumpSlots[] = {
        4, 84, 85, 100, 101, 104, 105, 112, 122, 123, 128, 129, 132,
        138, 139, 144, 145, 146, 147, 148, 149, 150, 152, 153, 176,
        178, 180, 181, 182, 186, 195, 196, 197, 200, 201, 202, 204,
        205, 212, 213, 214, 215, 216, 224, 225, 226, 227, 231, 232,
        233, 240, 241, 242, 243, 245, 246, 247, 248, 249, 250, 251,
        252, 253, 256, 257, 260, 268, 270, 271, 273, 275, 276, 277,
        278, 280, 281, 284, 285, 286, 287, 288, 289, 290, 291, 292,
        293, 294, 295, 312,
    };
    const float* sfloats = reinterpret_cast<const float*>(snap->data());
    float* tfloats = reinterpret_cast<float*>(b);
    for (size_t slot : kStuckJumpSlots) {
        tfloats[slot] = sfloats[slot];
    }
    static std::mutex log_m;
    static u64 last_log_submit = 0;
    const u64 submit = g_driveclub_submit_index.load();
    std::lock_guard l{log_m};
    if (submit - last_log_submit >= 120) {
        last_log_submit = submit;
        LOG_INFO(Render_Vulkan,
                 "[dc-calibrate] applied at submit={} arm={} addr={:#x}",
                 submit, g_driveclub_arm_count.load(), base);
    }
    return true;
}

// Dump the first 128 bytes of every SSBO bound to the tonemap compute
// on every bind. Rate-limited to one dump set per ~30 submits so the
// log stays manageable across a race. Enabled by
// SHADPS4_DC_TONEMAP_SSBODUMP=1.
void MaybeDumpDriveclubTonemapSsbo(VAddr base, u64 size, u32 cb_idx) {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_TONEMAP_SSBODUMP");
        const bool v = env && env[0] == '1' && env[1] == '\0';
        if (v) {
            LOG_INFO(Render_Vulkan,
                     "[dc-tmdump] enabled (SHADPS4_DC_TONEMAP_SSBODUMP=1)");
        }
        return v;
    }();
    if (!enabled) return;
    if (!IsDriveclubGuardEnabled()) return;
    if (g_driveclub_race_window.load() == 0) return;
    if (g_driveclub_current_pipeline_hash.load() != 0x000002c995517e7full) return;
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Compute)) return;
    if (base == 0 || size == 0) return;

    // Per-submit+cb_idx dedup: one dump per (submit, cb_idx) tuple.
    // Additionally throttle: only dump every 30 submits to keep log size
    // reasonable over a long race.
    const u64 submit = g_driveclub_submit_index.load();
    static std::mutex dedup_mutex;
    static u64 last_dump_submit = 0;
    {
        std::lock_guard lock{dedup_mutex};
        if (cb_idx == 0) {
            if (submit - last_dump_submit < 30) return;
            last_dump_submit = submit;
        }
    }

    const size_t len = std::min<size_t>(128, size);
    std::string hex;
    hex.reserve(len * 2);
    const auto* bytes = reinterpret_cast<const u8*>(base);
    for (size_t j = 0; j < len; ++j) {
        fmt::format_to(std::back_inserter(hex), "{:02x}", bytes[j]);
    }
    LOG_INFO(Render_Vulkan,
             "[dc-tmdump] submit={} cb{} addr={:#x} bytes={}",
             submit, cb_idx, base, hex);
}

// SHADPS4_DC_UBOSNAPSHOT=<period> — dump every scene-material UBO's
// raw bytes to /tmp/dc-snapshots/ once every <period> submits after the
// race window is armed.
//
// Mechanism: the first time we see a new submit index that meets the
// "due for snapshot" threshold, mark that entire submit as active and
// create a timestamped output directory for it. Inside the active
// submit, every BindBuffers call for a scene pipeline dumps each
// bound UBO (deduped per pipeline+cb). When the submit index advances,
// the snapshot closes. Schedule the next due-submit = current + period.
//
// Goal: capture bright / dim / recovered states over a single race so
// we can diff their UBO contents and pinpoint which bytes carry the
// blackout fade curve.
u32 GetDcUboSnapshotPeriod() {
    static const u32 period = [] {
        const char* env = std::getenv("SHADPS4_DC_UBOSNAPSHOT");
        if (!env || !env[0]) return 0u;
        const u32 n = static_cast<u32>(std::strtoul(env, nullptr, 0));
        if (n > 0) {
            LOG_INFO(Render_Vulkan,
                     "[dc-ubosnap] enabled, period={} submits "
                     "(SHADPS4_DC_UBOSNAPSHOT)", n);
        }
        return n;
    }();
    return period;
}

void MaybeUboSnapshotDriveclub(VAddr base, u64 size, u32 cb_idx) {
    const u32 period = GetDcUboSnapshotPeriod();
    if (period == 0) return;
    if (!IsDriveclubGuardEnabled()) return;
    if (g_driveclub_race_window.load() == 0) return;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return;
    }
    // Only scene-material draws — skip compute (tonemap/histogram), UI,
    // and post-fx. Scene covers the G-buffer fills which are the ones
    // the game's lighting/fade state actually feeds.
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Scene)) return;
    if (base == 0 || size == 0) return;

    const u64 submit = g_driveclub_submit_index.load();
    const u64 pipe_hash = g_driveclub_current_pipeline_hash.load();

    static std::mutex m;
    static u64 next_due_submit = 0;
    static u64 active_snap_submit = ~0ull;  // ~0 = no active snap
    static u32 snap_id = 0;
    static std::string snap_dir;
    static std::unordered_set<u64> dedup;

    bool dump_now = false;
    std::string local_dir;
    {
        std::lock_guard lock{m};
        // Advance snapshot window if this submit has left the active one.
        if (submit != active_snap_submit) {
            if (active_snap_submit != ~0ull) {
                // Closing previous snapshot.
                active_snap_submit = ~0ull;
                dedup.clear();
            }
            // Is this submit the new trigger?
            if (next_due_submit == 0) {
                next_due_submit = submit;  // first valid submit triggers
                                           // immediately.
            }
            if (submit >= next_due_submit) {
                active_snap_submit = submit;
                snap_id += 1;
                next_due_submit = submit + period;
                const auto tnow = std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now());
                snap_dir = fmt::format(
                    "/tmp/dc-snapshots/snap_{:04}_submit{}_{}", snap_id,
                    submit, static_cast<long long>(tnow));
                std::error_code ec;
                std::filesystem::create_directories(snap_dir, ec);
                LOG_INFO(Render_Vulkan,
                         "[dc-ubosnap] opening snap #{} submit={} "
                         "arm_count={} dir={}",
                         snap_id, submit,
                         g_driveclub_arm_count.load(), snap_dir);
            }
        }
        if (submit == active_snap_submit) {
            const u64 key = (pipe_hash ^ (static_cast<u64>(cb_idx) << 56));
            if (dedup.insert(key).second) {
                dump_now = true;
                local_dir = snap_dir;
            }
        }
    }
    if (!dump_now) return;

    const size_t len = static_cast<size_t>(size);
    std::string fname = fmt::format(
        "{}/pipe_{:016x}_cb{}_addr{:x}_sz{}.bin", local_dir, pipe_hash,
        cb_idx, static_cast<u64>(base), len);
    std::FILE* fp = std::fopen(fname.c_str(), "wb");
    if (fp) {
        std::fwrite(reinterpret_cast<const void*>(base), 1, len, fp);
        std::fclose(fp);
    } else {
        LOG_WARNING(Render_Vulkan,
                    "[dc-ubosnap] failed to open {} errno={}",
                    fname, errno);
    }
}

// SHADPS4_DC_LIGHT_PIN=1 — Phase 26 finding: a 1936-byte cb1 ring-
// buffer bound by two specific scene-material pipelines carries four
// co-modulated floats that the engine multiplies by the blackout fade
// factor (~0.094 at the dim minimum). The UBO rotates addresses every
// frame so we can't pin a static address — we pin by (pipeline, size,
// cb_idx) instead, and overwrite the four byte offsets with the
// post-recovery Munnar baseline values captured from a bright-state
// snapshot.
//
// If the pin visually lifts the blackout, these four floats are the
// single point where the fade multiplier lands in GPU-visible state.
bool IsDcLightPinEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_LIGHT_PIN");
        const bool v = env && env[0] == '1' && env[1] == '\0';
        if (v) {
            LOG_INFO(Render_Vulkan, "[dc-lightpin] enabled (SHADPS4_DC_LIGHT_PIN=1)");
        }
        return v;
    }();
    return enabled;
}

// SHADPS4_DC_LIGHT_PIN_WINDOW — submits after first-arm to keep the
// pin active. Default 1200 (~20s @ 60fps). Bump higher for slow
// time-lapse settings (timelapse 1x can need 60+ seconds). Set 0 to
// disable auto-expiry entirely.
// Scale factor applied to the snapshot's clamp-min targets.
// 1.0 = use snapshot values as-is.
// 2.0 = clamp-min to 2x the snapshot values (stronger light strength).
float GetDcLightPinBoost() {
    static const float b = [] {
        const char* env = std::getenv("SHADPS4_DC_LIGHT_PIN_BOOST");
        float v = env && env[0] ? std::strtof(env, nullptr) : 1.0f;
        if (v <= 0.f) v = 1.0f;
        LOG_INFO(Render_Vulkan,
                 "[dc-lightpin] clamp-target boost = {}x "
                 "(SHADPS4_DC_LIGHT_PIN_BOOST)", v);
        return v;
    }();
    return b;
}

u64 GetDcLightPinWindow() {
    static const u64 w = [] {
        const char* env = std::getenv("SHADPS4_DC_LIGHT_PIN_WINDOW");
        u64 v = env && env[0] ? std::strtoull(env, nullptr, 0) : 9000ull;
        LOG_INFO(Render_Vulkan,
                 "[dc-lightpin] auto-expire window = {} submits "
                 "(SHADPS4_DC_LIGHT_PIN_WINDOW)", v);
        return v;
    }();
    return w;
}

// Snapshot-file loader factory. Each caller owns its own static
// byte-buffer (one per size/tag).
template <size_t N>
const std::array<u8, N>* LoadDcPinSnapshot(const char* env_name,
                                             const char* log_tag) {
    const char* env = std::getenv(env_name);
    if (!env || !env[0]) return nullptr;
    auto* buf = new std::array<u8, N>{};
    std::FILE* fp = std::fopen(env, "rb");
    if (!fp) {
        LOG_WARNING(Render_Vulkan, "{} snapshot file {} missing",
                    log_tag, env);
        delete buf;
        return nullptr;
    }
    const size_t n = std::fread(buf->data(), 1, buf->size(), fp);
    std::fclose(fp);
    if (n != buf->size()) {
        LOG_WARNING(Render_Vulkan,
                    "{} snapshot file {} short read {}/{}", log_tag,
                    env, n, buf->size());
        delete buf;
        return nullptr;
    }
    LOG_INFO(Render_Vulkan, "{} snapshot overwrite enabled from {}",
             log_tag, env);
    return buf;
}

const std::array<u8, 1936>* GetDcLightPinSnapshot() {
    static const std::array<u8, 1936>* snap =
        LoadDcPinSnapshot<1936>("SHADPS4_DC_LIGHT_PIN_FILE", "[dc-lightpin]");
    return snap;
}

const std::array<u8, 224>* GetDcLightPinSnapshot224() {
    static const std::array<u8, 224>* snap = LoadDcPinSnapshot<224>(
        "SHADPS4_DC_LIGHT_PIN_FILE_224", "[dc-lightpin-224]");
    return snap;
}

// Lifecycle state machine for a fade-UBO pin. One instance per pin
// (one per UBO shape). Observes the game's own animation state via
// `sample_value` and transitions idle -> engaged -> expired.
//
// Key properties:
//   - Samples value ONCE per submit (before any pin write happens),
//     so subsequent bindings that read our overwritten bytes never
//     affect the state machine.
//   - Engagement: value drops below engage_below once.
//   - Expiration: value stays >= recover_above for recover_frames
//     consecutive distinct submits. Debounced so a single bounce
//     during the fade curve's ramp doesn't retire the pin early.
//   - Safety cap: env window bounds the engaged duration regardless.
struct FadePinLifecycle {
    std::atomic<u32> state{0};          // 0=idle, 1=engaged, 2=expired
    std::atomic<u64> first_engaged{0};
    std::atomic<u64> sampled_submit{~0ull};
    std::atomic<float> sampled_value{0.f};
    std::atomic<u32> recover_streak{0};
    std::atomic<u32> last_arm_count{0};

    // Returns true iff the pin should write this invocation.
    //
    // Engagement is content-driven (auto-detects the fade via the
    // game's own UBO write to `observed_now`). Expiration priority:
    //   1. `headlights_on` — the game flipped the car-headlights
    //      active flag (only observable on dusk/night tracks).
    //   2. Time-based upper bound via SHADPS4_DC_LIGHT_PIN_WINDOW —
    //      safety net for tracks where headlights never turn on
    //      (daytime / noon / bright conditions).
    //
    // `log_tag` is used only for state-transition logs.
    bool Tick(float observed_now,
              float engage_below,
              bool headlights_on,
              const char* log_tag) {
        // Detect a fresh race arm (user re-entered a race after one
        // already completed). Reset state so we can re-engage.
        const u32 arm = g_driveclub_arm_count.load();
        const u32 prev_arm = last_arm_count.load();
        if (arm > prev_arm) {
            last_arm_count.store(arm);
            if (state.load() != 0) {
                state.store(0);
                recover_streak.store(0);
                first_engaged.store(0);
                sampled_submit.store(~0ull);
                LOG_INFO(Render_Vulkan,
                         "{} reset (arm {} → {}, new race)",
                         log_tag, prev_arm, arm);
            }
        }
        if (state.load() == 2) return false;
        const u64 submit = g_driveclub_submit_index.load();

        // One sample per submit.
        float value = sampled_value.load();
        if (sampled_submit.load() != submit) {
            value = observed_now;
            sampled_value.store(value);
            sampled_submit.store(submit);
        }

        if (state.load() == 0) {
            if (value >= 0.f && value < engage_below) {
                state.store(1);
                first_engaged.store(submit);
                LOG_INFO(Render_Vulkan,
                         "{} engaged at submit {} (value={:.4g})",
                         log_tag, submit, value);
            } else {
                return false;
            }
        }

        // state == 1 (engaged). Primary expire: headlights-on flag.
        if (headlights_on) {
            state.store(2);
            LOG_INFO(Render_Vulkan,
                     "{} expired at submit {} (headlights-on detected)",
                     log_tag, submit);
            return false;
        }

        // Safety expire: time-based upper bound.
        const u64 window = GetDcLightPinWindow();
        if (window > 0 && submit - first_engaged.load() > window) {
            state.store(2);
            LOG_INFO(Render_Vulkan,
                     "{} safety-expired at submit {} (window={} submits)",
                     log_tag, submit, window);
            return false;
        }
        return true;
    }
};

// 1008-byte sky/ambient UBO — slots [84..86] and [88..90] are two
// neutral RGB ambient triples. Daytime 18/18/18 decays to 2.2 at
// night. Clamp-min floor at daytime level so ambient never drops
// below the "full daylight sky-dome" contribution.
bool MaybePinDriveclubAmbient1008(VAddr base, u64 size, u32 cb_idx) {
    if (!IsDcLightPinEnabled()) return false;
    if (!IsDriveclubGuardEnabled()) return false;
    if (g_driveclub_race_window.load() == 0) return false;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    if (size != 1008) return false;
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Scene)) return false;
    if (base == 0) return false;
    (void)cb_idx;

    auto* b = reinterpret_cast<u8*>(base);
    auto rf = [&](size_t slot) -> float {
        return *reinterpret_cast<const float*>(b + slot * 4);
    };

    // Signature: slots [84,85,86] and [88,89,90] are a neutral RGB
    // triple — all three values within 5% of each other.
    const float v84 = rf(84), v85 = rf(85), v86 = rf(86);
    if (!(v84 >= 0.f && v84 < 10000.f)) return false;
    if (!(v85 >= 0.f && v85 < 10000.f)) return false;
    if (!(v86 >= 0.f && v86 < 10000.f)) return false;
    // Neutral-ish: close together (within 20%) — allows daytime (all 18)
    // and night (all 2.2) but not random 3-float garbage.
    const float mx = std::max({v84, v85, v86});
    const float mn = std::min({v84, v85, v86});
    if (mx > 0.01f && (mx - mn) / mx > 0.3f) return false;

    // Clamp floor at 18.0 per channel (observed Canada daytime value).
    // Touches only when game writes below 18 (i.e. as TOD advances).
    constexpr float kAmbientFloor = 500.0f;  // DIAGNOSTIC: if this doesn't
                                             // light the scene, the 1008 UBO
                                             // isn't the right target.
    constexpr size_t kSlots[] = {84, 85, 86, 88, 89, 90};
    bool touched = false;
    for (size_t slot : kSlots) {
        float* p = reinterpret_cast<float*>(b + slot * 4);
        if (*p < kAmbientFloor) {
            *p = kAmbientFloor;
            touched = true;
        }
    }
    if (touched) {
        static std::mutex m;
        static u64 last_log = 0;
        const u64 sub = g_driveclub_submit_index.load();
        std::lock_guard l{m};
        if (sub - last_log >= 300) {
            last_log = sub;
            LOG_INFO(Render_Vulkan,
                     "[dc-ambient-1008] submit={} addr={:#x} floor "
                     "applied (daytime v84/85/86 = {:.2f}/{:.2f}/{:.2f})",
                     sub, base, v84, v85, v86);
        }
    }
    return touched;
}

bool MaybePinDriveclubSunLightUbo(VAddr base, u64 size, u32 cb_idx) {
    if (!IsDcLightPinEnabled()) return false;
    if (!IsDriveclubGuardEnabled()) return false;
    if (g_driveclub_race_window.load() == 0) return false;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    if (size != 224) return false;
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Scene)) return false;
    if (base == 0) return false;
    (void)cb_idx;

    // Signature check at known-stable offsets: the non-animated part of
    // this UBO has distinctive float values we can use as a fingerprint.
    auto* b = reinterpret_cast<u8*>(base);
    const float f0 = *reinterpret_cast<const float*>(b + 0x0);
    const float f4 = *reinterpret_cast<const float*>(b + 0x10);
    const float f8 = *reinterpret_cast<const float*>(b + 0x20);
    if (!(f0 > 4.5f && f0 < 7.0f)) return false;       // [0] ~ 5.79
    if (!(f4 > 2.5f && f4 < 3.5f)) return false;       // [4] == 3
    if (!(f8 > 1e-8f && f8 < 1e-5f)) return false;     // [8] ~ 5.79e-7

    // Lifecycle — runs AFTER shape+signature match so we only observe
    // transitions on the real sun UBO. vc8 = sun key-light intensity
    // (dim ≈ 0.01, recovered ≈ 0.29). Engage below 0.05, expire after
    // 30 sustained frames at >= 0.26 (leave margin under fully-recovered
    // so we don't retire mid-ramp).
    static FadePinLifecycle lifecycle;
    const float vc8 = *reinterpret_cast<const float*>(b + 0xc8);
    if (!lifecycle.Tick(vc8, /*engage_below=*/0.05f,
                        g_driveclub_headlights_on.load(),
                        "[dc-lightpin-224]")) {
        return false;
    }

    bool touched = false;
    if (const auto* snap = GetDcLightPinSnapshot224()) {
        // Restrict to the 4 known fade slots: [48], [49], [50], [51].
        const float* sfloats = reinterpret_cast<const float*>(snap->data());
        float* tfloats = reinterpret_cast<float*>(b);
        const float boost = GetDcLightPinBoost();
        constexpr size_t kFadeSlots[] = {48, 49, 50, 51};
        for (size_t slot : kFadeSlots) {
            const float target = sfloats[slot] * boost;
            if (std::abs(tfloats[slot]) < 0.5f * std::abs(target)) {
                tfloats[slot] = target;
                touched = true;
            }
        }
    } else {
        // Four specific offsets that drop 97% at dim — slot [48-51].
        struct S { size_t off; float v; };
        constexpr S slots[] = {
            {0xc0, 0.7483f},
            {0xc4, -0.7483f},
            {0xc8, 0.28996f},
            {0xcc, 0.064498f},
        };
        for (const auto& s : slots) {
            auto* f = reinterpret_cast<float*>(b + s.off);
            if (std::abs(*f) < 0.5f * std::abs(s.v)) {
                *f = s.v;
                touched = true;
            }
        }
    }
    if (touched) {
        static std::mutex m;
        static u64 last_log_submit = 0;
        const u64 submit = g_driveclub_submit_index.load();
        std::lock_guard l{m};
        if (submit - last_log_submit >= 60) {
            last_log_submit = submit;
            LOG_INFO(Render_Vulkan,
                     "[dc-lightpin-224] submit={} addr={:#x} applied",
                     submit, base);
        }
    }
    return touched;
}

// Returns true when at least one slot was rewritten; the caller must
// then invalidate the buffer_cache range so the write propagates to
// Vulkan's upload of this buffer.
bool MaybePinDriveclubLightFade(VAddr base, u64 size, u32 cb_idx) {
    if (!IsDcLightPinEnabled()) return false;
    if (!IsDriveclubGuardEnabled()) return false;
    if (g_driveclub_race_window.load() == 0) return false;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    // Match the 1936-byte scene-lighting UBO shape FIRST so the
    // lifecycle state machine below only observes the buffer that
    // actually carries the fade animation.
    if (size != 1936) return false;
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Scene)) return false;
    if (base == 0) return false;
    (void)cb_idx;

    // Content sanity: cross-track robust check using the ambient
    // light slots themselves. These are RGB intensity magnitudes —
    // always non-negative and bounded. Earlier signature using
    // vc0/vc8 was too Munnar-specific (vc0 is positive on Canada
    // daytime but negative on Munnar 19:30), which prevented the
    // pin from ever firing on bright-daytime tracks.
    auto* base_bytes_early = reinterpret_cast<u8*>(base);
    auto read_f = [&](size_t off) -> float {
        return *reinterpret_cast<const float*>(base_bytes_early + off);
    };
    {
        const float a24 = read_f(24 * 4);
        const float a25 = read_f(25 * 4);
        const float a26 = read_f(26 * 4);
        // Loosest possible signature: just need non-crazy finite values
        // at the ambient slots. Covers daytime/night/any track.
        auto ok = [](float v) {
            return std::isfinite(v) && v > -10.f && v < 10000.f;
        };
        const bool sig = ok(a24) && ok(a25) && ok(a26);
        // Periodic diagnostic so we can see what state the pin is in.
        static std::mutex diag_m;
        static u64 last_diag_submit = 0;
        const u64 sub = g_driveclub_submit_index.load();
        {
            std::lock_guard l{diag_m};
            if (sub - last_diag_submit >= 300) {
                last_diag_submit = sub;
                LOG_INFO(Render_Vulkan,
                         "[dc-lightpin] sample submit={} sig={} "
                         "a24={:.3g} a25={:.3g} a26={:.3g}",
                         sub, sig, a24, a25, a26);
            }
        }
        if (!sig) return false;
    }

    // Check this buffer for the headlights-on signal: slots
    // [73, 77, 81, 85, 109, 113] at offsets 0x124..0x1c4 flip from
    // 0.0 to 1.0 when the game's own headlight lights activate. Any
    // slot hitting >= 0.99 means headlights just came on — propagate
    // to the shared flag so both the 1936-byte and 224-byte pins
    // expire on the same event.
    //
    // Note: not every 1936-byte buffer carries this flag array — the
    // fade-UBO's slot [73] stays 0 always. But the flags-UBO (a
    // different 1936-byte buffer with the same size signature) does
    // transition here. Since both buffers pass our pin's shape check,
    // we'll see both and the flag is set on whichever binding carries
    // the signal.
    constexpr size_t kLightFlagOffsets[] = {
        0x124, 0x134, 0x144, 0x154, 0x1b4, 0x1c4,
    };
    for (size_t off : kLightFlagOffsets) {
        if (read_f(off) >= 0.99f) {
            if (!g_driveclub_headlights_on.exchange(true)) {
                LOG_INFO(Render_Vulkan,
                         "[dc-lightpin] headlights-on detected at "
                         "offset {:#x}", off);
            }
            break;
        }
    }

    // Ambient-floor clamp: runs UNCONDITIONALLY whenever the right
    // UBO shape + signature match. Independent of the scripted-fade
    // lifecycle because the ambient drop to zero is a natural TOD
    // event (not the scripted blackout) — so it happens on tracks
    // and states where the lifecycle never engages.
    //
    // Slots [24..28] are RGB(W+) ambient/sky intensity components
    // that go from ~70/52/20/13/70 at daytime to exact 0 at night.
    // Scene has no ambient → pitch black. We clamp-floor each slot
    // at 25% of the observed daytime value so nightfall retains a
    // twilight ambient. Daytime values (already above floor) pass
    // through untouched.
    auto* base_bytes_floor = base_bytes_early;
    float* tfloats_floor = reinterpret_cast<float*>(base_bytes_floor);
    struct AmbientFloor { size_t slot; float floor; };
    // Sun intensity slots, clamp-floor at 50% of daytime nominal.
    // Ambient/fill-light comes from the 1008-byte sky UBO pin below.
    constexpr AmbientFloor kAmbient[] = {
        {24, 35.49f},  // 70.979 * 0.5
        {25, 26.33f},
        {26, 10.35f},
        {27,  6.80f},
        {28, 35.49f},
        {29, 26.33f},
        {30, 10.35f},
        {31,  6.80f},
    };
    bool ambient_touched = false;
    for (const auto& a : kAmbient) {
        if (tfloats_floor[a.slot] < a.floor) {
            tfloats_floor[a.slot] = a.floor;
            ambient_touched = true;
        }
    }
    if (ambient_touched) {
        static std::mutex amb_log_m;
        static u64 last_amb_log = 0;
        const u64 sub = g_driveclub_submit_index.load();
        std::lock_guard l{amb_log_m};
        if (sub - last_amb_log >= 120) {
            last_amb_log = sub;
            LOG_INFO(Render_Vulkan,
                     "[dc-lightpin] ambient-floor applied submit={} "
                     "addr={:#x}", sub, base);
        }
    }

    // Lifecycle on v98 at offset 0x98 (dim ≈ 7.5, recovered ≈ 60).
    // Engage below 15. Primary expire on headlights-on; safety expire
    // via SHADPS4_DC_LIGHT_PIN_WINDOW. Gates the fade-slot clamp
    // below, but does NOT gate the ambient floor above.
    static FadePinLifecycle lifecycle;
    const bool fade_engaged = lifecycle.Tick(
        read_f(0x98), /*engage_below=*/15.f,
        g_driveclub_headlights_on.load(),
        "[dc-lightpin]");
    if (!fade_engaged) {
        // Return true iff we touched the buffer via ambient floor
        // (caller invalidates buffer_cache on true).
        return ambient_touched;
    }
    // Byte offsets identified by the bright/dim/recovered three-way
    // diff on Munnar India 19:30. The "recovered" value is what the
    // game settles on after the blackout animation completes; pinning
    // to it should look visually correct on that track.
    struct PinSlot { size_t off; float value; };
    constexpr PinSlot slots[] = {
        {0x98, 59.9375f},
        {0xc0, -20.4998f},
        {0xc8, 56.3229f},
        {0xf4, 0.000999009f},
    };
    auto* base_bytes = base_bytes_early;
    bool touched = false;

    // Per-slot clamp-min, restricted to the 4 known fade-ratio slots
    // ([38], [48], [50], [61]). These are the only slots that drop
    // by exactly the scripted-fade 0.094× ratio; everything else is
    // either static across the fade or is per-scene lighting data
    // that varies by track (Canada noon writes different values here
    // than Munnar sunset). Touching only the 4 fade slots keeps
    // cross-track behaviour correct while still killing the blackout.
    if (const auto* snap = GetDcLightPinSnapshot()) {
        const float* sfloats = reinterpret_cast<const float*>(snap->data());
        float* tfloats = reinterpret_cast<float*>(base_bytes);
        const float boost = GetDcLightPinBoost();

        // 4 known fade-ratio slots — the scripted-blackout fix.
        constexpr size_t kFadeSlots[] = {38, 48, 50, 61};
        for (size_t slot : kFadeSlots) {
            const float target = sfloats[slot] * boost;
            if (std::abs(tfloats[slot]) < 0.5f * std::abs(target)) {
                tfloats[slot] = target;
                touched = true;
            }
        }

    } else {
        for (const auto& s : slots) {
            if (s.off + sizeof(float) > size) continue;
            auto* f = reinterpret_cast<float*>(base_bytes + s.off);
            const float prev = *f;
            // Only rewrite when the current value is meaningfully below
            // the recovered value — leaves already-bright state alone.
            if (std::abs(prev) < 0.5f * std::abs(s.value)) {
                *f = s.value;
                touched = true;
            }
        }
    }
    if (touched) {
        static std::mutex log_mutex;
        static u64 last_log_submit = 0;
        const u64 submit = g_driveclub_submit_index.load();
        std::lock_guard lock{log_mutex};
        if (submit - last_log_submit >= 60) {
            last_log_submit = submit;
            LOG_INFO(Render_Vulkan,
                     "[dc-lightpin] submit={} pipe={:#018x} addr={:#x} "
                     "pinned slots",
                     submit, pipe, base);
        }
    }
    return touched;
}

void MaybePinDriveclubExposure(VAddr base, u64 size, u32 cb_idx) {
    if (!IsDcExposurePinEnabled()) return;
    if (!IsDriveclubGuardEnabled()) return;
    if (g_driveclub_race_window.load() == 0) return;
    // Must be the tonemap compute pipeline and the ssbo_5 binding (index 4).
    if (g_driveclub_current_pipeline_hash.load() != 0x000002c995517e7full) return;
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Compute)) return;
    if (cb_idx != 4) return;
    // Byte offset of the exposure scalar inside ssbo_5: index 16 (dwords)
    // → byte 64.
    constexpr size_t kExposureByteOffset = 16 * sizeof(u32);
    if (base == 0 || size < kExposureByteOffset + sizeof(float)) return;
    const float pin_value = GetDcExposurePinValue();
    auto* target16 = reinterpret_cast<float*>(base + kExposureByteOffset);
    const float previous = *target16;
    // HARD PIN the known exposure scalar at dword offset 16.
    *target16 = pin_value;

    // Phase 24: cb4 has a second animated scalar at dword offset 8
    // that swings *opposite* to offset 16 (dim state: 0.63, lift
    // state: 0.26). Pin it to the lift-state value so the whole
    // adaptation pair is killed, not just half of it.
    constexpr size_t kOffset8Byte = 8 * sizeof(u32);
    if (size >= kOffset8Byte + sizeof(float)) {
        auto* target8 = reinterpret_cast<float*>(base + kOffset8Byte);
        *target8 = 0.2624f;
    }
    // Log the first time per session + roughly every 60 submits
    // thereafter so we can plot the exposure curve against race time.
    static std::mutex log_mutex;
    static u64 last_log_submit = 0;
    const u64 submit = g_driveclub_submit_index.load();
    {
        std::lock_guard lock{log_mutex};
        if (last_log_submit == 0 || submit - last_log_submit >= 60) {
            LOG_INFO(Render_Vulkan,
                     "[dc-exppin] submit={} addr={:#x} previous={} pinned_to={}",
                     submit, base + kExposureByteOffset, previous, pin_value);
            last_log_submit = submit;
        }
    }
}

bool MaybeSmashDriveclubUbo(VAddr base, u64 size, u32 cb_idx, bool is_written,
                            bool is_formatted) {
    if (!IsDcUboSmashEnabled()) return false;
    if (!IsDriveclubGuardEnabled()) return false;
    if (g_driveclub_race_window.load() == 0) return false;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    // Kind gate shared with texnuke.
    const u32 kind_bit = 1u << g_driveclub_current_draw_kind.load();
    if ((GetDcNukeKindMask() & kind_bit) == 0) return false;

    if (base == 0 || size == 0) return false;
    if (GetDcUboSmashOnlyRead() && (is_written || is_formatted)) return false;

    const auto& cbs = GetDcUboSmashCbs();
    if (!cbs.empty() && cbs.count(cb_idx) == 0) return false;

    const u64 min_sz = GetDcUboSmashMinSize();
    const u64 max_sz = GetDcUboSmashMaxSize();
    if (min_sz > 0 && size < min_sz) return false;
    if (max_sz > 0 && size > max_sz) return false;

    // Per-UBO deterministic tint via splitmix64 avalanche on base.
    u64 h = base;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    h = h ^ (h >> 31);
    const u32 pattern = static_cast<u32>(h >> 32);

    auto* dst = reinterpret_cast<u32*>(base);
    const size_t n = size / 4;
    for (size_t i = 0; i < n; ++i) dst[i] = pattern;

    static std::mutex seen_mutex;
    static std::unordered_set<u64> seen;
    const u64 key = (g_driveclub_current_pipeline_hash.load() << 8) ^
                    (static_cast<u64>(cb_idx) << 1) ^ base;
    bool first = false;
    {
        std::lock_guard lock{seen_mutex};
        first = seen.insert(key).second;
    }
    if (first) {
        LOG_INFO(Render_Vulkan,
                 "[dc-ubosmash] addr={:#x} size={} cb={} pipe={:#018x} arm={}",
                 base, size, cb_idx, g_driveclub_current_pipeline_hash.load(),
                 g_driveclub_arm_count.load());
    }
    return true;
}

// Driveclub compute dispatch log (Phase 19).
// Enable with env SHADPS4_DC_DISPATCHLOG=1.
//
// Logs one line per unique (submit, compute_pipeline_hash) tuple
// while the race-window gate is armed. Intended to map the compute
// dispatch landscape the same way the drawlog maps graphics draws,
// so we can tell which compute passes run during dim vs bright and
// where to point the UBO/texture smash.
bool IsDcDispatchLogEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_DISPATCHLOG");
        const bool on = env != nullptr && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan,
                     "[dc-dispatchlog] enabled (SHADPS4_DC_DISPATCHLOG=1)");
        }
        return on;
    }();
    return enabled;
}

// SHADPS4_DC_DISPATCH_SKIP=0xHH,0xII,... — when the current compute
// pipeline hash matches one of these, the dispatch is turned into a
// pure no-op (the cmdbuf.dispatch call is never emitted). Gated by
// the race-window guard and NUKE_AFTER_ARM so pre-race state that
// might share a pipeline hash with an in-race one stays intact.
//
// Intended use: Phase 19b identified eight compute pipelines that
// fire during dim and go silent the moment the blackout lifts. This
// env var lets us force those into silence from the start and see
// whether the blackout ever develops.
// Same pattern as DISPATCH_SKIP but for graphics draws. Comma-
// separated pipeline hashes whose Draw/DrawIndirect call becomes a
// no-op when the current pipeline matches. Gated by the race window
// + NUKE_AFTER_ARM.
const std::unordered_set<u64>& GetDcDrawSkipPipes() {
    static const std::unordered_set<u64> set = [] {
        std::unordered_set<u64> s;
        const char* env = std::getenv("SHADPS4_DC_DRAW_SKIP");
        if (!env || !env[0]) return s;
        std::string in = env;
        size_t pos = 0;
        while (pos < in.size()) {
            const size_t end = in.find(',', pos);
            std::string tok = in.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!tok.empty()) {
                const u64 v = std::strtoull(tok.c_str(), nullptr, 0);
                if (v) s.insert(v);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        if (!s.empty()) {
            LOG_INFO(Render_Vulkan,
                     "[dc-drawskip] {} pipeline(s) configured (SHADPS4_DC_DRAW_SKIP)",
                     s.size());
        }
        return s;
    }();
    return set;
}

bool ShouldSkipDriveclubDraw(u64 pipeline_hash) {
    const auto& skip = GetDcDrawSkipPipes();
    if (skip.empty()) return false;
    if (!IsDriveclubGuardEnabled()) return false;
    if (g_driveclub_race_window.load() == 0) return false;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    if (skip.count(pipeline_hash) == 0) return false;

    static std::mutex seen_mutex;
    static std::unordered_set<u64> seen;
    bool first = false;
    {
        std::lock_guard lock{seen_mutex};
        first = seen.insert(pipeline_hash).second;
    }
    if (first) {
        LOG_INFO(Render_Vulkan,
                 "[dc-drawskip] no-op draw pipeline={:#018x} first-seen submit={}",
                 pipeline_hash, g_driveclub_submit_index.load());
    }
    return true;
}

const std::unordered_set<u64>& GetDcDispatchSkipPipes() {
    static const std::unordered_set<u64> set = [] {
        std::unordered_set<u64> s;
        const char* env = std::getenv("SHADPS4_DC_DISPATCH_SKIP");
        if (!env || !env[0]) return s;
        std::string in = env;
        size_t pos = 0;
        while (pos < in.size()) {
            const size_t end = in.find(',', pos);
            std::string tok = in.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            if (!tok.empty()) {
                const u64 v = std::strtoull(tok.c_str(), nullptr, 0);
                if (v) s.insert(v);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        if (!s.empty()) {
            LOG_INFO(Render_Vulkan,
                     "[dc-dispatchskip] {} pipeline(s) configured (SHADPS4_DC_DISPATCH_SKIP)",
                     s.size());
        }
        return s;
    }();
    return set;
}

bool ShouldSkipDriveclubDispatch(u64 pipeline_hash) {
    // Auto-kill the histogram adaptation compute when exposure pin is
    // on. Saves the user from having to remember to also pass a
    // DISPATCH_SKIP list — both halves of the "kill auto-exposure"
    // story travel together.
    if (IsDcExposurePinEnabled() && IsDriveclubGuardEnabled() &&
        g_driveclub_race_window.load() != 0) {
        // 8 dim-only compute pipelines identified in Phase 19b; all go
        // silent when adaptation converges naturally, so force them
        // silent up front.
        switch (pipeline_hash) {
        case 0x0000089fc5730348ull: case 0x000008323ad915adull:
        case 0x00000eea2b7d89d4ull: case 0x000002da4ae7f686ull:
        case 0x000002f76d33c858ull: case 0x000006b667870fd4ull:
        case 0x000000a556b3c66bull: case 0x00000f56cce1d724ull:
            return true;
        default:
            break;
        }
    }
    const auto& skip = GetDcDispatchSkipPipes();
    if (skip.empty()) return false;
    if (!IsDriveclubGuardEnabled()) return false;
    if (g_driveclub_race_window.load() == 0) return false;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return false;
    }
    if (skip.count(pipeline_hash) == 0) return false;

    static std::mutex seen_mutex;
    static std::unordered_set<u64> seen;
    bool first = false;
    {
        std::lock_guard lock{seen_mutex};
        first = seen.insert(pipeline_hash).second;
    }
    if (first) {
        LOG_INFO(Render_Vulkan,
                 "[dc-dispatchskip] no-op dispatch pipeline={:#018x} first-seen submit={}",
                 pipeline_hash, g_driveclub_submit_index.load());
    }
    return true;
}

void NoteDriveclubDispatchlog(u64 pipeline_hash, u64 shader_pgm_hash,
                              u32 dim_x, u32 dim_y, u32 dim_z) {
    if (!IsDcDispatchLogEnabled()) return;
    if (g_driveclub_race_window.load() == 0) return;

    const u64 submit_index = g_driveclub_submit_index.load();
    static u64 dedup_submit = ~0ull;
    static std::unordered_set<u64> seen_in_submit;
    static std::mutex dedup_mutex;
    {
        std::lock_guard lock{dedup_mutex};
        if (submit_index != dedup_submit) {
            dedup_submit = submit_index;
            seen_in_submit.clear();
        }
        if (!seen_in_submit.insert(pipeline_hash).second) {
            return;
        }
    }
    LOG_INFO(Render_Vulkan,
             "[dc-dispatchlog] submit={} pipeline={:#018x} shader={:#018x} dim=({},{},{})",
             submit_index, pipeline_hash, shader_pgm_hash, dim_x, dim_y, dim_z);
}

// Driveclub frame-order dump (Phase 24).
//
// SHADPS4_DC_FRAMEORDER=N — record the exact submit-order chain of
// every Draw and every compute Dispatch for the first N gated
// submits (race window armed AND NUKE_AFTER_ARM threshold satisfied).
//
// Where [dc-drawlog] and [dc-dispatchlog] dedup per (submit, pipeline)
// tuple — which loses ordering and collapses repeated passes — this
// mode preserves ordering so we can walk from the last scene-material
// draw forward to the tonemap compute and enumerate every pass in
// between. That chain is where the dim is produced (fog/atmosphere,
// envmap/IBL update, pre-tonemap apply, etc.) per Phase 24 findings.
u32 GetDcFrameOrderCount() {
    static const u32 count = [] {
        const char* env = std::getenv("SHADPS4_DC_FRAMEORDER");
        if (!env || !env[0]) return 0u;
        const u32 n = static_cast<u32>(std::strtoul(env, nullptr, 0));
        if (n > 0) {
            LOG_INFO(Render_Vulkan,
                     "[dc-frameorder] enabled, will dump {} gated submit(s) "
                     "(SHADPS4_DC_FRAMEORDER)",
                     n);
        }
        return n;
    }();
    return count;
}

struct FrameOrderTicket {
    bool enabled;
    u64 submit_index;
    u64 seq;
};

FrameOrderTicket BeginFrameOrderEvent() {
    FrameOrderTicket t{false, 0, 0};
    const u32 max_submits = GetDcFrameOrderCount();
    if (max_submits == 0) return t;
    if (!IsDriveclubGuardEnabled()) return t;
    if (g_driveclub_race_window.load() == 0) return t;
    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return t;
    }

    static std::mutex state_mutex;
    static u64 recorded_submit = ~0ull;
    static u32 submits_emitted = 0;
    static u64 seq_in_submit = 0;
    static bool finished = false;

    const u64 submit_index = g_driveclub_submit_index.load();
    std::lock_guard lock{state_mutex};
    if (finished) return t;
    if (submit_index != recorded_submit) {
        if (submits_emitted >= max_submits) {
            finished = true;
            LOG_INFO(Render_Vulkan,
                     "[dc-frameorder] finished after {} submit(s); disabling",
                     submits_emitted);
            return t;
        }
        recorded_submit = submit_index;
        seq_in_submit = 0;
        submits_emitted += 1;
        LOG_INFO(Render_Vulkan,
                 "[dc-frameorder] submit={} boundary (emitted {}/{})",
                 submit_index, submits_emitted, max_submits);
    }
    t.enabled = true;
    t.submit_index = submit_index;
    t.seq = seq_in_submit++;
    return t;
}

void NoteDriveclubFrameOrderDispatch(u64 pipeline_hash, u64 shader_pgm_hash,
                                     u32 dim_x, u32 dim_y, u32 dim_z) {
    const auto t = BeginFrameOrderEvent();
    if (!t.enabled) return;
    LOG_INFO(Render_Vulkan,
             "[dc-frameorder] submit={} seq={:04} kind=disp "
             "pipe={:#018x} shader={:#018x} dim=({},{},{})",
             t.submit_index, t.seq, pipeline_hash, shader_pgm_hash,
             dim_x, dim_y, dim_z);
}

void NoteDriveclubFrameOrderDraw(const GraphicsPipeline* pipeline,
                                 const AmdGpu::Regs& regs) {
    const auto t = BeginFrameOrderEvent();
    if (!t.enabled) return;
    const u64 pipeline_hash = std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey());

    std::string rts;
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf) continue;
        if (!rts.empty()) rts += ",";
        rts += fmt::format("{:#x}", col_buf.Address());
    }
    if (rts.empty()) rts = "<none>";
    const bool has_depth = regs.depth_buffer.DepthValid();
    LOG_INFO(Render_Vulkan,
             "[dc-frameorder] submit={} seq={:04} kind=draw "
             "pipe={:#018x} rts={} depth={} idx={}",
             t.submit_index, t.seq, pipeline_hash, rts,
             has_depth ? "yes" : "no", regs.num_indices);
}

// Driveclub push-constant smasher (Phase 18).
//
// shadPS4's Shader::PushData packs:
//   [0..15]   viewport xoffset/yoffset/xscale/yscale (4 floats)
//   [16..79]  ud_regs[16]   (the GCN user-data registers — scalars
//                            the PS4 game passes per-draw)
//   [80..]    buf_offsets[] (alignment fix-ups, infrastructure)
//
// Viewport and buf_offsets are load-bearing; touching them crashes
// clip-space / buffer binding. The interesting attack surface is
// ud_regs — 16 × u32 scalars that carry per-draw material / fade /
// exposure constants, exactly the layer we could not isolate in
// Phase 17's UBO smashing because whole-UBO garbage either killed
// transforms or missed the field.
//
// Env toggles:
//   SHADPS4_DC_PC_SMASH=1                  master enable
//   SHADPS4_DC_PC_RANGE=<first>,<last>     inclusive ud_regs indices (default 0,15)
//   SHADPS4_DC_PC_VALUE=<hex>              replacement u32 (default per-draw hash)
//
// Reuses the shared gates SHADPS4_DC_NUKE_AFTER_ARM, SHADPS4_DC_NUKE_KIND
// and the race-window guard. Zero-cost when the master enable is off.
bool IsDcPushConstantSmashEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("SHADPS4_DC_PC_SMASH");
        const bool v = env && env[0] == '1' && env[1] == '\0';
        if (v) {
            LOG_INFO(Render_Vulkan, "[dc-pcsmash] enabled (SHADPS4_DC_PC_SMASH=1)");
        }
        return v;
    }();
    return on;
}

std::pair<u32, u32> GetDcPushConstantRange() {
    static const auto p = [] {
        const char* env = std::getenv("SHADPS4_DC_PC_RANGE");
        std::pair<u32, u32> def{0u, 15u};
        if (!env || !env[0]) return def;
        std::string s = env;
        const auto comma = s.find(',');
        if (comma == std::string::npos) return def;
        const u32 lo = static_cast<u32>(std::strtoul(s.substr(0, comma).c_str(), nullptr, 0));
        const u32 hi = static_cast<u32>(std::strtoul(s.substr(comma + 1).c_str(), nullptr, 0));
        LOG_INFO(Render_Vulkan, "[dc-pcsmash] range = [{}..{}]", lo, hi);
        return std::pair<u32, u32>{lo, std::min(hi, 15u)};
    }();
    return p;
}

// Optional fixed replacement value for ud_regs. If 0 (default), we use
// a per-draw splitmix64 hash of the pipeline hash so different draws
// get different garbage (same strategy as texnuke / ubosmash).
u32 GetDcPushConstantFixedValue() {
    static const u32 v = [] {
        const char* env = std::getenv("SHADPS4_DC_PC_VALUE");
        if (!env || !env[0]) return 0u;
        return static_cast<u32>(std::strtoul(env, nullptr, 0));
    }();
    return v;
}

void MaybeSmashDriveclubPushConstants(Shader::PushData& push_data) {
    if (!IsDcPushConstantSmashEnabled()) return;
    if (!IsDriveclubGuardEnabled()) return;
    if (g_driveclub_race_window.load() == 0) return;

    const u32 arm_threshold = GetDcNukeAfterArm();
    if (arm_threshold > 0 && g_driveclub_arm_count.load() < arm_threshold) {
        return;
    }
    const u32 kind_bit = 1u << g_driveclub_current_draw_kind.load();
    if ((GetDcNukeKindMask() & kind_bit) == 0) return;

    const auto [lo, hi] = GetDcPushConstantRange();
    const u32 fixed = GetDcPushConstantFixedValue();

    u32 replacement;
    if (fixed != 0) {
        replacement = fixed;
    } else {
        u64 h = g_driveclub_current_pipeline_hash.load();
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
        h = h ^ (h >> 31);
        replacement = static_cast<u32>(h);
    }

    const u32 last = std::min(hi, static_cast<u32>(push_data.ud_regs.size() - 1));
    for (u32 i = lo; i <= last; ++i) {
        push_data.ud_regs[i] = replacement;
    }

    static std::mutex seen_mutex;
    static std::unordered_set<u64> seen;
    const u64 key = g_driveclub_current_pipeline_hash.load();
    bool first = false;
    {
        std::lock_guard lock{seen_mutex};
        first = seen.insert(key).second;
    }
    if (first) {
        LOG_INFO(Render_Vulkan,
                 "[dc-pcsmash] pipeline={:#018x} range=[{}..{}] value={:#x} arm={}",
                 key, lo, last, replacement, g_driveclub_arm_count.load());
    }
}

// Phase 28: probe what the tonemap compute reads. When the tonemap
// compute (0x000002c995517e7f) has an image bound, log the guest
// address, format, size, and the first 8 floats of its guest memory.
// Gives a direct yes/no answer to "is HDR zero when tonemap reads it"
// without any image dump / deswizzle step.
//
// Enabled via SHADPS4_DC_PROBE_TONEMAP=1.
void MaybeProbeDriveclubTonemapInput(const VideoCore::Image& image) {
    static const bool enabled = [] {
        const char* env = std::getenv("SHADPS4_DC_PROBE_TONEMAP");
        const bool on = env && env[0] == '1' && env[1] == '\0';
        if (on) {
            LOG_INFO(Render_Vulkan,
                     "[dc-tonemap-probe] enabled (SHADPS4_DC_PROBE_TONEMAP=1)");
        }
        return on;
    }();
    if (!enabled) return;
    if (!IsDriveclubGuardEnabled()) return;
    if (g_driveclub_race_window.load() == 0) return;
    if (g_driveclub_current_pipeline_hash.load() != 0x000002c995517e7full) return;
    if (g_driveclub_current_draw_kind.load() !=
        static_cast<u8>(DriveclubDrawKind::Compute)) return;
    const auto& info = image.info;
    if (info.guest_address == 0) return;

    // Throttle: one log per (submit, addr) pair, and overall once
    // every 120 submits so we get a handful of samples across the race.
    static std::mutex m;
    static u64 last_submit_sampled = 0;
    static std::unordered_set<u64> seen_this_burst;
    const u64 submit = g_driveclub_submit_index.load();
    bool emit = false;
    {
        std::lock_guard l{m};
        if (submit - last_submit_sampled >= 120) {
            last_submit_sampled = submit;
            seen_this_burst.clear();
        }
        if (seen_this_burst.insert(info.guest_address).second) {
            emit = true;
        }
    }
    if (!emit) return;

    // Log image state flags. guest_address bytes are stale (shadPS4
    // doesn't sync render targets back to CPU unless explicitly asked),
    // so the answer we need is in `image.flags`:
    //   GpuModified = GPU wrote to this image  (upstream produced data)
    //   Dirty       = CPU wrote to it since    (upstream is stale)
    // If GpuModified=false for tonemap inputs, the upstream lighting
    // compute never produced output — bug is upstream.
    // If GpuModified=true, the data is in device-local Vulkan memory
    // (unobservable from here) — downstream consumes correct data but
    // produces wrong pixels.
    const bool gpu_mod = True(image.flags & VideoCore::ImageFlagBits::GpuModified);
    const bool dirty = True(image.flags & VideoCore::ImageFlagBits::Dirty);
    const bool maybe_cpu = True(image.flags & VideoCore::ImageFlagBits::MaybeCpuDirty);
    const auto* src = reinterpret_cast<const float*>(info.guest_address);
    float f[4] = {};
    std::memcpy(f, src, sizeof(f));
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S",
                  std::localtime(&tt));
    LOG_INFO(Render_Vulkan,
             "[dc-tonemap-probe] wc={}.{:03d} submit={} addr={:#x} "
             "size={}x{} fmt={} gpu_mod={} dirty={} cpu_dirty={}",
             time_buf, static_cast<int>(ms), submit, info.guest_address,
             info.size.width, info.size.height,
             vk::to_string(info.pixel_format), gpu_mod, dirty, maybe_cpu);
}

void MaybeDumpDriveclubTexture(const VideoCore::Image& image) {
    if (!IsDcTexDumpEnabled()) {
        return;
    }
    if (!IsDriveclubGuardEnabled()) {
        return;
    }
    if (g_driveclub_race_window.load() == 0) {
        return;
    }
    const auto& info = image.info;
    if (info.guest_address == 0 || info.guest_size == 0) {
        return;
    }
    // Sanity: skip absurd sizes and likely-render-target images (we
    // already know those and they're HDR floats not asset textures).
    if (info.guest_size > 64u * 1024u * 1024u) {
        return;
    }
    if (info.size.width < 64 || info.size.height < 64) {
        return;
    }

    // Dedup per guest-address across the whole session.
    static std::mutex seen_mutex;
    static std::unordered_set<u64> seen;
    {
        std::lock_guard lock{seen_mutex};
        if (!seen.insert(info.guest_address).second) {
            return;
        }
    }

    const u64 submit = g_driveclub_submit_index.load();
    const auto fmt_str = vk::to_string(info.pixel_format);
    // Vulkan format strings contain characters we want out of a path.
    std::string clean_fmt;
    clean_fmt.reserve(fmt_str.size());
    for (char c : fmt_str) {
        clean_fmt.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    const auto path = fmt::format("{}/s{:06}_a{:x}_{}x{}_{}_tm{}.bin",
                                   DcTexDumpDir(), submit, info.guest_address,
                                   info.size.width, info.size.height, clean_fmt,
                                   static_cast<u32>(info.tile_mode));

    const auto* src = reinterpret_cast<const u8*>(info.guest_address);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        LOG_WARNING(Render_Vulkan, "[dc-texdump] fopen failed: {}", path);
        return;
    }
    const size_t wrote = std::fwrite(src, 1, info.guest_size, f);
    std::fclose(f);

    LOG_INFO(Render_Vulkan,
             "[dc-texdump] submit={} pipeline={:#018x} addr={:#x} size={}x{} "
             "pitch={} bits={} fmt={} tile={} bytes={} -> {}",
             submit, g_driveclub_current_pipeline_hash.load(),
             info.guest_address, info.size.width, info.size.height,
             info.pitch, info.num_bits, fmt_str,
             static_cast<u32>(info.tile_mode), wrote, path);
}

bool IsCompressedBC(vk::Format fmt) {
    switch (fmt) {
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc2UnormBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc4UnormBlock:
    case vk::Format::eBc4SnormBlock:
    case vk::Format::eBc5UnormBlock:
    case vk::Format::eBc5SnormBlock:
    case vk::Format::eBc6HUfloatBlock:
    case vk::Format::eBc6HSfloatBlock:
    case vk::Format::eBc7UnormBlock:
    case vk::Format::eBc7SrgbBlock:
        return true;
    default:
        return false;
    }
}

bool CurrentDrawHitsTortureTarget(const AmdGpu::Regs& regs) {
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable) {
        return false;
    }
    for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf) {
            continue;
        }
        const VAddr addr = col_buf.Address();
        if (std::ranges::find(kTortureRenderTargets, addr) != kTortureRenderTargets.end()) {
            return true;
        }
    }
    return false;
}

void LogTortureSubOnce(VAddr dst_addr, VAddr src_addr, u32 w, u32 h, vk::Format fmt) {
    static std::mutex seen_mutex;
    static std::unordered_set<u64> seen;
    const u64 key = static_cast<u64>(src_addr) ^ (static_cast<u64>(w) << 44) ^
                    (static_cast<u64>(h) << 28) ^ static_cast<u64>(fmt);
    std::lock_guard lock{seen_mutex};
    if (seen.insert(key).second) {
        LOG_INFO(Render_Vulkan, "[dc-torture] sub dst={:#x} src={:#x} {}x{} fmt={}", dst_addr,
                 src_addr, w, h, vk::to_string(fmt));
    }
}

} // namespace

bool IsDriveclubRaceWindowActive() {
    return IsDriveclubGuardEnabled() && g_driveclub_race_window.load() > 0;
}

void NoteDriveclubVideoOutGamma(float gamma) {
    if (!IsDriveclubGuardEnabled()) {
        return;
    }
    if (gamma < 0.49f || gamma > 0.51f) {
        return;
    }
    const u64 submit_index = g_driveclub_submit_index.load();
    g_driveclub_gamma_hint_until_submit.store(submit_index + kDriveclubGammaHintWindowSubmits);
}

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool} {
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() = default;

void Rasterizer::CpSync() {
    scheduler.EndRendering();
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const auto& regs = liverpool->regs;
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    g_driveclub_current_pipeline_hash.store(
        std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey()));
    g_driveclub_current_draw_kind.store(static_cast<u8>(ClassifyDriveclubDraw(regs)));
    if (ShouldSkipDriveclubDraw(g_driveclub_current_pipeline_hash.load())) {
        return;
    }
    NoteDriveclubRaceGateCandidate(pipeline, regs);
    NoteDriveclubDrawlog(pipeline, regs);
    NoteDriveclubFrameOrderDraw(pipeline, regs);
    NoteDriveclubUboLog(pipeline, regs);
    MaybeClampDriveclubLuminanceUbo(pipeline, regs);
    MaybeRestoreDriveclubExposureUbo(pipeline, regs);
    MaybeNukeDriveclubExposureUbo(pipeline, regs);

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset);
    }

    MaybeSmashDriveclubPushConstants(push_data);
    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }

    ResetBindings();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    g_driveclub_current_pipeline_hash.store(
        std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey()));
    g_driveclub_current_draw_kind.store(
        static_cast<u8>(ClassifyDriveclubDraw(liverpool->regs)));
    if (ShouldSkipDriveclubDraw(g_driveclub_current_pipeline_hash.load())) {
        return;
    }
    NoteDriveclubRaceGateCandidate(pipeline, liverpool->regs);
    NoteDriveclubDrawlog(pipeline, liverpool->regs);
    NoteDriveclubFrameOrderDraw(pipeline, liverpool->regs);
    NoteDriveclubUboLog(pipeline, liverpool->regs);
    MaybeClampDriveclubLuminanceUbo(pipeline, liverpool->regs);
    MaybeRestoreDriveclubExposureUbo(pipeline, liverpool->regs);
    MaybeNukeDriveclubExposureUbo(pipeline, liverpool->regs);

    PrepareRenderState(pipeline);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);

    buffer_cache.BindVertexBuffers(*pipeline);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0);
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    MaybeSmashDriveclubPushConstants(push_data);
    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    UpdateDynamicState(pipeline, is_indexed);
    scheduler.BeginRendering(state);

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
    }

    ResetBindings();
}

void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    const u64 cs_hash =
        std::hash<ComputePipelineKey>{}(pipeline->GetComputeKey());
    g_driveclub_current_pipeline_hash.store(cs_hash);
    g_driveclub_current_draw_kind.store(static_cast<u8>(DriveclubDrawKind::Compute));
    NoteDriveclubDispatchlog(cs_hash,
                             pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash,
                             cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    NoteDriveclubFrameOrderDispatch(
        cs_hash, pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash,
        cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    if (ShouldSkipDriveclubDispatch(cs_hash)) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    scheduler.EndRendering();
    MaybeSmashDriveclubPushConstants(push_data);
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;

    scheduler.PopPendingOperations();

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    const u64 cs_hash =
        std::hash<ComputePipelineKey>{}(pipeline->GetComputeKey());
    g_driveclub_current_pipeline_hash.store(cs_hash);
    g_driveclub_current_draw_kind.store(static_cast<u8>(DriveclubDrawKind::Compute));
    NoteDriveclubDispatchlog(cs_hash,
                             pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash,
                             cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    NoteDriveclubFrameOrderDispatch(
        cs_hash, pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash,
        cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
    if (ShouldSkipDriveclubDispatch(cs_hash)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    scheduler.EndRendering();
    MaybeSmashDriveclubPushConstants(push_data);
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    cmdbuf.dispatchIndirect(buffer->Handle(), base);

    ResetBindings();
}

u64 Rasterizer::Flush() {
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    g_driveclub_submit_index.fetch_add(1);
    if (const u32 remaining = g_driveclub_race_window.load(); remaining > 0) {
        g_driveclub_race_window.store(remaining - 1);
    }
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    // Phase 28 candidate #4 — silent-skip instrumentation.
    // Log every compute dispatch that the heuristic detectors drop so
    // we can cross-reference with the Driveclub frame-order trace.
    // Enabled via SHADPS4_DC_LOG_SKIPS=1.
    const bool is_copy = IsComputeImageCopy(pipeline);
    const bool is_meta = is_copy ? false : IsComputeMetaClear(pipeline);
    const bool is_clear = (is_copy || is_meta) ? false : IsComputeImageClear(pipeline);
    if (is_copy || is_meta || is_clear) {
        static const bool log_skips = [] {
            const char* env = std::getenv("SHADPS4_DC_LOG_SKIPS");
            const bool on = env && env[0] == '1' && env[1] == '\0';
            if (on) LOG_INFO(Render_Vulkan,
                             "[dc-skip-log] enabled (SHADPS4_DC_LOG_SKIPS=1)");
            return on;
        }();
        if (log_skips && pipeline->IsCompute()) {
            const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
            const auto& cs_pgm = liverpool->GetCsRegs();
            static std::mutex m;
            static std::unordered_set<u64> seen;
            std::lock_guard l{m};
            if (seen.insert(cs.pgm_hash).second) {
                const char* reason = is_copy  ? "IsComputeImageCopy"
                                   : is_meta  ? "IsComputeMetaClear"
                                              : "IsComputeImageClear";
                LOG_INFO(Render_Vulkan,
                         "[dc-skip-log] pgm_hash={:#018x} dim=({},{},{}) "
                         "submit={} reason={}",
                         cs.pgm_hash, cs_pgm.dim_x, cs_pgm.dim_y,
                         cs_pgm.dim_z, g_driveclub_submit_index.load(), reason);
            }
        }
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();

    bool uses_dma = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        stage->PushUd(binding, push_data);
        BindBuffers(*stage, binding, push_data);
        BindTextures(*stage, binding);
        uses_dma |= stage->uses_dma;
    }

    if (uses_dma) {
        // We only use fault buffer for DMA right now.
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (auto& range : mapped_ranges) {
            buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
        }
        fault_process_pending = true;
    }

    return true;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    // vkCmdCopyImage requires EXACT VkFormat match (or size-compatible
    // formats under Maintenance8 — but even that doesn't permit e.g.
    // D32_SFLOAT_S8_UINT → R8G8B8A8_SRGB because the byte sizes differ).
    // The old heuristic `src.is_depth == dst.is_depth` let D32_SFLOAT
    // and D32_SFLOAT_S8_UINT through; Vulkan correctly rejects the copy
    // and the data silently never transfers (observed as Driveclub GI/
    // lighting corruption). Be strict: only fast-path when pixel
    // formats match exactly, otherwise always go through the buffer-
    // mediated copy which accepts any format pair.
    if (src_image.info.pixel_format == dst_image.info.pixel_format) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    buffer_bindings.clear();

    for (const auto& desc : stage.buffers) {
        const auto vsharp = desc.GetSharp(stage);
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, size);
            buffer_bindings.emplace_back(buffer_id, vsharp, size);
        } else {
            buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, 0);
        }
    }

    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const bool is_storage = desc.IsStorage(vsharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        // Buffer is not from the cache, either a special buffer or unbound.
        if (!buffer_id) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 ubo_size = stage.flattened_ud_buf.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(stage.flattened_ud_buf.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& cs_program = liverpool->GetCsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else if (instance.IsNullDescriptorSupported()) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
            }
        } else {
            MaybeDumpDriveclubTonemapSsbo(vsharp.base_address, size, i);
            MaybePinDriveclubExposure(vsharp.base_address, size, i);
            MaybeUboSnapshotDriveclub(vsharp.base_address, size, i);
            MaybeRecordDriveclubLighting(vsharp.base_address, size, i);
            if (MaybeCalibrateAtArmDriveclub(vsharp.base_address, size, i)) {
                buffer_cache.InvalidateMemory(vsharp.base_address, size);
            }
            if (MaybePinDriveclubLightFade(vsharp.base_address, size, i)) {
                buffer_cache.InvalidateMemory(vsharp.base_address, size);
            }
            if (MaybePinDriveclubSunLightUbo(vsharp.base_address, size, i)) {
                buffer_cache.InvalidateMemory(vsharp.base_address, size);
            }
            if (MaybePinDriveclubAmbient1008(vsharp.base_address, size, i)) {
                buffer_cache.InvalidateMemory(vsharp.base_address, size);
            }
            if (MaybeSmashDriveclubUbo(vsharp.base_address, size, i,
                                       desc.is_written, desc.is_formatted)) {
                buffer_cache.InvalidateMemory(vsharp.base_address, size);
            }
            // If exposure pin is active and this is the tonemap's ssbo_5,
            // invalidate the cache so our overwrite propagates to the
            // Vulkan upload that ObtainBuffer is about to perform.
            if (IsDcExposurePinEnabled() &&
                g_driveclub_current_pipeline_hash.load() == 0x000002c995517e7full &&
                g_driveclub_current_draw_kind.load() ==
                    static_cast<u8>(DriveclubDrawKind::Compute) &&
                i == 4) {
                buffer_cache.InvalidateMemory(vsharp.base_address, size);
            }
            const auto [vk_buffer, offset] = buffer_cache.ObtainBuffer(
                vsharp.base_address, size, desc.is_written, desc.is_formatted, buffer_id);
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
            }
            if (desc.is_written && desc.is_formatted) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageBuffer : vk::DescriptorType::eUniformBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    // For loading/storing to explicit mip levels, when no native instruction support, bind an array
    // of descriptors consecutively, 1 for each mip level. The shader can index this with LOD
    // operand.
    // This array holds the size of each consecutive array with the number of bindings consumed.
    // This is currently always 1 for anything other than mip fallback arrays.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;

    for (const auto& image_desc : stage.images) {
        const auto tsharp = image_desc.GetSharp(stage);
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        if (tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            auto* image = &texture_cache.GetImage(image_id);
            if (image->depth_id) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = image->depth_id;
                image = &texture_cache.GetImage(image_id);
            }
            MaybeDumpDriveclubTexture(*image);
            MaybeProbeDriveclubTonemapInput(*image);
            if (MaybeNukeDriveclubTexture(*image)) {
                // Page-fault tracking may not catch our own writes into
                // tracked pages from this same thread, so explicitly
                // invalidate the region. Next FindImage will re-upload
                // the nuked bytes from guest memory to the Vulkan image.
                texture_cache.InvalidateMemory(image->info.guest_address,
                                               image->info.guest_size);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    const bool torture_active =
        IsTortureEnabled() && CurrentDrawHitsTortureTarget(liverpool->regs);
    VAddr torture_dst_addr = 0;
    if (torture_active) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            const auto& col_buf = liverpool->regs.color_buffers[cb];
            if (col_buf && std::ranges::find(kTortureRenderTargets, col_buf.Address()) !=
                               kTortureRenderTargets.end()) {
                torture_dst_addr = col_buf.Address();
                break;
            }
        }
    }
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (torture_active && image_id && !is_storage) {
            const auto& probe_image = texture_cache.GetImage(image_id);
            if (std::ranges::find(kTortureSourceAddrs, probe_image.info.guest_address) !=
                kTortureSourceAddrs.end()) {
                LogTortureSubOnce(torture_dst_addr, probe_image.info.guest_address,
                                  probe_image.info.size.width, probe_image.info.size.height,
                                  probe_image.info.pixel_format);
                image_id = {};
            }
        }
        if (!image_id) {
            if (instance.IsNullDescriptorSupported()) {
                image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            } else {
                auto& null_image_view = texture_cache.FindTexture(VideoCore::NULL_IMAGE_ID, desc);
                image_infos.emplace_back(VK_NULL_HANDLE, *null_image_view.image_view,
                                         vk::ImageLayout::eGeneral);
            }
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);

            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((image.binding.force_general || image.binding.is_target) &&
                !image.info.props.is_depth) {
                image.Transit(instance.IsAttachmentFeedbackLoopLayoutSupported() &&
                                      image.binding.is_target
                                  ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                  : vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderRead |
                                  (image.info.props.is_depth
                                       ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                       : vk::AccessFlagBits2::eColorAttachmentWrite),
                              {});
            } else {
                if (is_storage) {
                    image.Transit(vk::ImageLayout::eGeneral,
                                  vk::AccessFlagBits2::eShaderRead |
                                      vk::AccessFlagBits2::eShaderWrite,
                                  desc.view_info.range);
                } else {
                    // Phase 28 candidate #2 instrumentation: log any image
                    // descriptor that goes READ_ONLY despite being
                    // declared as written. Gated by SHADPS4_DC_LOG_RO_WRITES=1.
                    static const bool log_ro_writes = [] {
                        const char* env = std::getenv("SHADPS4_DC_LOG_RO_WRITES");
                        return env && env[0] == '1' && env[1] == '\0';
                    }();
                    if (log_ro_writes) {
                        static std::mutex rom;
                        static std::unordered_set<u64> ro_seen;
                        std::lock_guard l{rom};
                        const u64 sig = std::hash<VAddr>{}(image.info.guest_address);
                        if (ro_seen.insert(sig).second) {
                            LOG_INFO(Render_Vulkan,
                                     "[dc-ro-writes] image at guest={:#x} "
                                     "size={}x{} format={} forced READ_ONLY "
                                     "(desc.type non-storage, no force_general)",
                                     image.info.guest_address,
                                     image.info.size.width,
                                     image.info.size.height,
                                     static_cast<u32>(image.info.pixel_format));
                        }
                    }
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead,
                                  desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear = regs.depth_render_control.depth_clear_enable ||
                                    texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        image.Transit(new_layout,
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                      desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 liverpool->regs.color_buffers[0].Address(),
                                 liverpool->regs.color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);
    return true;
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        scissors.push_back({
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        });
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        dynamic_state.SetStencilReferences(front.stencil_test_val, back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency ||
               topology == vk::PrimitiveTopology::ePatchList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
