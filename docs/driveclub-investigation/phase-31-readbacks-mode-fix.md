## Phase 31 — `readbacks_mode = Precise` closes the investigation

### Resolution

Setting `readbacks_mode: 2` (Precise) in
`~/.local/share/shadPS4/custom_configs/CUSA00003.json` makes DriveClub
v1.28 playable on shadPS4 for the first time:

- Canada night track: 2 min clean, race starts bright, no progressive
  dimming, no pitch-black window, no 1:30 recalibration pop.
- Munnar track: 1+ min clean, same outcome.

Neither the tonemap SPIR-V patches nor the UBO fade-pin machinery from
phases 22-29 are needed. The only change required is one integer in
the per-game config.

### What the setting does

`src/common/config.h:26`:

    enum GpuReadbacksMode { Disabled, Relaxed, Precise };

`src/video_core/buffer_cache/memory_tracker.h:68-89` — when the CPU
reads a guest memory range, `InvalidateRegion` is called. With
`Disabled`, the CPU just sees whatever was last written to guest
memory by the CPU (i.e. uninitialised / stale for anything the GPU
produced). With `Precise`, the emulator page-protects GPU-written
ranges, so the first CPU read after a GPU write faults, the emulator
issues a `vkCmdCopyBuffer` download + scheduler wait, and the CPU
sees fresh data.

`src/video_core/buffer_cache/region_manager.h:98,129` — Precise is
also the only mode that enables *read*-protection on GPU-written
pages; `Relaxed` does only write-protection. This is why `Relaxed`
would not have sufficed for DriveClub's feedback loop.

`src/video_core/buffer_cache/fault_manager.cpp:80-175` — per-frame
compute dispatch that drains the fault buffer and schedules the
downloads.

### Why DriveClub specifically needed it

DriveClub auto-exposure is a classic GPU→CPU feedback loop:

1. Scene renders to an HDR target.
2. A compute shader computes a luminance histogram into an SSBO.
3. The CPU reads that SSBO next frame and derives a target exposure.
4. The CPU writes the exposure back into the 1936-byte lighting UBO
   (slots `[38] [48] [50]` — what we were calling "fade" throughout
   phases 25-29).
5. Fragment shaders read that UBO and scale scene luminance by it.

With `readbacks_mode: Disabled`, step 3 reads stale zeros forever.
The CPU-side exposure integrator concludes "scene is pitch black,
open the aperture wide" and ramps its output value monotonically
upward. That is *exactly* the `fade[38]` trajectory we logged in
the stream-copy probe: `2.59 → 7.84 → 24 → 90 → 179 → 255` over
three minutes. The game wasn't broken; the feedback input was
dead, and the integrator drifted.

The `~90s pitch-black window` before recalibration is the point at
which the integrator saturates the lighting model so hard that
everything clips to zero. The "recalibration" we saw at 1:30 was
presumably a game-side failsafe timer that snaps the exposure back
to a default — not a real readback.

### Why we missed this for 30 phases

Listing the misdirections candidly so future investigations of
similar feedback-loop bugs can short-circuit faster:

1. **Symptom disguise.** Progressive darkening looks exactly like a
   broken tonemap / bad shader / miscompiled exposure scalar. Every
   dump-driven probe found values *in the UBO* that were wrong, which
   is true but describes the downstream read, not the upstream gap.
2. **`readback_linear_images` red herring.** We tested that flag in
   phase 29 and it had no effect, and assumed the readback surface
   was fully audited. That setting is for *linear image* readbacks
   (via `TextureCache::DownloadImageMemory`) — a separate code path
   from buffer readbacks. The histogram SSBO is a buffer, so the
   image-readback toggle did nothing.
3. **Pinning made things worse, not better.** Overwriting slots
   [38/48/50] from the outside looked briefly promising but could
   never converge, because the game kept re-writing on top of the
   pin from its own integrator. We treated that as "pin mechanism
   too late in the pipeline" and spent phases 27-29 moving the pin
   earlier, when the real fix was to stop clobbering the GPU-side
   input the integrator reads.
4. **The stream-copy probe in phase 30 was the decisive data.**
   Seeing `fade[38]` evolve smoothly, monotonically, from 2.59 all
   the way to 255 ruled out every "wrong-write / wrong-copy /
   corrupted-memory" candidate. The CPU was deliberately computing
   bad values. Once that was clear, the only remaining source of
   error was the *input* the CPU was computing from — which must be
   GPU output — which must be reaching the CPU via a readback path.
5. **Didn't check the readback buffer path.** The audit in phase 30
   listed seven writer candidates but all of them were about CPU- or
   GPU-side *write* correctness. None asked "what does the CPU
   read into that it expects the GPU to have filled?"

Reproducible takeaway: **for any "value drifts monotonically toward
a bad equilibrium" symptom, suspect a broken GPU→CPU feedback loop
first, before any downstream shader / UBO instrumentation.**

