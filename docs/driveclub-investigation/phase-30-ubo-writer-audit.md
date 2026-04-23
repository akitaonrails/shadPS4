## Phase 30 — Exhaustive audit: who writes the 1936-byte UBO

Phase 29's pin experiments established that the race-start-dim
symptom is caused by **value suppression at slot [38] of the
1936-byte lighting UBO**: it ramps from ~8 to ~600 over 90s on
shadPS4, where on real PS4 it should start at ~200-400 and stay
there. Pinning the buffer from the outside doesn't fix it — the
value has to be **correct when the game writes it**, or downstream
reads pick up the wrong value.

So: who actually writes this slot, on both PS4 and shadPS4? Where
could shadPS4 miss / delay / corrupt that write?

### All candidate writers

Ranked by agent-estimated plausibility for our "low-then-ramps"
symptom.

#### 1. `ObtainBuffer` + ring-allocator first-copy race  (~90%)

**File:** `src/video_core/buffer_cache/buffer_cache.cpp`, `ObtainBuffer`
around line 375-382.

When a buffer is first bound read-only (`is_written=false`), and
the region is CPU-modified but not yet GPU-modified,
`stream_buffer.Copy()` uploads the buffer to a ring-allocated
StreamBuffer. This happens **once per binding**.

The game ring-allocates a fresh guest address for the 1936 UBO each
frame. If the game's first write into that fresh address happens
AFTER the emulator's `ObtainBuffer` reads, the CPU write is missed
and the GPU sees stale / uninitialised data. On subsequent
iterations the page-fault tracker catches up, but the first few
frames render with wrong values — and whatever downstream
feedback loop depends on them then "catches up" slowly.

**Verify:** instrument `stream_buffer.Copy()` for 1936-byte UBOs to
log the value at offset 0x98 at the moment of copy. If race-start
copies show zeros / NaN / low garbage, this is confirmed.

#### 2. Ring-allocator + MemoryTracker lazy-region race  (~70%)

**Files:** `src/video_core/page_manager.cpp` (`OnGpuMap`),
`src/video_core/buffer_cache/memory_tracker.h` (`CreateRegion`
line 155-156).

MemoryTracker creates a RegionManager lazily — only on first access.
If the game's CPU write to the newly-ring-allocated address races
the emulator's region-manager instantiation, the write happens
before the tracker can see it. Writes in that window are invisible
until something else lazy-triggers the region.

**Verify:** add a log in `CreateRegion()` with the region address
and wall-clock. Cross-reference with the first-write of each ring
address.

#### 3. Histogram compute dispatch skip  (~60% — already explored)

**File:** `src/video_core/renderer_vulkan/vk_rasterizer.cpp`
`ShouldSkipDriveclubDispatch`, `MaybePinDriveclubExposure`.

If the histogram / auto-exposure compute is skipped via
`IsComputeImageCopy` or `IsComputeMetaClear` heuristic, the game
never computes initial exposure and the scalar at offset 0x98
defaults to whatever the CPU writes (which would be a low
fallback).

**Status:** ruled out for the specific hashes we saw in Phase 28
(`0x66315c56` is a legitimate buffer-copy shader; `0xa6cfe095` is
skinning). But there may be OTHER histogram-like computes that get
caught if their signatures look like clears/copies.

#### 4. Page-fault handler delayed invalidation  (~40%)

**File:** `src/video_core/buffer_cache/fault_manager.cpp`
`ProcessFaultBuffer` line 80-175.

Faults are processed via a compute shader and the region update is
deferred via `scheduler.DeferOperation()`. If the deferred callback
runs late (multiple submits later), CPU writes in the interim
aren't registered.

**Verify:** log all `fault_areas[area]` updates against the submit
index. Check for a multi-submit gap during race-start.

#### 5. Readback gating on frame 0  (~35%)

**File:** `src/video_core/texture_cache/texture_cache.cpp`
`ProcessDownloadImages`, `DownloadImageMemory`.

Readback is gated on `IsReadbackLinearImagesEnabled()` and only
downloads images marked `GpuModified`. If the histogram/luminance
image used by eye-adapt isn't marked on first frames, readback is
skipped and the feedback loop starts with zero input.

**Verify:** check `readback_linear_images_enabled` setting (our
CUSA00003.json has it `false` currently). Enable it and re-run.

#### 6. Buffer-coherency mapping race  (~20%)

**File:** `src/video_core/buffer_cache/buffer.cpp`,
`ObtainBuffer` with coherent host-visible mapping.

If the GPU hasn't transitioned the buffer to host-visible or
flushed a prior frame's value, the mapped pointer reads stale
data. Missing `vkInvalidateMappedMemoryRanges` before first read
could cause this.

**Verify:** add explicit invalidation and retest.

#### 7. Push-constant overwrite  (~10% — listed for completeness)

If push-constants are written instead of UBO updates, a missing
update path could produce stale values. No evidence in the code
that Driveclub uses this pattern for exposure.

### Investigation plan

1. **#1 first** (ObtainBuffer first-copy gap): highest probability,
   most direct to instrument. Add a probe that captures the value
   at offset 0x98 at the moment of copy. If first-frame copies
   show wrong values, the bug is confirmed and we can check the
   page-fault timing.

2. **#2 in parallel** (region-manager race): add a log in
   `CreateRegion()`. Cheap to add.

3. **#5** (readback gating): try enabling
   `readback_linear_images_enabled` as a one-flag test. If the
   race-start dim goes away, that's the bug.

4. **#4** (fault-manager delayed): instrument after we have data
   from #1 and #2.

5. **#6** and **#3/#7**: leave for last.
