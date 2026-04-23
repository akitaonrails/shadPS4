## Operational — pipeline cache corruption after many stop/start cycles

Symptom seen at 2026-04-22 ~12:44 onward, during the Phase 11 bisect
setup:

- Emulator launches but deterministically crashes at Vulkan swapchain
  init before any game code runs. Log ends with:
  ```
  [Render.Vulkan] <Info> vk_swapchain.cpp:81 Create: Swapchain created: ...
  [Debug] <Critical> signals.cpp:96 SignalHandler: Unreachable code!
  Unhandled access violation at code address 0x...: Read from address 0x10
  ```
- Reproduces on both scripted launches and on the user's own terminal.
- Reproduces with a **100 % symlink overlay** (zero patched files).
  That rules out our overlay state as the cause.
- The same binary (built 11:25 today) had been running fine through
  the earlier Munnar recordings at 12:15 and 12:31 on the same
  session.

Root cause: **corrupted persistent pipeline cache**. shadPS4 caches
compiled Vulkan pipelines at
`$XDG_DATA_HOME/shadPS4/cache/CUSA00003/`. Over many launches a cache
entry can end up inconsistent with the live pipeline state, and on
next boot dereferencing it produces the null-deref at swapchain init.

**Fix:**

```sh
CACHE=/mnt/data/distrobox/gaming/.local/share/shadPS4/cache
mv "$CACHE" "${CACHE}.backup-$(date +%s)"
mkdir -p "$CACHE"
```

Next cold launch takes several minutes of shader compilation but
boots cleanly. Confirmed recovered 2026-04-22 12:48 — user launched,
reached main menu, quit cleanly.

This is operational guidance, not investigation evidence. Anyone
else resuming work and seeing a deterministic swapchain-init crash
should wipe the pipeline cache first before debugging anything else.
