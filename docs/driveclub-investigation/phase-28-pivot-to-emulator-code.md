## Phase 28 — Pivot: hunt the emulation gap, not more UBO pins

Phase 26/27 landed a working scripted-blackout fix via UBO pins (the
1936-byte fade slots and the 224-byte sun UBO) plus a tonemap
luminance-preserve boost. Those make Munnar India 19:30 playable.

Trying to fix Canada's dusk/night scenes the same way (UBO pins +
ambient floors) revealed the limit of that approach:

- The 1936-byte UBO slots `[24..31]` were identified as **sun
  intensity**. Clamping them lights mountain tops (sun-facing
  surfaces) but not the foreground.
- The 1008-byte UBO slots `[84..90]` were identified as **sky-dome
  color**. Clamping them lights the sky and reflective surfaces but
  not the foreground.
- No third slot group matched a "fill light" / ambient-diffuse /
  GI-irradiance pattern in any of the 237 common buffers captured.
  Clamping every remotely-plausible candidate made no visible
  difference on the diffuse foreground.

### Reframing

The game runs correctly on real PS4 hardware. The ambient/GI
rendering code shipped in the game is correct. That means the
missing foreground illumination is **not a wrong value in a UBO
we haven't found** — it's a **shadPS4 translation gap**. A compute
pass, a shader opcode, a texture layout, a descriptor binding, or
a resource classification that shadPS4 gets wrong for Driveclub,
causing the computed ambient/GI output to be silently dropped or
zeroed before it reaches the scene lighting pass.

Pinning UBO values is a bandage. The real fix is somewhere in
shadPS4's source — and because the game works on PS4, that's
where we have to look.

### Candidate emulator-side bugs

Explore agent swept `src/shader_recompiler/`, `src/video_core/`
looking for silent-failure paths that could produce "sun works,
sky works, ambient/GI drops". Top five, ranked by fit to the
symptom:

| # | Where | Why suspect |
|---|---|---|
| **4** | `vk_rasterizer.cpp:3083-3171` (`ExecuteShaderHLE`) | Some compute dispatches are intercepted by HLE replacements and skipped entirely. If a Driveclub ambient/GI compute shader is *misidentified* as a metadata-clear or image-copy, its dispatch is dropped silently. Cleanly explains "some computes run, others vanish" while sun and sky dispatches (different hashes) continue to work. |
| **2** | `vk_rasterizer.cpp:3535, 3558` (image classification) | `FindImage` decides whether a compute-written image binds as `StorageImage` or `SampledImage`. If a GI output texture is classified as sampled, the compute's write call is valid-looking but silently dropped by Vulkan because the binding type is wrong. |
| **5** | `vk_rasterizer.cpp:3604-3612` (layout transitions) | Storage-image layout transitions gate on `is_storage`. If is_storage is false at transition time, the image goes to `eShaderReadOnlyOptimal` right before the compute tries to write it — same silent-drop outcome as #2. Shares root cause with #2. |
| **1** | `resource_tracking_pass.cpp:551-594` (IMAGE_STORE_MIP fallback) | When `supports_image_load_store_lod=false`, LOD-store instructions fall back to array-index addressing. Heavily modified in commit `1bb152d9`. If the fallback is broken for Driveclub's GI texture writes, the mip target is wrong. |
| **3** | `emit_spirv_image.cpp:~278` (OpImageFetch LOD restriction) | Recent fix `ffbcd0d3` prevents passing both Lod and Sample to OpImageFetch. If Driveclub's GI sampling reads with both, the restriction may drop the explicit LOD, returning mip 0 bias instead of a correct mip level — could return near-zero. |

### Investigation plan (priority order)

1. **Candidate #4** (ExecuteShaderHLE skip) — highest priority.
   Steps:
   - Read `vk_shader_hle.*` and identify every HLE replacement
     that causes `DispatchDirect` to short-circuit.
   - Log every compute pipeline hash that takes the HLE path in
     a Canada race.
   - Cross-reference with our frame-order trace: any pipeline that
     runs `(60,34,1)` or `(256,128,1)` shapes in the compute-dense
     pre-G-buffer segment and hits HLE is a candidate.
2. **Candidate #2** — image storage classification. Trace
   `FindImage` and `is_written` / `is_storage` flag propagation
   from compute shader info through descriptor write.
3. **Candidate #5** — image layout transitions. Tied to #2; same
   investigation typically resolves both.
4. **Candidate #1** — IMAGE_STORE_MIP fallback. Read commit
   `1bb152d9` context; check if Driveclub shaders use LOD store.
5. **Candidate #3** — OpImageFetch LOD. Read commit `ffbcd0d3`;
   check if Driveclub GI sampling uses both Lod and Sample.

### What to change when a bug is found

- Fix the emulator code path, not the game state.
- Keep a before/after test: capture the 1008-byte and 1936-byte
  UBO contents plus frame-order trace before, apply the emulator
  fix, re-capture. Scene should brighten in the FOREGROUND without
  needing UBO pins.
- Once a real emulator fix lights the scene, the UBO pins and
  ambient floors can be removed or downgraded to "scripted-fade
  only" (their original purpose).

### Durable principle

When the emulator produces egregiously wrong output (pitch-black
unplayable scene in a racing game), the hypothesis is always
"emulator translation gap", never "author intended zero ambient".
The fix lives in `src/`, not in overlayed UBO clamps.
