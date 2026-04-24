## Phase C-continued — findings after per-game A/B

After the Phase C baseline matrix, we extended the testing to the
specific game-and-scenario combinations upstream flagged as
problematic: Bloodborne at menu, Bloodborne during gameplay, and
DriveClub in a real race. All cells single-run, all on the same
hardware (one vendor, one driver, one kernel). This is not an
N≥3 study, and the hardware is explicitly high-end.

### Hardware and software

- CPU: AMD Ryzen 9 7950X3D (16c/32t)
- GPU: RTX 5090
- RAM: 92 GB
- OS: Arch Linux (host), Arch Linux distrobox `gaming` container
- Driver: Nvidia proprietary (exact version from `pacman -Q` not
  recorded; latest at 2026-04-24)
- shadPS4 build: `readbacks-perf` branch at commit 454b4fe1
  (= upstream main `d1643d14` + harness instrumentation in `src/common/readback_metrics.{h,cpp}`)
- Config: per-game JSONs in
  `~/.local/share/shadPS4/custom_configs/{CUSA00003,CUSA00207}.json`,
  `readbacks_mode` cycled by `scripts/benchmark_readbacks.sh`

### Complete matrix

Raw CSVs under `data/`. Analysis via `scripts/analyze_readbacks.py`.

| game / workload         | mode     | frames | p50ms | p95ms | p99ms  | Finish/f | Finish_ms/f | Read/f | mprotect/f | inval/f |
|-------------------------|----------|--------|-------|-------|--------|----------|-------------|--------|------------|---------|
| DriveClub menu idle     | disabled | 3911   | 16.67 | 16.67 | 16.71  | 0.00     | 0.000       | 0.00   | 86         | 55      |
| DriveClub menu idle     | relaxed  | 3011   | 16.67 | 16.67 | 16.71  | 14.87    | 2.657       | 14.87  | 83         | 53      |
| DriveClub menu idle     | precise  | 3200   | 16.67 | 16.67 | 16.68  | 15.36    | 2.838       | 15.36  | 111        | 50      |
| DriveClub race (1 min)  | precise  | 3830   | 33.33 | 33.34 | 33.36  | 19.07    | 2.758       | 19.21  | 170        | 89      |
| Bloodborne menu idle    | precise  | 1559   | 33.33 | 33.34 | 33.35  | 0.92     | 0.174       | 0.92   | 4          | 1       |
| Bloodborne gameplay     | precise  | 6631   | 33.33 | 33.38 | 50.00  | 105.95   | 7.232       | 105.95 | 809        | 661     |
| Bloodborne gameplay     | disabled | 3843   | 33.33 | 33.35 | 50.00  | 0.00     | 0.000       | 0.00   | 462        | 439     |

### Findings in one page

1. **Upstream's "Precise regresses" doesn't reproduce on this
   hardware.** Across DriveClub and Bloodborne, no hang, no
   user-visible frame-pacing regression vs Disabled. p99 is
   equal or better under Precise.
2. **Precise's structural cost is workload-linear.** Each
   `BufferCache::ReadMemory` triggers one `Scheduler::Finish`;
   DriveClub race does ~19/frame (2.8ms), Bloodborne gameplay
   does ~106/frame (7.2ms). mprotect rate tracks the same
   curve. Fault-buffer compute is unused in both games.
3. **Relaxed does not earn its own mode in our data.** Clean
   menu-idle cell shows same Finish density as Precise with
   marginally fewer mprotects, and no user-visible advantage.
   The upstream rationale for introducing Relaxed (PR #4085's
   "Precise causes Bloodborne stutter during asset loading")
   does not reproduce here.
4. **The 50ms p99 in Bloodborne gameplay is not readback-related.**
   Same tail under Disabled and Precise. Almost certainly
   shader-compile / area-transition hitches, orthogonal to the
   readback path.
5. **Per-Finish wait is hardware-dependent.** 7.23ms cumulative /
   106 Finishes = ~68µs per Finish on a 5090. On slower GPUs the
   wait can balloon — the GPU is further behind the CPU request
   when the CPU asks. A 2-3× multiplier would push the Bloodborne
   gameplay cost from 7.2ms/f to 15-20ms/f, which would start
   pushing frames over the 33ms 30fps budget.
6. **This is why the default can't be flipped.** Not because
   Precise is broken, but because we cannot defend its behaviour
   on hardware we don't own. Issues #3322 (AMD RDNA 3) and
   #4120 (AMD ~12fps) are exactly this class of regression.

### The honest position

- We have **strong evidence Precise is viable as the recommended
  per-game mode on Nvidia consumer GPUs** for the games we tested.
- We have **no evidence against Relaxed**, only an absence of data
  supporting it. Our clean-cell data does not reproduce the Bloodborne
  asset-loading stutter that motivated PR #4085.
- We have **no evidence for or against Precise on AMD / Intel /
  Apple Silicon / Steam Deck-class hardware**. None of those are
  in our lab.

### What's durable regardless of default-flip question

1. **The measurement harness.** Upstream has nothing like it.
   `src/common/readback_metrics.{h,cpp}` + 6 instrumentation sites
   + `scripts/benchmark_readbacks.sh` + `analyze_readbacks.py`.
   Any future readback PR can be A/B'd against it locally, which
   is what every previous readbacks PR lacked. Upstream PRs #3404,
   #4085, #4120 each made claims like "+10-15 fps" or "12 fps on
   AMD" without a common framework — everyone was measuring
   differently. This fixes that.

2. **The concrete structural cost breakdown.** We now know the
   dominant cost is **N × Scheduler::Finish per frame**, not
   mprotect churn and not the fault-buffer compute. The per-call
   wait is hardware-dependent but the count is not. That lands
   Phase D squarely on the right target: batching the N calls
   into 1 so the per-Finish wait gets amortised N-fold, which
   kills the hardware dependency.

3. **Evidence that Relaxed may be redundant.** The clean-cell
   data shows it matches Precise in cost with no measured
   advantage. That's worth raising in a GitHub discussion even
   if we don't push for removing the mode.

### Files produced this phase

- `docs/readbacks-perf/00-plan.md` — original plan
- `docs/readbacks-perf/01-harness.md` — harness implementation notes
- `docs/readbacks-perf/02-baseline.md` — initial baseline matrix
- `docs/readbacks-perf/03-findings.md` — this file
- `docs/readbacks-perf/research/readbacks-research.md` — dossier
- `docs/readbacks-perf/data/*.csv` — archived raw CSVs
- `src/common/readback_metrics.{h,cpp}` — counter singleton
- `scripts/benchmark_readbacks.sh` — single-cell runner
- `scripts/analyze_readbacks.py` — CSV → markdown table

### Cells we never ran but could

- Bloodborne Relaxed gameplay — would settle "is Relaxed worse
  than Precise under Bloodborne asset load" one way or another
- Longer sessions (20+ min) under Precise — checks for
  accumulator-drift bugs (#4215 pattern)
- A non-PS4-exclusive game that does not exercise readbacks at
  all — to quantify the true "tax" floor
- Any of the above on a second GPU, but we don't have one
