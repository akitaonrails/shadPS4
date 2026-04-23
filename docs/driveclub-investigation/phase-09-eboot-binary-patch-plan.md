## Phase 9 — eboot binary-patch plan

### Target

The `FreeplayGetInCar` controller class inside
`/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin`.
Specifically: neutralize its render/update function so the page still
navigates and transitions (page dictionary stays complete) but no 3D
car and no dim layer are drawn while the page is active.

Known facts from early recon (2026-04-21):

- `eboot.bin` is 25 MB; `file` reports `data` (stripped / unrecognised
  ELF variant — PS4 OELF, probably post-sceAuthEtcFromSelf decryption).
- `FreeplayGetInCar` ASCII string is present twice in the binary.
  Siblings `TourGetInCar`, `ChallengeGetInCar`, `GetInCar` also
  present.
- `readelf` / `objdump` / `nm` are available. Ghidra and radare2 are
  not currently installed on this host; we will add one before doing
  real disassembly work.

### Approach

1. **Install a disassembler** with PS4 OELF support. Shortlist in
   preference order:
   - `ghidra` (Arch: `pacman -S ghidra`) — de facto standard for
     console RE; ships with a built-in OELF loader via community
     plugins.
   - `radare2` / `rizin` — lighter, script-friendly; OELF partial
     support via `iaito` / plugins.
   - As a last resort: parse the OELF segments by hand in Python,
     feed the `.text` range to `capstone` (`pip install capstone`).

2. **Find the controller's class/vtable** in two passes:
   a. Locate the two ASCII occurrences of `FreeplayGetInCar` via
      `grep -abo` on the raw file. Record file offsets.
   b. In the disassembler, search for data-ref / LEA references to
      those offsets. They will fall inside the controller registry /
      class constructor. Follow to the class's virtual-method table.

3. **Identify the render dispatch**. The controller vtable has a
   handful of virtual methods; the one we want is the per-frame
   render call that issues the 3D car draws + dim composite. Common
   naming patterns: `Render`, `OnRender`, `Draw`, `OnDraw`,
   `UpdateAndRender`. The method will dispatch into the engine's 3D
   renderer (many sceGnm / graphics-module calls downstream).

4. **Patch strategy**. Two variants to try in order of lowest risk:
   a. Prologue RET — replace the first byte of the method with `0xC3`
      (`ret`). The page is still active, timer still counts, but the
      method is a no-op. If the method has a non-void return, we may
      need to stub a return value (zero the return register before
      the ret).
   b. NOP the specific draw call — if prologue-RET breaks the flow
      (hang / soft-lock), find the GNM draw dispatch inside the
      method and replace its `call` instruction with NOPs of equal
      length.

5. **Apply safely**:
   - Copy real `eboot.bin` into
     `/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin`
     (replacing the current symlink).
   - Keep the unpatched copy as `eboot.bin.vanilla` next to it.
   - Patch the overlay copy byte-for-byte. Document every patched
     offset in this doc and in a companion `eboot_patches.md`.

6. **Verify**: launch via `scripts/run_driveclub_overlay.sh`, run a
   Munnar 19:30 start, observe whether the overlay disappears while
   the rest of the flow (page navigation, HUD, race start) is intact.

### Rollback

The overlay eboot lives only at
`tmp/driveclub_overlay/CUSA00003/eboot.bin`. Deleting it and
re-symlinking to the real install restores vanilla behaviour in one
command. No destructive operation touches the installed game tree.

### Risks

- The controller may have more than one callsite that renders the
  car (e.g. separate `Update` + `Render` method, or the render is
  done by a child actor). First patch might be incomplete.
- Driveclub's v1.28 eboot may have integrity checks. If a plain byte
  patch causes the OELF to fail its internal signature check, we'll
  see it as a launch-time refusal rather than a visual change.
- Patching may break `TourGetInCar` / `ChallengeGetInCar` if those
  share the same controller class via inheritance. Minor — we aren't
  using tour / challenge modes in the repro.

### Recon already completed (2026-04-21 end-of-session)

Concrete data gathered, stored at
`/mnt/data/Projects/shadPS4/tmp/eboot_extract/`:

- `eboot.elf` — inner ELF carved out of the OELF at file offset
  `0x120` (`OELF header = 32 B + 8 × 32 B segment descriptors`,
  rounded to 16 B → 0x120).
- `eboot.et_exec.elf` — same ELF with `e_type` flipped from
  `ET_SCE_EXEC (0xFE10)` to `ET_EXEC (0x0002)` and OSABI from
  `FreeBSD (0x09)` to `SYSV (0x00)` so GNU binutils and LLVM tools
  accept it. Standard `objdump` still refuses (no section headers);
  `llvm-objdump -d` works.

ELF layout:

| Segment | File offset | VA | File size | Flags |
|---|---|---|---|---|
| LOAD #0 (text + rodata) | `0x4000` | `0x0` | `0x1551ab8` | R E |
| LOAD #1 (data) | `0x1558000` | `0x1554000` | `0x11be20` | RW |

→ `file_off (text) = VA + 0x4000`.
→ `file_off (data) = VA - 0x1554000 + 0x1558000 = VA + 0x4000`.

String VAs (all in text segment):

| String | VA | File offset (inner ELF) |
|---|---|---|
| `TourGetInCar` (×2) | `0x12c4041` / `0x12c4052` | `0x12c8041` / `0x12c8052` |
| `FreeplayGetInCar` (×2) | `0x12c4848` / `0x12c485d` | `0x12c8848` / `0x12c885d` |
| `ChallengeGetInCar` (×2) | `0x12c4c57` / `0x12c4c6d` | `0x12c8c57` / `0x12c8c6d` |

