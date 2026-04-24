# shadPS4 `readbacks_mode` Research Dossier

Branch inspected: `readbacks-perf` (= `origin/main`, tip `d1643d14`).
Repo: https://github.com/shadps4-emu/shadPS4
Compiled from GitHub PR/issue threads and the local source tree.

---

## A — Timeline

All dates in UTC. "merged/closed/open" reflects the state at time of writing (April 2026).

| # | Kind | Title | Author | Merged | Date | Base → effect |
|---|------|-------|--------|--------|------|---------------|
| [#2668](https://github.com/shadps4-emu/shadPS4/pull/2668) | PR | video_core: Readbacks proof of concept | raphaelthegreat | Yes (superseded by #3178) | 2025-06-30 | First PoC: mprotect-based read protection on GPU-modified pages; synchronous download on every page fault. |
| [#2819](https://github.com/shadps4-emu/shadPS4/pull/2819) | PR | video_core: Implement DMA | raphaelthegreat | Yes | ~2025-06 | Introduced fault-buffer compute path; this infrastructure is what `fault_manager.cpp` now owns. |
| [#3178](https://github.com/shadps4-emu/shadPS4/pull/3178) | PR | Readbacks proof of concept rebased | raphaelthegreat | Yes | 2025-07-01 | First opt-in landing. Added a single on/off config flag so the PoC could merge "without affecting performance for everyone." |
| [#3204](https://github.com/shadps4-emu/shadPS4/pull/3204) | PR | texture_cache: Async download of GPU modified linear images | raphaelthegreat | Yes | 2025-07-07 | Separate path for linear-tiled images (`readbackLinearImages`); copies image → host asynchronously one frame late, bypassing read protection to avoid false positives from image overlap. |
| [#3305](https://github.com/shadps4-emu/shadPS4/pull/3305) | PR | qt: Add toggle in the settings window for readbacks | tomboylover93 | Yes | 2025-07-24 | Placed the option in an "Experimental Features" group with warning: *"WARNING: Readbacks are currently slow and experimental. Only enable if explicitly instructed to do so."* |
| [#3322](https://github.com/shadps4-emu/shadPS4/issues/3322) | Issue | Driveclub works fine with Nvidia GPU but fails with AMD RDNA 3 | bulumbala | Closed (dup of #3315) | 2025-Q3 | First recorded AMD-only regression under readbacks. RX 7800 XT freezes at loading screen with `readbacks=true`. |
| [#3346](https://github.com/shadps4-emu/shadPS4/issues/3346) | Issue | CUSA00093 [DRIVECLUB] Restarting the driver or even freezing the system | DpaKc404 | Closed | 2025-Q3 | AMD RX 6650 XT: readbacks kill the driver / hang Linux. |
| [#3404](https://github.com/shadps4-emu/shadPS4/pull/3404) | PR | video_core: Readback optimizations | raphaelthegreat | **Open / stalled** | created 2025-08-08 | Fence-detection prepass over PM4 stream; converts self-modifying `DispatchDirect` into `DispatchIndirect`; preemptive async downloads of hot pages; adds `fenceDetection` knob (0/1). Never merged; gray characters in Uncharted 3 when fence detection enabled; "not final code." Partially split out later (see #3941, #4085). |
| [#3593](https://github.com/shadps4-emu/shadPS4/pull/3593) | PR | texture_cache: Make sure that readback images are downloaded in time | squidbus | Yes | 2025-09-13 | Replaces `scheduler.DeferOperation` for readback copies with a dedicated background thread: *"Use a thread for deferring copying readback images to memory, instead of a scheduler deferred operation, to make sure that the copy executes in time for the guest to see."* Fixes Ratchet & Clank hang; one tester reported 60→34 fps regression with patch applied. |
| [#3809](https://github.com/shadps4-emu/shadPS4/pull/3809) | PR | buffer_cache: Split DMA fault handling code from buffer cache | raphaelthegreat | Yes | 2025-11-18 | Extracts the Vulkan fault-buffer pipeline into `FaultManager`. Eliminates redundant `vkCmdFillBuffer`s and a `Scheduler::Wait` workaround. Uses `FindBuffer` instead of `CreateBuffer` for fault ranges. |
| [#3826](https://github.com/shadps4-emu/shadPS4/issues/3826) | Issue | Readbacks cause hangs in Bloodborne during loading screens | Missake212 | Open | 2025-Q4 | Repro: enable readbacks, teleport a few times, game hangs on next load. RTX 4090 / Windows + Linux. |
| [#3941](https://github.com/shadps4-emu/shadPS4/pull/3941) | PR | video_core: Small readback optimization | raphaelthegreat | Yes | 2026-01-21 | Detects `DmaData` copies into the current command list matching `DispatchDirect` dims and rewrites them into `DispatchIndirect`. Region is never marked GPU-modified → flush avoided. Lets DriveClub run *without* readbacks (was previously "device loss" on the self-modifying command stream). Initial logging cost 3 fps in Bloodborne; removed in final commit. |
| [#4085](https://github.com/shadps4-emu/shadPS4/pull/4085) | PR | Low readbacks mode | rainmakerv3 | Yes | 2026-02-28 | Introduces Relaxed mode, extracted from #3404. Author: *"Low is the same as high but fence detection is more relaxed and it only triggers on page writes, so no read protection. This is useful when guest doesn't want to access GPU data ever but readbacks are needed to fix data corruption (i.e. vertex explosions)."* Bloodborne testing: *"seems to work to prevent vertex explosions… far better performance, only additional issue is some significant additional stuttering during asset loading. Much more usable than the current expensive readbacks."* |
| [#4091](https://github.com/shadps4-emu/shadPS4/pull/4091) | PR | changed readbacks mode to Relaxed,Precised | georgemoralis | Yes | 2026-03-01 | Renames enum values so Relaxed < Precise in the TOML (legacy `readbacks = true` maps to Precise). Default stays `Disabled`. kalaposfos13: *"'precised' is not a real word."* |
| [#4120](https://github.com/shadps4-emu/shadPS4/pull/4120) | PR | video_core: opts amd low/high readback crash fixed hopefully | Kyoskii | Closed (not merged) | 2026-03-12 | Gates four optimizations (lockless BitArray pre-check, buffer HLE, DMA sync throttling, cache GC VRAM queries) when readbacks are active; fixes AMD races. Closed — author could only test on AMD iGPU at ~12 fps; no maintainer follow-up. |
| [#4215](https://github.com/shadps4-emu/shadPS4/issues/4215) | Issue | CUSA3173 Bloodborne progressive FPS degradation over time with Relax Readbacks when loading map related dcx assets | KrisCris | Open | 2026-Q1 | RTX 4080 Super / Win11. Stable 4K60 without readbacks; with Relaxed, drops 60→58 initially, 10–40 fps after ~1 hour. Correlates with #4085's "significant additional stuttering during asset loading." |

Abandoned / dormant approaches explicitly identified:
- **#3404** — fence-detection + preemptive downloads. Rejected on correctness grounds (Uncharted 3 buffer-size misreport, gray characters). Only the `DispatchDirect→Indirect` slice was rescued (#3941) and only the Relaxed-mode page-write-only variant was rescued (#4085).
- **#4120** — AMD-specific gating. Closed without merge; no alternative landed.

Config key on disk (`user/config.toml`, section `[GPU]`): `readbacksMode = 0|1|2` and `readbackLinearImages = true|false`. Default at load: `Disabled` / `false` (`src/common/config.cpp:189-190`, `src/common/config.cpp:1269-1270`). The "reset to defaults" path (`setDefaultValues`) hard-codes `Disabled`.

---

## B — Per-frame cost of Precise / Relaxed (local code audit)

All paths below are in `/mnt/data/Projects/shadPS4/src`.

### B.1 — Entry points that fire only when `readbacksMode != Disabled`

1. `video_core/buffer_cache/memory_tracker.h:77-88` — `InvalidateRegion` (called from `BufferCache::InvalidateMemory` on CPU write-faults, `buffer_cache.cpp:76-82`):
   - If `readbacksMode != Disabled` AND the region has `Type::GPU` bits set, triggers an `on_flush` callback that lands in `BufferCache::ReadMemory` → `DownloadBufferMemory<false>`.
   - If Disabled, the only action is `ChangeRegionState<Type::CPU,true>` (flip CPU-dirty bit).

2. `video_core/buffer_cache/region_manager.h:96-100` — `ChangeRegionState<Type::GPU, enable>`:
   - `if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise)` → `UpdatePageWatchers<enable, is_read=true>`. **Precise** is the only mode that installs *read* page-watchers. Relaxed writes the bitmap but never flips the `r` bit of the mapping.
   - Cost: one `mprotect` (or `UFFDIO_WRITEPROTECT`) batch per contiguous run of pages that transitions between "GPU has it" and "CPU may read."

3. `video_core/buffer_cache/region_manager.h:125-132` — `ForEachModifiedRange<Type::GPU, clear=true>`:
   - `if (readbacksMode != Disabled) UpdatePageWatchers<false, is_read=true>` — drops read-protection on pages whose GPU dirt was just downloaded. Fires from `ForEachDownloadRange` in `memory_tracker.h:117-124`.

### B.2 — The download hot path (`buffer_cache.cpp:84-145`)

`BufferCache::ReadMemory` bounces through `liverpool->SendCommand<true>` — that variant **blocks the calling CPU thread** until the Liverpool command processor has drained up to this point (see `video_core/amdgpu/liverpool.h:99`). Cost already *before* any GPU work: full guest-GPU thread round-trip.

`DownloadBufferMemory<async>`:
- Walks `memory_tracker->ForEachDownloadRange<false>` → one `RegionManager` lookup per 4 MB slab covered, plus per-page bit scan inside the bitset.
- Builds a `small_vector<vk::BufferCopy, 1>` from `gpu_modified_ranges.ForEachInRange` and *immediately* `Subtract`s those ranges (so a subsequent duplicate read will do nothing).
- `download_buffer.Map(total_size_bytes) + Commit()` — bumps a ring offset in a single 32 MB `MemoryUsage::Download` staging buffer (`buffer_cache.cpp:21 DownloadBufferSize = 32_MB`). If the ring overflows it triggers allocator growth.
- `scheduler.EndRendering()` then `cmdbuf.copyBuffer(...)` — **ends the active render pass** every time, even if the read is unrelated to the current draw. This is one of the hidden costs of Precise.
- When `async=false` (which is what `BufferCache::ReadMemory` always uses, `buffer_cache.cpp:87`), it follows with `scheduler.Finish()` — and `Finish()` at `vk_scheduler.cpp:104-110` does `SubmitExecution` then `Wait(presubmit_tick)`. That is a **full GPU stall** on whatever commands were in the current batch.
- Then `TryWriteBacking` does an mmap-level write into guest memory, followed by `UnmarkRegionAsGpuModified` + `MarkRegionAsCpuModified`, which *re-enters* `UpdatePageWatchers` twice more.

Per `ReadMemory` call, worst case:
- 1 cross-thread jump (CPU → Liverpool → CPU)
- 1 `EndRendering` (splits the current render pass)
- 1 `vkQueueSubmit` + 1 timeline-semaphore wait (the `scheduler.Finish()`)
- 0–N `mprotect` batches to un-read-protect touched pages (only if Precise)
- 1 `memcpy` into backing store

### B.3 — The fault-buffer compute pipeline (`fault_manager.cpp:80-175`)

This runs *every flush*, not only under readbacks. It exists so that GPU-side BDA loads that touch unmapped pages can call back into `BufferCache::FindBuffer` and re-map. Enabled unconditionally — relevant to readback sizing because `Precise`/`Relaxed` both enlarge the set of pages that are read-protected and thus the fault rate.

Per flush:
- `scheduler.Wait(fault_areas[current_area])` if the slot ring (size 8) is saturated — **GPU stall**.
- `scheduler.PopPendingOperations()` — drains deferred callbacks up to that tick.
- `memset(PageFaultAreaSize = 1024 * 8 = 8 KB, 0)` of the mapped download region.
- Two `vkCmdPipelineBarrier2` (pre + post) on `fault_buffer`.
- `cmdbuf.dispatch(ceil(caching_num_pages / 32 / 64), 1, 1)` — one workgroup per 64×32 = 2048 pages of address space tracked. With `CACHING_PAGEBITS=12` (4 KB) and `CACHING_NUMPAGES` covering the 40-bit PS4 VA map (≈256 M pages), that is ~2¹⁷ workgroups = **131,072 invocations** every flush. Each invocation walks one 32-bit word of the fault bitset (`host_shaders/fault_buffer_process.comp`).
- One `DeferOperation` that, once the slot's tick is free, reads back up to `MaxPageFaults=1024` fault records, inserts them into `RangeSet`, and calls `BufferCache::FindBuffer(start, size)` per range.

`fault_buffer_size = caching_num_pages / 8` — if `CACHING_NUMPAGES = 2^28`, that's 32 MB of device-local scratch dedicated to the fault bitmap; the ring of download areas takes `MaxPendingFaults * 1024 * 8 = 64 KB` of host-visible memory.

### B.4 — Page protection (`page_manager.cpp`)

Three backends (`src/video_core/page_manager.cpp`):
- **Windows**: `impl.Protect` → `AddressSpace::Protect` → `VirtualProtect` (`_WIN64` branch).
- **Linux without userfaultfd**: same interface → `mprotect` via `AddressSpace`.
- **Linux with `ENABLE_USERFAULTFD`**: `UFFDIO_WRITEPROTECT` ioctl on the uffd fd; a dedicated `ufd_thread` polls and routes to `rasterizer->InvalidateMemory(addr, 1)`.

`UpdatePageWatchers` (`page_manager.cpp:221-286`) uses a coalescing pass: it tracks the run of consecutive pages where the new permission matches and only emits a single `Protect()` for each contiguous run, skipping pages where nothing changed. This is the main existing batching mechanism — but it is **per-call**, not cross-call. A burst of `ChangeRegionState<GPU, true>` calls across many buffers will still issue one Protect per call.

`UpdatePageWatchersForRegion` (line 288) is a second variant used when the change is described by a 4 MB `RegionBits` mask, again batching within that mask.

### B.5 — Texture-side path (`texture_cache/texture_cache.cpp`)

Controlled by `readbackLinearImages` (orthogonal to `readbacksMode`):
- Line 30: constructor reads the setting into `readback_linear_images`.
- Line 645–647 (`FindTexture`, storage-type image): if set and image is linear with `guest_address != 0`, inserts image into `download_images` (a `std::unordered_set`).
- Line 656–658 (`FindRenderTarget`): same for linear render targets.
- Line 87–92: `ProcessDownloadImages` drains the set each frame.
- Line 94–131: `DownloadImageMemory` — `Map` on the download buffer, one `cmdbuf.copyImageToBuffer`, then `scheduler.DeferPriorityOperation` → `TryWriteBacking`. Async by design. The `DeferPriorityOperation` is cheaper than the buffer path's `scheduler.Finish()` because it rides the existing submit.

`image.info.pitch * height * layers * bytes_per_pixel` can be large (megabytes per linear RT per frame). There is no coalescing: every image id in the set gets its own `copyImageToBuffer`. There is no throttling: a game that keeps re-binding a linear RT will queue a download per bind-site per frame.

### B.6 — Existing caching / coalescing

- `RangeSet gpu_modified_ranges` (buffer-cache global) — already coalesces adjacent ranges so duplicate downloads of the same byte don't happen within one flush (`buffer_cache.cpp:112` `Subtract`).
- `download_images` is a `set` — dedupes image ids per frame.
- `UpdatePageWatchers` coalesces contiguous runs of pages per call.
- `FaultManager` dedupes fault pages through `RangeSet fault_ranges` before calling `FindBuffer`.
- No game-specific carve-outs in the active mainline code. The only game-aware logic is in `liverpool.cpp` DMA-patching (#3941) which avoids marking command-list targets as GPU-modified at all.

### B.7 — Rough per-frame accounting (Precise, worst case)

For a game that touches ~100 GPU-modified buffer regions per frame and has ~500 pages read-protected:
- `ReadMemory` callbacks: ≤100 → up to 100× `scheduler.Finish()`. This is the cliff.
- `mprotect` batches (release + re-protect): 100–500 syscalls after page coalescing.
- Fault-buffer compute dispatches: 1 per flush irrespective of mode, but the bitmap is larger under Precise so the compute does more work per invocation.
- Staging bandwidth: bounded by `DownloadBufferSize = 32 MB` before ring-wrap.

For Relaxed, the same code runs *except* the `UpdatePageWatchers<..., is_read=true>` calls are skipped (region_manager.h:98 short-circuits on `!= Precise`). That eliminates the mprotect churn but keeps every `scheduler.Finish()` — which is why Relaxed is still expensive and why #4215's progressive degradation is possible (the scheduler wait can accumulate pending operations over time).

---

## C — Failure modes and regressions

| Game / scenario | Mode | Symptom | Report | Blamed cause |
|---|---|---|---|---|
| DriveClub CUSA00093 | readbacks=true (legacy) | Loading-screen freeze on AMD RDNA 3 (RX 7800 XT); Nvidia GTX 1060 unaffected | [#3322](https://github.com/shadps4-emu/shadPS4/issues/3322) | AMD driver timing under repeated `scheduler.Finish()` after readbacks — dup'd to #3315. |
| DriveClub CUSA00093 | readbacks=true | Driver reset / system hang on RX 6650 XT (Win11 + Linux) | [#3346](https://github.com/shadps4-emu/shadPS4/issues/3346) | Same class as #3322; closed as resolved by #3941 (fixes the root cause at the guest-PM4 level, removing the need for readbacks for DriveClub). |
| DriveClub | without readbacks | Required readbacks for vertex rendering to even appear | raphaelthegreat in [#3941](https://github.com/shadps4-emu/shadPS4/pull/3941) | Self-modifying `DispatchDirect` command list. Fixed by rewriting to `DispatchIndirect`. |
| Bloodborne | Precise | Hangs during loading screens after a few teleports | [#3826](https://github.com/shadps4-emu/shadPS4/issues/3826) | Still open; blamed on instability of readbacks themselves rather than perf. |
| Bloodborne | Precise (original) | 36 fps → 16–22 fps | [#2668](https://github.com/shadps4-emu/shadPS4/pull/2668) comments | `scheduler.Finish()` on every page fault. |
| Bloodborne | Relaxed (#4085) | Vertex explosions fixed but "significant additional stuttering during asset loading" | PR body of [#4085](https://github.com/shadps4-emu/shadPS4/pull/4085) | Load-time clusters of flushes still hit `scheduler.Finish()`. |
| Bloodborne CUSA03173 | Relaxed | Progressive degradation: 60→58 initially, 10–40 after ~1h when loading `.flver.dcx` assets | [#4215](https://github.com/shadps4-emu/shadPS4/issues/4215) | Open. Likely the deferred-operation queue or staging-ring fragmentation accumulating over time; not yet root-caused. |
| Uncharted 3 / Nathan Drake Collection | Precise + fence detection (#3404) | Gray character models when fence-detect enabled | coolllman on [#3404](https://github.com/shadps4-emu/shadPS4/pull/3404) | Fence-detection prepass mis-identifies buffer size. |
| Uncharted 3 | Precise | Menu rendering broken | [#3178](https://github.com/shadps4-emu/shadPS4/pull/3178) | Resolved by enabling readbacks (that's the reason the feature exists). |
| The Last Guardian | readbackLinearImages | Water-physics softlock | [#3204](https://github.com/shadps4-emu/shadPS4/pull/3204) | Resolved by async image download. |
| Ratchet & Clank (2016) | Precise | Load-screen hang | [#3593](https://github.com/shadps4-emu/shadPS4/pull/3593) | `DeferOperation` not running in time; resolved by dedicated background copy thread. |
| LEGO games | Precise (PoC) | Vertex explosions fixed; 15-minute load screens | [#2668](https://github.com/shadps4-emu/shadPS4/pull/2668) | Full-synchronous download per fault. |
| Persona 5 Royal | Precise (PoC) | Increased crash frequency | [#2668](https://github.com/shadps4-emu/shadPS4/pull/2668) | Not diagnosed. |
| Marvel's Spider-Man | After #3941 | Freezes at logo (was previously crashing on SignalFence) | [#3941](https://github.com/shadps4-emu/shadPS4/pull/3941) | Regression in DispatchIndirect rewrite at a different call site. |
| Generic AMD (RDNA+igpu) | Low/High | Crashes / ~12 fps | [#4120](https://github.com/shadps4-emu/shadPS4/pull/4120) | Races between lockless BitArray pre-checks and page-fault handler; buffer HLE / DMA sync throttling not safe under readbacks. |

---

## D — Optimization attempts that did not land

### D.1 — PR #3404 (raphaelthegreat, Aug 2025, still open)

Two independent ideas bundled:

1. **Fence-detection prepass**: scan the PM4 stream heuristically for "signal" packets matched with `WaitRegMem` packets, use that to *defer* read-protecting GPU-modified pages until the actual fence boundary. Added `fenceDetection` setting (0=strictest, 1=with detection).
2. **Preemptive buffer downloads**: track pages that are flushed frequently and copy them to host asynchronously between flushes, so the synchronous `Finish()` has nothing to wait for.

Reported results: +10–15 fps in Bloodborne (bigol83); gains diminish at 4K because the download cost dominates (rafael-57); no regressions in tested titles (StevenMiller123). Correctness failure: gray character models in Uncharted 3 with detection on (coolllman), traced by the author to buffer-size misreporting in games that don't follow the heuristic. Author left it marked "still some cleanup to do, not final code" and never returned.

Reclaimable? Partially — the `DispatchDirect→DispatchIndirect` half was cherry-picked into #3941 and merged. The preemptive-download half was partially revived as Relaxed mode in #4085 (page-write-triggered only, no read protection), which landed. **The fence-detection prepass itself has never landed** and is the biggest known win left on the table — blocked on correctness in games that don't follow the signal/wait idiom.

### D.2 — PR #4120 (Kyoskii, Mar 2026, closed)

Proposed: gate four emulator-side optimizations when readbacks are active:
- Disable lockless `BitArray` pre-check in buffer cache (race vs. page-fault handler).
- Skip buffer HLE.
- Skip DMA sync throttling.
- Use direct VRAM queries in cache GC (not the cached value).

Also fixed an `infos[]` aliasing bug in pipeline caching discovered in passing.

Why it didn't land: Kyoskii could only validate on an AMD iGPU at ~12 fps. No maintainer review committed to the approach; the PR closed without discussion. No alternative landed.

Reclaimable? The aliasing fix almost certainly yes (independent bug). The four gating changes are worth revisiting but need a better testbed than iGPU@12fps, and likely need to be reframed: instead of "disable when readbacks are on" the change is "make these optimizations lock-safe."

### D.3 — Ideas that have *not* been proposed in any PR or issue I can find

The thread searches (`is:pr readback`, `is:issue readback`) produce no results for:
- **Batch-coalescing across `ReadMemory` calls.** Each call currently does its own `scheduler.Finish()`. Nothing batches them at frame granularity. Not proposed.
- **Adaptive mode switching.** No PR has attempted to flip between Relaxed and Precise based on runtime signals.
- **Per-page quiescence tracking / aging.** Pages never "cool down" — a page that is read-protected stays read-protected until a write fault.
- **Selective protection based on buffer usage class.** All GPU-modified pages are treated identically; UBO-sized regions pay the same cost as large UAVs.

These are all open ground.

---

## E — What comparable emulators do

### yuzu (Switch)

Moved the Query Cache to asynchronous downloads at Normal-accuracy, skipping host-guest fence sync and conditional-rendering downloads. Claims 50–87% perf improvement. Also moved swapchain presentation to a separate thread to stop stalling the GPU thread. Source: [Progress Report May 2023](https://yuzu-mirror.github.io/entry/yuzu-progress-report-may-2023/), [Progress Report Feb 2023](https://yuzu-mirror.github.io/entry/yuzu-progress-report-feb-2023/).

### Ryujinx (Switch)

Per-handle data flushing (not per-view); flushes synchronised to syncpoint increments rather than draw boundaries. For linear textures, pre-emptive flush to CPU-accessible memory skips waits the GPU would otherwise do while data is copied; linear layout can even be served by direct memory import. Backend thread explicitly forbidden from triggering readbacks that wait on GPU (self-wait hazard). Source: [Ryujinx progress reports](https://blog.ryujinx.org/progress-report-january-2022/).

### Dolphin (GC/Wii)

**Deferred EFB Copies** (#7539 by stenzek): queue EFB→RAM copies until a DrawDone/token packet, then flush them in order. Before: GPU-idle per copy. After: ~+12% in demanding scenes. Also has **Defer EFB Cache Invalidation** for a further perf-vs-stability tradeoff. Source: [dolphin-emu/dolphin#7539](https://github.com/dolphin-emu/dolphin/pull/7539), [Dolphin performance guide](https://dolphin-emu.org/docs/guides/performance-guide/).

### PCSX2 (PS2)

Readbacks flagged in-docs as the worst GPU-perf cost. Only mitigation shipped is "Spin GPU During Readbacks" which actively burns GPU cycles to keep the pipeline hot across the stall (avoids driver sleep states). Source: [PCSX2 performance troubleshooting](https://pcsx2.net/docs/troubleshooting/performance/), [PCSX2 #7485](https://github.com/PCSX2/pcsx2/issues/7485).

### PPSSPP (PSP)

"Virtual GPU readbacks": when the guest writes into a target address expecting a framebuffer there, synthesize a virtual framebuffer at that address and satisfy subsequent sampling via GPU-side copy rather than downloading to CPU. Source: [hrydgard/ppsspp#11528](https://github.com/hrydgard/ppsspp/issues/11528), [#16900](https://github.com/hrydgard/ppsspp/issues/16900).

### Three techniques shadPS4 does not have

1. **Deferred / batched readbacks until a synchronization packet** (Dolphin EFB approach). Mechanism: accumulate `ReadMemory` callbacks into a queue within a fault batch; flush once per fault batch or once per `WaitRegMem` boundary instead of per call. Link: https://github.com/dolphin-emu/dolphin/pull/7539
2. **Per-handle (not per-range) flush with syncpoint alignment** (Ryujinx). Mechanism: associate each buffer with a single flush handle, coalesce every touch inside a frame, emit the download at the next sync-boundary the guest already waits on. Link: https://blog.ryujinx.org/progress-report-january-2022/
3. **Virtual readbacks** (PPSSPP). Mechanism: detect guest-side reads that are actually "copy this framebuffer to that address so I can render it as a texture" and satisfy them with a GPU-side image-to-buffer copy into a virtual FB instead of a CPU download. Link: https://github.com/hrydgard/ppsspp/issues/11528

---

## F — Measurement baseline

### What exists upstream

None of the standard tools below are present for this feature:
- No benchmark / perf harness under `tests/`. `shadps4_settings_test` and `shadps4_gcn_test` are the only tests and neither touches readbacks.
- No scripted A/B mode in the CLI. The binary runs an eboot; throughput is observed via the F10 FPS overlay.
- No Tracy zones specifically tagged for readback paths. The buffer cache uses `RENDERER_TRACE` macros (see `region_manager.h:81, 113, 147, 174`) which feed Tracy when `TRACY_GPU_ENABLED=1` — so you can see *counts* of `UpdateProtection`, `ForEachModifiedRange`, `UpdatePageWatchers` under Tracy, but there are no named, aggregated zones for "time spent in readbacks."
- `scheduler.Wait` and `scheduler.Finish` have no dedicated counter. `master_semaphore` has timeline semaphore state but no logged histogram.
- RenderDoc integration exists (`src/video_core/renderdoc.*`) — can capture a single frame but is not a throughput tool.
- `dumpShaders=true`, `rdocEnable=true`, `validation=true` (from CLAUDE.md) — none are perf hooks.

### What an A/B harness should measure

Minimum set to compare "any rewrite" vs current Precise:

| Metric | How to collect |
|---|---|
| CPU frametime p50/p95/p99 | Tracy frame marker, or a 60s-rolling window accumulator around `vk_presenter.cpp` present path. |
| GPU frametime p50/p95/p99 | Tracy GPU zones (already present as "Guest Frame" at `vk_scheduler.cpp:143`). |
| `scheduler.Finish()` calls/sec and total time in Wait | Instrument `vk_scheduler.cpp:104-119` with a counter + histogram. |
| Page-protection syscalls/sec | Wrap `page_manager.cpp:125-134` (uffd) and `:201-208` (mprotect) — count + total time. |
| Fault-handler wakeups/sec | Counter around `page_manager.cpp:210-218` `GuestFaultSignalHandler` and `:136-179` `UffdHandler`. |
| Download staging MB/s | Integrate `total_size_bytes` in `DownloadBufferMemory` over time; also `download_size` in `DownloadImageMemory`. |
| Fault-buffer compute dispatch count/frame | Counter in `FaultManager::ProcessFaultBuffer` (already naturally per-flush). |
| `download_images.size()` per frame | Counter at top of `ProcessDownloadImages` (`texture_cache.cpp:87`). |
| `gpu_modified_ranges` size / coalescing ratio | Exposed via `RangeSet` in `buffer_cache/range_set.h`. |

A reproducible rig would pin a known workload (e.g., DriveClub menu at a specific CUSA checkpoint, or Bloodborne Central Yharnam teleport loop from #3826 repro) and run N minutes in each mode with the above counters dumped as CSV. No such rig exists in-tree; it would have to be built alongside any rewrite.

---

## Cross-reference of relevant source paths

- `src/common/config.h:26-30` — `enum GpuReadbacksMode { Disabled, Relaxed, Precise }`
- `src/common/config.cpp:189` — default `GpuReadbacksMode::Disabled`
- `src/common/config.cpp:990` / `:1173` — TOML key `[GPU] readbacksMode`
- `src/core/emulator_settings.h:32-36` — mirror of enum; `:356` default `Disabled`; `:638` forwarder
- `src/video_core/buffer_cache/memory_tracker.h:68-124` — `InvalidateRegion`, `ForEachUploadRange`, `ForEachDownloadRange`
- `src/video_core/buffer_cache/region_manager.h:79-136` — `ChangeRegionState`, `ForEachModifiedRange`, `UpdateProtection`
- `src/video_core/buffer_cache/fault_manager.{h,cpp}` — the compute fault-buffer pipeline
- `src/video_core/buffer_cache/buffer_cache.cpp:76-145` — `InvalidateMemory`, `ReadMemory`, `DownloadBufferMemory`
- `src/video_core/page_manager.cpp:125-134, :201-208, :221-356` — `Protect`, `UpdatePageWatchers(+ForRegion)`
- `src/video_core/texture_cache/texture_cache.cpp:87-131` — `ProcessDownloadImages`, `DownloadImageMemory`
- `src/video_core/texture_cache/texture_cache.cpp:645-658` — `readback_linear_images` decision points
- `src/video_core/renderer_vulkan/vk_scheduler.cpp:104-119` — `Finish`, `Wait`
- `src/video_core/host_shaders/fault_buffer_process.comp` — the GPU-side page-fault-bitset parser