### Why `readbacks_mode` defaults to Disabled in upstream

Researched against the shadps4-emu upstream and the local tree.
It's deliberate — three axes: perf cost, driver stability, and
per-game regressions.

#### Per-frame cost of `Precise` (concrete, from the tree)

Turning `Precise` on activates a page-fault-driven GPU→CPU sync
path that is otherwise completely dormant. What actually runs:

- **Read-protect every GPU-written page.**
  `region_manager.h:98-100` and `:128-131` gate on
  `ReadbacksMode::Precise` before calling `UpdateProtection<enable,
  /*is_read=*/true>()`. This is what makes the guest address
  page-fault on CPU *read*, not just on write.
  `page_manager.cpp:201-208` (`impl->Protect`) issues a real
  `mprotect` on Linux, `VirtualProtect` on Windows, or
  `UFFDIO_WRITEPROTECT` on systems using the `userfaultfd` backend
  (`page_manager.cpp:125-134`) — per contiguous range, every time
  the GPU dirties a new page.
- **Signal-handler-driven GPU stall per read.**
  `GuestFaultSignalHandler` (`page_manager.cpp:210-218`) routes the
  fault into `Rasterizer::ReadMemory → BufferCache::ReadMemory`
  (`buffer_cache.cpp:84-89`), which `SendCommand`s onto the
  Liverpool thread *synchronously* — the CPU thread blocks until
  the GPU queue accepts.
- **`vkCmdCopyBuffer` Download + scheduler wait.**
  `DownloadBufferMemory` (`buffer_cache.cpp:92`) issues a staging
  copy from the GPU-local buffer back to the host-visible download
  staging, and the scheduler waits on it.
- **Per-frame fault-buffer compute dispatch.**
  `FaultManager::ProcessFaultBuffer` (`fault_manager.cpp:80-175`)
  runs a compute shader over the fault buffer
  (`cmdbuf.dispatch(DivCeil(num_pages/32, 64), 1, 1)`) plus two
  pipeline barriers and a `scheduler.Wait(...)` on the prior tick,
  then drains `fault_ranges` through `FindBuffer`.

With `Disabled`, *none* of that executes. `InvalidateRegion`
(`memory_tracker.h:68-89`) short-circuits the `on_flush` callback
entirely — it's guarded by `ReadbacksMode != Disabled`. The upload
side (CPU→GPU dirty tracking) is unaffected in either mode.

So the steady-state cost is: N mprotect syscalls per frame (one
per newly-GPU-dirtied page), one compute dispatch + fence wait
per frame, plus O(reads) signal-handler round-trips and buffer
copies. On Nvidia in a warm path this is tolerable; on AMD under
some drivers it tanks FPS into single digits.

#### Upstream timeline

- **PR #2668** (raphaelthegreat, unmerged) — original PoC, author
  explicit: *"implemented in relatively slowish way to test for
  correctness/data-validity. Before merging performance will be
  considered."*
- **PR #3178** (merged 2025-07-01) — rebased PoC landed **behind an
  opt-in flag**, no QT toggle. First time the feature reached main.
- **PR #3305** (merged 2025-07-24, tomboylover93) — added the QT
  toggle and parked it behind "Advanced". Author's exact phrasing:
  *"the current readbacks implementation is slow and will most
  likely be rewritten in the future… locked behind another
  'Advanced' section to avoid people enabling it and coming across
  issues in games that don't need it/don't work well with it as a
  result."*
- **PR #3404** (TheTurtle, unmerged) — the planned efficient
  rewrite: fence-detection heuristic + preemptive downloads. Still
  stalled as of 2026-04.
- **PR #3941** (merged 2026-01-21) — a targeted optimization that
  promoted `DmaData`-patched `DispatchDirect` to `DispatchIndirect`
  for a specific DriveClub-adjacent case (CUSA00093, eliminated
  ~12 flushes/frame). Reduces but does not remove the need for
  readbacks for that game.
- **PR #4085** (merged 2026-02-28) — introduced `Relaxed` mode
  specifically because `Precise` caused *"significant additional
  stuttering during asset loading"* in Bloodborne. `Relaxed`
  write-protects only, trading exactness for fewer stalls.
  Default stayed `Disabled`.
- **PR #4091** (merged 2026-03-01) — fixed ordering of the
  Disabled/Relaxed/Precise enum, also kept default at `Disabled`.
- **PR #4120** (Kyoskii, unmerged) — AMD-specific fix attempt:
  *"previously affected readbacks on AMD GPUs… AMD has a lot more
  strict timing compared to Nvidia… low/high readbacks do not
  crash on AMD GPU anymore, but… I get like 12 fps."*

#### Known regressions with readbacks enabled

- **Issue #3826** — Bloodborne hangs during loading screens,
  RTX 4090, open.
