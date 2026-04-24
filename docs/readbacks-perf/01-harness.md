## Phase A — Measurement harness

Built and smoke-tested. Notes for anyone reading the numbers.

### What it captures

`SHADPS4_READBACKS_METRICS=/path/to/out.csv` at startup turns on
wall-clock timing. Dump is fired at three places:

- `Emulator::Run`'s explicit `Dump()` right before `std::quick_exit`
  — the natural close-window path.
- `std::atexit` hook registered at `Init` — covers normal main
  return.
- `std::at_quick_exit` hook — covers quick_exit calls from other
  sites.
- `SIGTERM` / `SIGINT` chained — dumps then re-raises default. Used
  when benchmark scripts kill the process.

CSV columns — one row per counter, plus frame percentiles at the
end. Schema is stable within a phase; any addition is appended.

| counter | meaning |
|---|---|
| `finish_count` / `finish_ns` | `Scheduler::Finish` invocations + cumulative ns |
| `wait_count` / `wait_ns` | `Scheduler::Wait` invocations + cumulative ns (includes non-readback sync) |
| `submit_count` | `Scheduler::SubmitExecution` invocations |
| `buffer_read_count` / `buffer_read_ns` | `BufferCache::ReadMemory` calls + total time |
| `buffer_download_count` / `buffer_download_bytes` / `buffer_download_ns` | `DownloadBufferMemory` calls, cumulative bytes, total time |
| `image_download_count` / `_bytes` / `_ns` | `TextureCache::DownloadImageMemory` path |
| `image_download_batches` / `_batched` | `ProcessDownloadImages` batch count + total images across batches |
| `fault_dispatch_count` | `FaultManager::ProcessFaultBuffer` dispatches |
| `page_protect_count` / `page_protect_ns` | `PageManager::Protect` (uffd + mprotect fallback) |
| `page_invalidate_count` | `BufferCache::InvalidateMemory` calls |
| `frame_count` / `frame_ring_count` | total guest frames + sample count in the bounded ring (16k) |
| `frame_p50_ns` / `_p95_ns` / `_p99_ns` | CPU inter-present-time percentiles from the ring |

### Cost of the harness itself

- Atomic increments on disabled paths: ~5ns each. Always live.
- Wall-clock capture on enabled paths: ~30-50ns per
  `steady_clock::now` pair. Only fires when `IsEnabled()` is true.
- CSV write on exit: one-shot, bounded (<1 KB).

Frame-ring sampling uses a bounded 16k slot ring — enough for ~4
minutes at 60fps or 8 minutes at 30fps of percentile data.

### First smoke test

DriveClub CUSA00003 v1.28, nightly code + `readbacks_mode=2`
(Precise), ~50s idle in menu, ~2911 presents. Raw CSV at
`docs/readbacks-perf/data/00-smoke-precise-menu.csv`.

Headline numbers (per-frame averages):

| metric | total | /frame |
|---|---|---|
| presents | 2911 | — |
| `frame_p50_ns` | 33.33ms | `→ 30fps locked` |
| `Scheduler::Finish` | 39,948 | **14** |
| `Scheduler::Finish` time | 7.95s | **2.73ms (8% of 33ms budget)** |
| `BufferCache::ReadMemory` | 40,937 | 14 (1:1 with Finish) |
| `buffer_download_bytes` | 43.2 MB | 15 KB |
| `page_protect` syscalls | 380,884 | 131 |
| `page_invalidate` calls | 206,273 | 71 |
| `fault_dispatch_count` | 0 | — |

### Interpretation

1. **The thesis holds.** Every `ReadMemory` triggers a `Finish`
   (39,948 vs 40,937 — the ~1000 gap is `ReadMemory` calls that
   returned before hitting the download path). That is the
   structural cost Phase D targets.
2. **2.73ms/frame of pure CPU stall at menu idle.** Under Precise
   the game burns 8% of its 33ms budget on synchronous waits that
   a deferred-batch design should collapse into one or two per
   frame.
3. **mprotect churn is real but second-order.** 131/frame of
   `Protect` syscalls is noisy but each is fast (cumulative
   ~1.6s / 2911 frames = 547us/frame = 1.6% of budget). Worth
   reducing if easy but not the primary target.
4. **Frame pacing is hard-locked at 30fps.** The p50 = p95 = p99
   makes it hard to see stutter under menu load; the race
   workload will have more variance and will be the discriminating
   test.
5. **Fault-dispatch compute is unused on DriveClub.** The fault
   buffer path costs zero here — so the fixed 131,072-invocation
   per-flush compute dispatch quoted in the dossier is amortised
   to nothing in the games that don't exercise BDA-faulting
   loads. Good to know: it's a non-issue for most readback
   workloads.

### What the harness does NOT yet distinguish

- `Scheduler::Wait` is called from many places beyond readbacks
  (timeline-semaphore polling). The `wait_count` of 37M is not
  meaningful on its own. `finish_count` / `finish_ns` is the
  reliable discriminator.
- No attribution of Finish calls back to the call site (readback
  vs scheduler GC vs framebuffer-copy). If Phase D's Batched
  mode reduces Finish count, we'll know it came from the readback
  path by the co-movement with `buffer_read_count`.
- GPU-side time is not instrumented. Tracy zones already exist
  for that (`RENDERER_TRACE`) but they're not piped into the CSV.
  The CPU frametime percentiles are a proxy; good enough for
  comparing modes.

### Harness dev quirks observed

- distrobox-enter passes all env vars through (confirmed —
  `SHADPS4_READBACKS_METRICS` shows up in the process's
  `/proc/<pid>/environ`). No wrapper plumbing needed.
- `/tmp/<path>` is visible to both host and container (bind
  mount). Benchmark scripts can use `/tmp/dc-bench/*.csv` safely,
  but `/mnt/data/Projects/shadPS4/bench/` is more persistent
  across Bash-tool invocations and is the recommended default.
- The first smoke CSV came back via the `atexit` path when the
  window was closed — so SIGTERM paths don't need to be tested
  separately for the menu-close scenario.
