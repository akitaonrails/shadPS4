## Phase C — Baseline matrix

Workload: **menu-idle, 30s wall-clock**, DriveClub CUSA00003 v1.28.
Game is VSync-locked, boot-state varies per run.
Three cells, single run each (not N≥3 yet; see *Known non-determinism*).

### Results

Raw CSVs under `data/menu-30s-{disabled,relaxed,precise}.csv`.

| mode      | frames | p50ms | p95ms | p99ms    | Finish/f | Finish_ms/f | Read/f | mprotect/f | inval/f |
|-----------|--------|-------|-------|----------|----------|-------------|--------|------------|---------|
| disabled  | 1975   | 16.67 | 33.34 | **50.00** | 0.00     | 0.000       | 0.00   | 103        | 67      |
| relaxed   | 1793   | 16.67 | 33.34 | **66.67** | 14.03    | 2.594       | 14.04  | 102        | 67      |
| precise   | 4983   | 16.67 | 33.34 | **33.35** | 15.46    | 2.686       | 15.48  | 153        | 83      |

### Findings

1. **Relaxed has the worst p99 (66.7ms).**
   Worse than both Disabled and Precise. This is consistent with
   Bloodborne [#4215](https://github.com/shadps4-emu/shadPS4/issues/4215)'s
   progressive degradation report. Async-flush without read
   protection lets stalls pile up in bursts rather than being
   amortised evenly.

2. **Precise has the cleanest p99 (33.4ms)** — equal to p95.
   Synchronous reads make per-frame cost predictable; variance
   goes into the p50 instead of the tail.

3. **Finish-per-frame density is the dominant cost, not
   mprotect.** Relaxed vs Precise: 14.0 vs 15.5 Finish/f, 2.59
   vs 2.69 ms/f. Removing read-protection (Relaxed → Precise
   difference) saves only ~30% of mprotect syscalls but nearly
   zero ms/frame on Finish. Confirms the dossier thesis:
   deferring / batching Finish is the real win.

4. **Disabled still pays 103 mprotect/f** — that is the
   upload-side CPU-write-tracking that every mode does. The
   readback-specific mprotect delta is only ~50/f on top of that
   under Precise.

5. **Zero image downloads across all modes.** DriveClub does not
   use linear image readback. The `readbackLinearImages` setting
   is orthogonal and inert for this game. Same for
   `fault_dispatch_count` — this game doesn't touch unmapped BDA
   pages on the GPU side.

### Known non-determinism

- Frame counts vary 1.8k → 5k across modes despite identical
  30s wall-clock. Menu boot state depends on prior shader cache
  warmth, presenter race at window creation, and per-run
  pipeline variance. The workload needs a deterministic entry
  point (e.g. cold start + first N presents after first
  user-input window).
- p99 reproducibility is untested — single run per cell. Phase E
  requires N≥3 with the deterministic workload. Before Phase E,
  update `benchmark_readbacks.sh` to:
  - Clear shader/pipeline cache between cells.
  - Stop the emulator via `distrobox-enter gaming -- pkill` so
    the SIGTERM lands in the container PID namespace (the host
    `pgrep` approach occasionally leaked wrapper processes).
  - Verify the process is actually dead before moving on
    (loop `pgrep` inside container until empty).

### Target for Phase D

Under Precise the CPU spends **2.69ms/frame in Scheduler::Finish
just from ReadMemory calls**, across ~15 calls/frame. Dolphin's
EFB-copy pattern applied here collapses that to ≤2 Finish/frame.

Conservative target: **Batched mode should show Finish/f ≤ 2,
Finish_ms/f ≤ 0.5**, with p99 ≤ Precise's 33.4ms. mprotect/f is
a secondary target: if the batched flush handles all pending
read-protect updates in one coalesced pass, 153 → ~50/f is
plausible.

Acceptance gate for Phase E:
- Correctness: screenshot at 60s on Canada race matches Precise.
- Throughput: frame_p50 ≤ 1.1× Disabled's 16.67ms.
- Tail: frame_p99 ≤ 1.2× Precise's 33.35ms.