The two `FreeplayGetInCar` strings are a 20-char
`FreeplayGetInCarPage` followed by a 16-char `FreeplayGetInCar`
(the page's class name and the controller's class name).

Instructions that reference `FreeplayGetInCar` string VAs:

| VA | Bytes | Instruction | Targets |
|---|---|---|---|
| `0xf89b60` | `48 8d 05 f6 ac 33 00` | `lea rax, [rip+0x33acf6]` | `0x12c485d` (controller name) |
| `0xf89b70` | `48 8d 05 d1 ac 33 00` | `lea rax, [rip+0x33acd1]` | `0x12c4848` (page name) |

Each is followed immediately by `c3` (`ret`) and padding NOPs. These
are two virtual-method name-getter functions — standard Itanium
RTTI accessors that return a pointer to the class's ASCII name.

The constructor that writes the vtable pointer for this class is
directly above them, at `VA 0xf89b00-0xf89b50`:

```
f89b39: lea rax, [rip+0x6528a0]   # = 0x15dc3e0
f89b40: add rax, 0x10             # points to first vfn slot
f89b44: mov [rbx], rax            # *this = vtable
```

Vtable lives at VA `0x15dc3e0` (file offset `0x15e03e0` inside the
inner ELF, inside LOAD #1 data segment). Its function-pointer slots
are zero or carry **SCE dynamic relocation encoded placeholders**:

```
vtable at VA 0x15dc3e0 / file 0x15e03e0:
  +000  0000000000000000  (offset-to-top)
  +008  0000000700000016  (RTTI ptr reloc)
  +010  0000000000000000  vfn[0]
  +018  0000000700000017  vfn[1]  (reloc, not an addr)
  ... rest zero
```

The nonzero 8-byte values (`0x0000000700000016` etc.) are SCE
relocation-record indices, not resolved function addresses. The
static file does not carry the real virtual method addresses for
`Render` / `OnDraw` / etc. — those are filled in by the dynamic
linker at runtime.

### What that means for the patch plan

We cannot pre-compute the `FreeplayGetInCar::Render` file offset by
reading the static ELF alone. Two viable paths from here:

1. **Parse the SCE dynamic section** (DYNAMIC segment starts at file
   offset `0x186e568`) to extract the relocation table and symbol
   table, resolve the vtable slots to their intended target
   addresses, and patch the render slot.
   - Needs either a tool with SCE OELF support (Ghidra + PS4 plugin,
     rizin + rz-pssl, or custom Python) or the shadPS4 loader code
     itself extended to dump resolved relocations.

2. **Patch the class constructor instead of the vtable**. The
   constructor at `VA 0xf89b00` stores `0x15dc3e0 + 0x10` as the
   vtable pointer in the object. If we change the source address to
   a vtable that contains a NO-OP/RET stub for the render slot, any
   virtual dispatch becomes a no-op. Requires either:
   - a scratch region in the file to hold a fake vtable, or
   - editing the existing vtable after dynamic linking, which is
     equivalent to path 1.

3. **Patch the RTTI name getter**. The name-getter at `VA 0xf89b60`
   returns a pointer to the `FreeplayGetInCar` class name. If we
   change the return string to point at a different class name that
   the factory does not recognise, the registry lookup for the
   `getincar.freeplay` page fails at controller-registration time,
   and the page becomes unrenderable in the same way the
   `comment-out-the-Page-entry` experiment did — which crashed the
   game. Not viable.

4. **Patch a downstream SCE-GNM draw dispatcher**. If we can find a
   specific draw-state set or texture binding that only the
   GetInCar controller path hits, we can NOP it. Requires full code
   analysis. Same tooling requirement as path 1.

### Recommended order of operations for next session

1. **Install a real RE tool with PS4 OELF support.** Fastest option
   likely `ghidra` from the Arch repos, plus the community
   `ghidra-ps4-loader` plugin. Alternative: rizin with SCE plugins.
   Either gives us proper relocation resolution and xref tracing.

2. **Load `eboot.et_exec.elf` (or the original OELF if the plugin
   can handle it) into the chosen tool.** Use the string VAs above
   as anchor points. Find the class vtable at VA `0x15dc3e0` once
   relocations are resolved. Identify the render method slot.

3. **Patch the overlay eboot**, not the real install. Overlay path:
   `/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin`.
   Current overlay is a symlink — break it and drop a patched copy.
   Keep `eboot.bin.vanilla` beside it for one-step rollback.

4. **Test via `scripts/run_driveclub_overlay.sh`.** Repro track:
   Munnar 19:30 clear.

### File-path cheatsheet for the binary-patch work

```
# Original OELF (read-only install)
/mnt/terachad/Emulators/EmuDeck/roms_rare/ps4/CUSA00003/eboot.bin   (25 MB OELF)

# Carved inner ELF (writable, for analysis)
/mnt/data/Projects/shadPS4/tmp/eboot_extract/eboot.elf              (inner ELF, ET_SCE_EXEC, FreeBSD)
/mnt/data/Projects/shadPS4/tmp/eboot_extract/eboot.et_exec.elf      (same, retyped to ET_EXEC + SYSV so
                                                                      GNU/LLVM tools parse it)

# Overlay eboot (write path for the patched binary)
/mnt/data/Projects/shadPS4/tmp/driveclub_overlay/CUSA00003/eboot.bin (currently symlinked to real install)
```

Everything above this line is asset-side; everything below will be
byte-level patching of the eboot. No emulator source changes planned
for this phase.
