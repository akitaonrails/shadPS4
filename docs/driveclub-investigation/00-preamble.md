<!--
SPDX-FileCopyrightText: 2026 Fabio Akita
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Driveclub on shadPS4: v1.28 + gamma investigation

Branch-local progress log. Lives on `gamma-debug`. Not destined for upstream as-is;
trimmed/split excerpts may eventually feed upstream PRs (see "Upstream candidates"
at the end).

Sibling deploy notes in `~/Projects/distrobox-gaming/docs/driveclub-shadps4.md`
are still the operational runbook for the gaming distrobox. This doc only captures
what changed while working on this branch.

## Target

- **Game**: DRIVECLUB™, CUSA00003, stock v1.28 disc image, no premium DLC.
- **Host**: Arch Linux, Clang 22.1.3, CMake 4.3, Vulkan 1.4.341, SDL3 3.4.4.
- **GPU**: NVIDIA RTX 5090, driver 595.58.3.0, Wayland/Hyprland.
- **CPU**: Ryzen 9 7950X3D.
- **Runtime**: gaming distrobox, `~/.local/share/shadPS4/` as the data root,
  QtLauncher as the shell. Upstream nightly `main-2026-04-19` was the baseline.

## Fork layout

- **`akitaonrails/shadPS4`** (this repo) — `origin`, fork of `shadps4-emu/shadPS4`.
  Remotes: `origin` = my fork (push), `upstream` = canonical (read-only).
  `main` tracks `upstream/main` so `git pull` on `main` is always a fast-forward.
  Branch-local experiments live on feature branches such as `gamma-debug`.
- **`akitaonrails/DriveClubFS`** (`~/Projects/DriveClubFS`) — fork of
  `Nenkai/DriveClubFS`. Upstream went dormant after tag `1.1.0` (June 2025) with
  only README churn since; my fork is the de-facto maintained copy. No code
  changes applied yet — see v1.28 section below for why the "1.28 crashes" claim
  from the distrobox docs did not reproduce.

## Verified build recipe

```sh
git submodule update --init --recursive --jobs 8
CMAKE_POLICY_VERSION_MINIMUM=3.5 cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel "$(nproc)"
```

`CMAKE_POLICY_VERSION_MINIMUM=3.5` is mandatory on this host — CMake 4.3 dropped
compatibility with pre-3.5 `cmake_minimum_required`, and a few submodules
(miniz, etc.) still declare older minimums.

Output: `build/shadps4`, ~44 MB, PIE ELF, clang 22.
