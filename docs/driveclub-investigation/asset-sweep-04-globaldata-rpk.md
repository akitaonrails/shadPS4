## `globaldata.rpk` findings

`globaldata.rpk` is the strongest shared asset candidate found so far,
but it is too fragile to brute-patch broadly.

### What is inside `globaldata.rpk`

Direct string and probe inspection found:

- shared transition tracks:
  - `FadeIn`
  - `FadeOut`
  - `FadeIn_Fast`
  - `FadeOut_Fast`
- shared postfx transition tracks:
  - `RTT_BLUR_ON`
  - `RTT_BLUR_OFF`
  - `RTT_GRADING_ON`
  - `RTT_GRADING_OFF`
- shared override actors:
  - `PostFX_BloomOverride_*`
  - `PostFX_ColourGradingOverride`
- shared cutscene cameras

This is exactly the kind of shared layer that could sit above both the
main race view and the mirror/prerace handoff.

Important limit from the track probe:

- the `globaldata` string found inside `india_road_point_02_n.rpk`
  appears in generic asset-path/provenance text alongside other pack
  references such as `worlds/india_common.rpk`
- it is **not** evidence that Munnar's `india_posteffects` directly
  references `globaldata` fade sequences at runtime

### What was tried

Two classes of `globaldata` patches were attempted:

1. broad shared patch
   - touched fade sequences
   - touched RTT blur/grading override tracks
   - touched shared override actors
2. fade-only patch
   - touched only:
     - `FadeIn`
     - `FadeOut`
     - `FadeIn_Fast`
     - `FadeOut_Fast`

### What happened

Both classes were unstable at boot.

Important correction discovered during testing:

- one boot crash was caused by the overlay accidentally missing
  `globaldata.rpk` entirely
- after restoring the original file, the broad and fade-only
  `globaldata` patches were still unstable enough that they are not
  trustworthy runtime shotguns

Current conclusion for `globaldata.rpk`:

- it remains a plausible shared blackout layer
- but it is **not** a safe brute-force patch target
- broad or direct sequence edits there should be treated as fragile and
  analysis-only until a much more surgical patching method exists
