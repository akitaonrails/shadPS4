## Readbacks perf investigation — plan

Branch: `readbacks-perf` (off `origin/main`).
Goal: make `readbacks_mode` cheap enough to be the default, or at
least to stop being a per-game hazard.

### Why this isn't solved

From the research dossier (`/tmp/readbacks-research.md`, also worth
committing here):

1. Dominant cost of `Precise` is `Scheduler::Finish()` per
   `BufferCache::ReadMemory` call — one `vkQueueSubmit` +
   timeline-semaphore wait per guest read of a GPU-dirty page. No
   batching across calls exists.
2. `Relaxed` removes read-protection mprotect churn but keeps the
   Finish. That is why Bloodborne still stutters during asset
   loading (#4215) under Relaxed.
3. The biggest unshipped optimization, `PR #3404` fence-detection
   prepass, was blocked on correctness in 3 games (Uncharted 3
   gray characters) rather than perf. Only slices of it landed:
   DispatchDirect→Indirect (#3941) and page-write-only mode
   (#4085).
4. AMD RDNA drivers regress under high Finish density every time
   the readback code is touched (#3322, #3346, #4120).
5. No measurement harness exists in-tree — no benchmark, no
   perf counters, no CSV export. Any claim of improvement is
   unfalsifiable.

### Proposed direction

Match what Dolphin did for EFB copies in 2018: **defer readback
downloads to synchronization boundaries** (PM4 `WaitRegMem` /
fence packets) instead of emitting one `Scheduler::Finish` per
page fault. This is the single most architectural win on the
table, and it is orthogonal to the fence-detection prepass that
sank #3404 — we don't need heuristics, the sync packets are
already in the guest command stream.

### Phased plan

**Phase A — measurement harness (no behaviour change).**
Build it first so every subsequent step is falsifiable.

- `src/common/readback_metrics.{h,cpp}`: atomic counters for
  Finish/Wait/Protect/Download calls + cumulative ns, plus a
  bounded frame-time ring for p50/p95/p99.
- CSV dump on process exit when
  `SHADPS4_READBACKS_METRICS=/path/to/out.csv` is set. Zero-cost
  when the env var is unset.
- Instrumentation sites:
  - `vk_scheduler.cpp` — `Finish`, `Wait`, `SubmitExecution`.
  - `buffer_cache.cpp` — `ReadMemory`, `DownloadBufferMemory`.
  - `texture_cache.cpp` — `DownloadImageMemory`,
    `ProcessDownloadImages`.
  - `fault_manager.cpp` — `ProcessFaultBuffer`.
  - `page_manager.cpp` — `Protect` (linux/win/uffd branches).
  - `vk_presenter.cpp` — frame-end marker.

**Phase B — reproducible benchmark workloads.**

Two workloads, both we already know:
1. `dc-canada-60s` — DriveClub Canada race start, controller-free.
   Readbacks required for correctness. Exercises feedback loop.
2. `dc-menu-30s` — DriveClub menu idle. Almost no readback
   traffic; sanity-check that instrumentation doesn't add cost.

Automated launch via `scripts/benchmark_readbacks.sh` with mode
knob (`SHADPS4_READBACKS_MODE=0|1|2`).

**Phase C — baseline matrix.**

Run Phase B against Phase A harness across all three modes, N≥3
runs each, dump CSVs. Commit the raw CSVs + a summary table.
Gate: if Relaxed vs Precise vs Disabled don't show clear
separation in Finish count, the harness is broken; fix before
moving on.

**Phase D — deferred readback batching.**

Design:
- New class `DeferredReadbackQueue` in buffer_cache.
- `BufferCache::ReadMemory` pushes `(addr, size, is_write)` into
  the queue instead of immediately calling `DownloadBufferMemory`.
- Flush the queue when the PM4 processor emits a sync packet
  (`WaitRegMem`, `EventWrite` fence, or end-of-IB) — these are
  already observed in `liverpool.cpp`.
- A single `Scheduler::Finish` drains the whole queue via one
  coalesced `vkCmdCopyBuffer` with the union of all ranges.
- Preserve ordering guarantees: if the guest reads and the queue
  already holds a pending download for that range, force-flush
  the queue *before* the read.

Opt-in via a new mode: `GpuReadbacksMode::Batched` — keeps
Precise/Relaxed intact for regression comparison.

**Phase E — A/B under harness.**

Same matrix as Phase C plus Batched. Required: ≥3 runs per cell,
computed p50/p95/p99 CPU frame time, scheduler.Finish count and
total time, download MB, page-protection syscalls/sec.

Acceptance: Batched ≥ 90% of Disabled throughput on `dc-menu-30s`
and ≥ 70% on `dc-canada-60s`, with no correctness regressions
(screenshot comparison at 60s).

**Phase F — upstream pitch (only if E passes).**

Two optional landings:
1. CUSA00003 per-game default of `readbacks_mode=Batched` in the
   compatibility repo.
2. PR to shadps4-emu adding Batched mode. Do NOT propose
   flipping the global default — that remains Disabled until
   AMD RDNA 2/3/4 validation happens externally.

### What we're deliberately not doing (and why)

- **Revisiting the #3404 fence-detection prepass.** Higher risk,
  blocked on correctness in 3 games. Batching is independent and
  clears the biggest structural cost. If Batched still isn't
  enough, fence-detection becomes a Phase G.
- **AMD-specific gating from #4120.** We have no AMD hardware to
  validate. Flag in the final PR as "needs AMD reviewer" and
  leave it alone in code.
- **Changing the global default.** Every PR that touched this
  hit AMD regressions. The right scope for our change is per-game
  Batched opt-in plus the mode knob.

### Files we expect to touch

- New: `src/common/readback_metrics.{h,cpp}` (Phase A).
- New: `docs/readbacks-perf/{01-harness.md,02-baseline.md,...}`.
- New: `scripts/benchmark_readbacks.sh` (Phase B).
- Modified (Phase A, small, pure add-only):
  - `src/video_core/renderer_vulkan/vk_scheduler.cpp`
  - `src/video_core/buffer_cache/buffer_cache.cpp`
  - `src/video_core/buffer_cache/fault_manager.cpp`
  - `src/video_core/texture_cache/texture_cache.cpp`
  - `src/video_core/page_manager.cpp`
  - `src/video_core/renderer_vulkan/vk_presenter.cpp`
  - `src/emulator.cpp` (CSV dump on shutdown)
- Modified (Phase D):
  - `src/video_core/buffer_cache/buffer_cache.{h,cpp}`
  - `src/video_core/amdgpu/liverpool.cpp` (sync-packet hook)
  - `src/common/config.{h,cpp}` (new mode enum value)

### What stops this plan

- AMD regressions discovered during Phase E — would force
  narrowing to Nvidia-only.
- Ordering-constraint holes in Phase D (e.g., guest reads a range
  the GPU hasn't finished writing yet because we deferred). The
  design requires per-range dirty tracking that matches the
  current flush semantics.
- Upstream maintainer pushback on adding a fourth mode — if they
  prefer to fold into Relaxed/Precise, rework per their
  direction.

### Tracking

One doc per phase under `docs/readbacks-perf/`:
- `00-plan.md` (this file)
- `01-harness.md` — harness implementation notes
- `02-baseline.md` — Phase C numbers
- `03-batched-design.md` — Phase D design
- `04-ab.md` — Phase E numbers
- `research/readbacks-research.md` — copy of the dossier
