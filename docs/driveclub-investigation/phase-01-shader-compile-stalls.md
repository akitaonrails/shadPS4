## Phase 1 — shader compile stalls

### Symptom

Driveclub menus animated at crawl speed on first entry, then snapped to fast
once past that moment. Pattern repeats on next cold launch.

### Root cause

shadPS4's persistent pipeline cache was not enabled by default. A single session
compiled ~527 shaders + ~354 pipelines from scratch; every new UI pipeline
stalled the GPU comm thread during its first use. Nothing cached to disk meant
the same cost every launch.

### Fix (config-only, no code)

Set `Vulkan.pipeline_cache_enabled = true` in
`~/.local/share/shadPS4/config.json`. Left `pipeline_cache_archived` at `false`
(zip-compressed cache) — we want fast load over small disk footprint.

First launch after enabling still has to compile everything. Second launch
onwards replays the cached pipelines and menus should be snappy.

Backup of the original config preserved as
`config.json.bak-before-pipeline-cache` next to the live file.
