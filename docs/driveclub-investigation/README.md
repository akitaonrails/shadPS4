# DriveClub v1.28 gamma/blackout investigation index

One file per phase. Read in order for the full story; jump to any
file directly for context on a specific probe.

| file | summary |
|------|---------|
| [00-preamble.md](00-preamble.md) | Target, fork layout, build recipe, and session zero setup |
| [phase-01-shader-compile-stalls.md](phase-01-shader-compile-stalls.md) | First session: shader compile stalls and pipeline-cache baseline |
| [phase-02-gamma-dim-image.md](phase-02-gamma-dim-image.md) | Gamma dim and the "dark car static image" first described; VideoOut gamma path ruled out |
| [phase-05-brightness-followup.md](phase-05-brightness-followup.md) | Follow-up brightness probes; sceVideoOutSetFlipRate and presenter path checked |
| [phase-03-v128-content-access.md](phase-03-v128-content-access.md) | v1.28 patch content access, asset layout, and libc_internal I/O fixes |
| [priority-note.md](priority-note.md) | Session priority note: blackout over slowness |
| [phase-04-slowness.md](phase-04-slowness.md) | Slowness / frame-pacing investigation; shader compile latency profiled |
| [race-start-blackout-040420.md](race-start-blackout-040420.md) | Race-start blackout narrowed to a repeatable submit-window gate |
| [upstream-candidates.md](upstream-candidates.md) | Upstream shadPS4 issues and PRs that may be relevant |
| [handoff-040420-late.md](handoff-040420-late.md) | Late session handoff: pipeline torture plan and render-target map |
| [phase-07-aggressive-torture-probe.md](phase-07-aggressive-torture-probe.md) | Aggressive pipeline torture: skip / null / nuke probes on scene passes |
| [post-reset-clean-branch-040421.md](post-reset-clean-branch-040421.md) | Branch reset to upstream HEAD, clean baseline re-established |
| [phase-08-ui-asset-surgery.md](phase-08-ui-asset-surgery.md) | UI asset surgery: newui panel strings and RPK asset replacement probes |
| [phase-09-eboot-binary-patch-plan.md](phase-09-eboot-binary-patch-plan.md) | Eboot binary patch plan: CameraFade / SetCamera string corruption strategy |
| [resume-point.md](resume-point.md) | Resume-point note after context reset |
| [phase-10a-rethink-040422.md](phase-10a-rethink-040422.md) | Full investigation rethink: renderer-side pipeline graph re-mapped |
| [phase-10b-video-diagnostic-animlib.md](phase-10b-video-diagnostic-animlib.md) | Video diagnostic and animlib: sceVideoOut gamma + animation library leads |
| [phase-11-multi-scalar-patch.md](phase-11-multi-scalar-patch.md) | Multi-scalar eboot patch: simultaneous string/branch mutations |
| [interlude-pipeline-cache-corruption.md](interlude-pipeline-cache-corruption.md) | Pipeline-cache corruption note and recovery procedure |
| [phase-12-bisect-closes-asset-side.md](phase-12-bisect-closes-asset-side.md) | Asset-side bisect closed: no RPK/PKZ asset is the blackout source |
| [file-path-cheatsheet.md](file-path-cheatsheet.md) | File-path cheatsheet for common log, cache, and asset locations |
| [asset-sweep-01-clean-misses.md](asset-sweep-01-clean-misses.md) | Asset sweep round 1: clean misses across globaldata categories |
| [asset-sweep-02-manual-sweep.md](asset-sweep-02-manual-sweep.md) | Asset sweep round 2: manual RPK file substitution |
| [asset-sweep-03-munnar-trackpack.md](asset-sweep-03-munnar-trackpack.md) | Asset sweep round 3: Munnar track-pack RPK isolated |
| [asset-sweep-04-globaldata-rpk.md](asset-sweep-04-globaldata-rpk.md) | Asset sweep round 4: globaldata.rpk deep dive |
| [asset-sweep-05-takeaways.md](asset-sweep-05-takeaways.md) | Asset sweep takeaways: no asset file drives the blackout |
| [asset-sweep-06-india-deep-dive.md](asset-sweep-06-india-deep-dive.md) | India track deep dive: per-sub-asset substitution on Munnar |
| [asset-sweep-07-external-tooling.md](asset-sweep-07-external-tooling.md) | External tooling: DriveClubFS and asset extraction helpers |
| [asset-sweep-08-probe-direction.md](asset-sweep-08-probe-direction.md) | Asset probe direction change: pivot from asset to renderer |
| [asset-sweep-09-conclusions.md](asset-sweep-09-conclusions.md) | Asset sweep final conclusions and renderer-side pivot rationale |
| [dump-driven-late-branch-analysis.md](dump-driven-late-branch-analysis.md) | Dump-driven late-branch analysis: RT graph mapped, carrier tree ruled out |
| [code-side-camera-family-followup.md](code-side-camera-family-followup.md) | Code-side camera family follow-up: all camera dispatcher arms tested, clean misses |
| [phase-13-ubo-dump-breakthrough.md](phase-13-ubo-dump-breakthrough.md) | UBO dump breakthrough: UBOLOG reveals monotonically rising luminance offset 3 |
| [phase-14-scene-pipeline-ubos-wrong.md](phase-14-scene-pipeline-ubos-wrong.md) | Scene-pipeline UBOs are the wrong layer; pivot to post-fx composition UBOs |
| [phase-15-runtime-texture-dump.md](phase-15-runtime-texture-dump.md) | Runtime texture dump: 1079 textures captured, vignette hypothesis explored |
| [phase-16-runtime-texture-mutation.md](phase-16-runtime-texture-mutation.md) | Runtime texture mutation: 14-batch rotation rules out all texture content as dim source |
| [phase-17-ubo-batch-smashing.md](phase-17-ubo-batch-smashing.md) | UBO batch smashing: 6-batch rotation hits crash ceiling, no dim found |
| [phase-18-push-constant-smashing.md](phase-18-push-constant-smashing.md) | Push-constant smashing: null result in one batch; GCN ud_regs not consumed visually |
| [phase-19-compute-dispatch-probe.md](phase-19-compute-dispatch-probe.md) | Compute dispatch probe: 19k dispatches logged, all 3 smash batches crash |
| [phase-19b-dim-vs-lift-diff.md](phase-19b-dim-vs-lift-diff.md) | Dim-vs-lift dispatch diff: 8 compute pipelines that stop exactly at lift identified |
| [phase-19c-dispatch-draw-skip-null.md](phase-19c-dispatch-draw-skip-null.md) | Dispatch and draw skip tests: all 101 dim-only pipelines skipped, dim runs unchanged |
| [phase-20-shader-dump-tonemap-inline.md](phase-20-shader-dump-tonemap-inline.md) | Shader dump analysis: tonemap found to be inline in scene materials, not a separate pass |
| [phase-21-fragment-to-flip-buffer.md](phase-21-fragment-to-flip-buffer.md) | Fragment shaders mapped to flip buffers; 8 final-composite fs shaders identified |
| [phase-22-tonemap-compute-identified.md](phase-22-tonemap-compute-identified.md) | Tonemap compute shader cs_0x2c918c06 identified; skip test confirms it drives the dim |
| [phase-22plus-tonemap-compute-patches.md](phase-22plus-tonemap-compute-patches.md) | Tonemap SPIR-V patches: ×10 boost makes race playable; bright→dim→bright pattern confirmed |
| [phase-23-cpu-exposure-intercept.md](phase-23-cpu-exposure-intercept.md) | CPU-side exposure intercept: scripted fade curve captured; cross-track threshold problem |
| [phase-24-dim-upstream-of-tonemap.md](phase-24-dim-upstream-of-tonemap.md) | Dim is upstream of tonemap; atmospheric/fog extinction or pre-tonemap HDR multiply suspected |
| [phase-25-frameorder-trace.md](phase-25-frameorder-trace.md) | Frame-order trace maps the pre-tonemap chain; Suspect B `0x00000663dc6eb328` (progressive 256→2560→10240) is the volumetric-fog signature and top target |
| [phase-26-ubo-fade-pin.md](phase-26-ubo-fade-pin.md) | UBO fade pin: memory diff finds the scripted-blackout fade; per-slot clamp-min on 1936 + 224 byte UBOs. Content-aware three-state lifecycle, headlight-flag expire, luminance-preserve tonemap gamma 1.9 |
| [phase-28-pivot-to-emulator-code.md](phase-28-pivot-to-emulator-code.md) | UBO-pin approach has hit its limit; sun/sky work, ambient/GI doesn't — pivot to investigating shadPS4 translation gaps. Five candidates ranked; plan to audit ExecuteShaderHLE, image classification, layout transitions, IMAGE_STORE_MIP fallback, OpImageFetch LOD |
| [phase-29-calibrated-state.md](phase-29-calibrated-state.md) | Recording harness + screenshot-timestamp correlation reveals the real bug: the 1936-byte scene-lighting UBO has ~30 slots in [144..295] that stay denormal/uninitialised for the first ~90s of a race. At the 1:30 auto-recalibration moment, the game writes their real values and scene looks correct. Captured calibrated-state snapshot as `tools/driveclub_pin_snapshots/canada_calibrated_1936.bin` for calibrate-at-arm pin. |
| [phase-30-ubo-writer-audit.md](phase-30-ubo-writer-audit.md) | Exhaustive audit of who/what writes the 1936-byte UBO slot [38]. Seven candidates ranked. #1 (ObtainBuffer first-copy race in buffer_cache.cpp) and #2 (lazy RegionManager in memory_tracker.h) are the top suspects. Plan to instrument both + try readback_linear_images_enabled=true as a quick test. |
| [phase-31-readbacks-mode-fix.md](phase-31-readbacks-mode-fix.md) | **RESOLVED.** `readbacks_mode: 2` (Precise) in per-game config makes DriveClub playable. Auto-exposure is a GPU→CPU histogram feedback loop; without buffer readback, the CPU integrator drifts from stale zeros. Novel config — no prior guide documented `Precise` mode for CUSA00003. Off by default upstream because of known AMD instability and Bloodborne regressions. |
| [investigation-effort-accounting.md](investigation-effort-accounting.md) | Total spend before resolution: 44 fork commits over 3 days, 15,668 net lines changed, 33 numbered phases, 6,867 lines of prose, vk_rasterizer.cpp grown 3.4×, 32 runtime probe env vars, ~427 MB of runtime/asset/log artifacts. Resolution: one integer in per-game config. Cleanup scope: ~470 MB reclaimable. |