- **Issue #4215** — Bloodborne progressive FPS degradation over
  time with Relaxed, open.
- **Issue #3322** — DriveClub specifically fails on AMD RDNA 3
  with readbacks enabled; works on Nvidia.
- **Issue #3346** (closed by StevenMiller123) — already
  acknowledged upstream: *"DRIVECLUB™ currently requires readbacks
  enabled to function properly."*

#### Ruling in/out each possible reason

| Candidate | Verdict |
|---|---|
| Raw perf cost | **PRIMARY.** Per-page `mprotect` + per-fault GPU stall + `vkCmdCopyBuffer` + compute dispatch + scheduler wait. TheTurtle's PR #3404 body calls the current implementation "relatively slow"; #4085 and #4120 corroborate with concrete stutter / sub-12-fps reports. |
| Stability | **CONTRIBUTING.** #3826 (BB hangs), #3322 / #4120 (AMD driver timeouts), #4215 (progressive degradation). The fault-buffer + scheduler-wait plumbing is not race-free on all drivers. |
| Correctness regressions | **NOT SIGNIFICANT.** No PR/issue describes a game that renders *worse* with readbacks on — only slower or crashing. |
| Incomplete implementation | **YES.** #2668, #3178, #3305, #3404 all state that the current implementation is a stopgap. #3404's rewrite is the planned long-term fix and is not merged. |
| Platform split | **PARTIAL.** AMD drivers have a documented bad interaction (#3322, #4120). macOS/MoltenVK is not explicitly called out but isn't verified either. |
| Historical inertia | **NO.** This is a fresh, actively-debated feature. The adoption PR merged 2025-07, triage is still ongoing as of 2026-03. The default is *deliberately* opt-in pending correctness, perf, and AMD-stability work. |

Short answer: `Disabled` is the default because **the
implementation is known-slow, known-unstable on AMD, and known to
regress specific games (Bloodborne hangs) — maintainers explicitly
flagged it experimental behind the "Advanced" dialog**. DriveClub
is the poster child for needing it (Issue #3346 already admits it),
but no one had connected `Precise` mode specifically, or the
exposure-decay symptom, to the fix.

### Prior-art check

A prior-art sweep of shadPS4 issues, PRs, release notes, the
shadps4.net forums, NeoGAF, YouTube guides, and Reddit found no
document that recommends `readbacks_mode = Precise` (or numeric
value `2`) for DriveClub. All community guidance uses the
pre-v0.15.0 boolean `readbacks = true` or explicitly recommends
turning readbacks *off* for FPS. My own
`akitaonrails/distrobox-gaming/docs/driveclub-shadps4.md` had
`readbacks_mode: 0` — the opposite of the fix.

Verdict: **novel configuration**. The mode has existed in upstream
since v0.15.0 (2025-09), but nobody publicly connected Precise ↔
DriveClub ↔ exposure feedback loop.

### Config recipe

`~/.local/share/shadPS4/custom_configs/CUSA00003.json`:

    {
      "GPU": {
        "readback_linear_images_enabled": false,
        "readbacks_mode": 2,
        "copy_gpu_buffers": false,
        "direct_memory_access_enabled": false
      }
    }

`readbacks_mode: 2` is the only required change. The other three
keep their defaults.

### What happens to the rest of the investigation

- **Tonemap SPIR-V patches** (`phase-22plus-tonemap-compute-patches`)
  — no longer needed. The compute shader already produces correct
  output when fed a real histogram.
- **UBO pin machinery** (`phase-26-ubo-fade-pin`, `phase-29-calibrated-state`)
  — no longer needed. Slots `[38/48/50]` now settle on their own.
- **Fade-pin lifecycle state machine**, recording harness, calibrate-
  at-arm pin, Canada snapshot `.bin` files — all kept in-tree as
  diagnostic tools. They are not load-bearing for playability any
  more, but are useful for future investigations.
- **Phase-28 `IsComputeImageCopy` format-compat fix (commit 2b9525ab)**
  — still a real upstream-worthy bug fix, unrelated to readbacks.

### Upstream candidacy

Two separate contributions worth considering:

1. **Config recipe for DriveClub.** The shadps4-compatibility repo
   could get a per-game note: *"Set `readbacks_mode = Precise`.
   Without it, auto-exposure drifts to black after ~90s of any
   race."* This is pure documentation and low-risk.
2. **Default change** — probably *not* advisable given the
   Bloodborne/AMD regressions above. Do not push for this upstream.

### Closing state

This file closes the gamma/blackout investigation. Phases 01-30
exhaustively audited the game-side and renderer-side write paths
and produced a bug fix (#19 `IsComputeImageCopy` format-compat)
and a lot of diagnostic scaffolding. The actual playability fix is
a single integer in the per-game config. That asymmetry is worth
internalising: the next "looks like a shader bug" symptom starts
with a 5-minute readback-mode A/B before any instrumentation.
