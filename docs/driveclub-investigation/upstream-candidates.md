## Upstream candidates

Code that is clean enough to feed back to `shadps4-emu/shadPS4` once the
experiment is validated:

1. The swapchain `LOG_INFO` at `vk_swapchain.cpp::Create()` and the
   `GetFrameViewFormat` per-format INFO log are broadly useful diagnostics
   and would fit as a standalone PR (drop the `[gamma-dbg]` tag).
2. Exposing the `pp.gamma` push-constant as a per-game JSON knob (my env-var
   path is a branch-local shim; the real fix is a config field plumbed through
   `emulator_settings.cpp`).
3. Possibly a fix for the `A2R10G10B10Bt2020Pq → Unorm` fallthrough, once we
   actually see a Driveclub session declare that format.

Everything else in this branch is scoped to the investigation and wouldn't
be upstreamed as-is.
