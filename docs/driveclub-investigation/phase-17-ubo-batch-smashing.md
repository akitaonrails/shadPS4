## Phase 17 — UBO batch smashing

Texture-content exhaustion at the end of Phase 16 pushed us into the
uniform-buffer path. Same mechanics, new target:
`MaybeSmashDriveclubUbo(base, size, cb_idx, is_written, is_formatted)`
hooks `Rasterizer::BindBuffers` just before `buffer_cache.ObtainBuffer`,
writes a per-address splitmix64 tint over the full UBO range, then
calls `buffer_cache.InvalidateMemory(base, size)` so the Vulkan UBO
re-uploads the clobbered bytes. Env knobs, all runtime and zero-cost
when unset:

- `SHADPS4_DC_UBO_SMASH=1`            master enable
- `SHADPS4_DC_UBO_SMASH_CB=0,1,...`   cb-index include list
- `SHADPS4_DC_UBO_SMASH_MIN_SIZE=N`   byte floor
- `SHADPS4_DC_UBO_SMASH_MAX_SIZE=N`   byte ceiling (0 = unlimited)
- `SHADPS4_DC_UBO_SMASH_ONLY_READ=1`  skip SSBOs / formatted buffers

Shares the broader gates with texnuke (`SHADPS4_DC_NUKE_AFTER_ARM`,
`SHADPS4_DC_NUKE_KIND`, plus the race-window guard).

### Six-batch rotation

Time-boxed to ≤6 bracket tries before pivoting to a different class:

| # | filter                                        | observed                                                        | dim? |
|---|-----------------------------------------------|-----------------------------------------------------------------|------|
| 1 | all read-only scene UBOs                       | heavy blinking, whites/blacks/red-yellow explosions             | yes  |
| 2 | `CB=0` only                                    | crash on race load (SIGTRAP) — cb0 carries view / clip-space    | —    |
| 3 | `CB=1` only                                    | same crash — cb1 also carries transforms                        | —    |
| 4 | `MAX_SIZE=256`, all cb                         | scene over-exposes to white with cyan bokeh rings               | yes  |
| 5 | `CB=2,3,4 MAX_SIZE=256`                        | colour scene blinks + persistent white lens-dirt bokeh overlay  | yes  |
| 6 | `MIN_SIZE=16 MAX_SIZE=64`, all cb              | cyan/green bokeh overlay following the car's headlights         | yes  |

Screenshots of rounds 4..6 show the bokeh patterns tracking the car's
own light sources — those small UBOs are per-light constants, and
the visible transition between "during blackout" and "after lift"
still plays out over our garbage overlay (darker HDR beneath → white
bokeh; brighter HDR beneath → greener bokeh). The dim keeps cycling
on its usual 8–30 s schedule through every one of the six rounds.
Not a single batch shifts blackout timing, extent, or lift moment.

### Honest read

**The UBO smash did not hit the dim driver within the six-batch
budget.** That's not the same as "the dim isn't in UBOs" — it means
the brackets we could safely test (cb≥2, size ≤64, etc.) don't cover
it, and the brackets that would cover it (cb=0 / cb=1, large-size)
are load-bearing for the pipeline and crash when smashed. We are at
a real diagnostic limit of the random-fill approach: any bracket
wide enough to include the dim also includes transforms that kill
the session.

What's still on the table for a smarter probe:

1. **Targeted single-field mutation, not a random fill.** Pick each
   4-byte offset inside a candidate UBO one at a time, force it to a
   known value (0, 1, or the observed "bright" value), and watch
   the blackout. This is strictly safer than the random tint because
   adjacent fields stay valid. The offset-by-offset sweep that
   Phase 13 started but did not finish.
2. **Per-shader offset skips.** Keep the random fill, but pre-
   compute "skip bytes 0..63 of cb0 because this shader's vertex
   transform is there". Requires parsing the shader's buffer
   resource table.
3. **Push constants.** Never instrumented. ≤128 bytes, per-draw, and
   exactly the place an engine would put a per-draw multiplier or
   fade scalar.
4. **Compute dispatch output.** Still unprobed. Bloom / auto-
   exposure / luma integration are the likeliest dim homes; they
   feed graphics through SSBOs our UBO smash already covers, but
   only if the SSBO is bound as a uniform descriptor — if it's bound
   as storage, `SHADPS4_DC_UBO_SMASH_ONLY_READ=1` filters it out.
   A compute-side probe would cover it either way.

### Commit state at end of Phase 17

`src/video_core/renderer_vulkan/vk_rasterizer.cpp` now carries:

- `MaybeSmashDriveclubUbo` + its knob-readers (Phase 17)
- all of Phase 16's texnuke infrastructure
- all of Phase 15's texdump infrastructure
- all of Phase 13's targeted UBO clamp/restore/nuke probes

`scripts/run_driveclub_overlay.sh` now passes through all nine of
the `SHADPS4_DC_*` probe env vars.

Nothing mutates emulator behaviour unless a matching env var is set.
