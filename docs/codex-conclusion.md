<!--
SPDX-FileCopyrightText: 2026 Fabio Akita
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Codex conclusion: what the Driveclub blackout investigation actually taught us

This is the hindsight document for the Driveclub blackout chase.
It is not another raw phase log. The goal is to answer the obvious
post-mortem questions after [Phase 26](driveclub-investigation/phase-26-ubo-fade-pin.md):

- could we have reached the discovery earlier?
- what did we miss?
- was the answer truly hidden?
- what should we do differently next time?
- and how should we honestly describe the current "fix"?

## What Phase 26 actually found

[Phase 26](driveclub-investigation/phase-26-ubo-fade-pin.md) is the
first phase that explains the blackout in a way that matches the full
symptom set.

The strongest conclusion is:

- the blackout is not primarily "a bad post-process pass"
- it is not "a single wrong texture"
- it is not "a UI fade asset"
- it is not "the tonemap compute alone"
- it is not "one draw/dispatch we can simply skip"

It is a **CPU-driven lighting fade state** written into at least two
shared UBOs:

- a 1936-byte scene-light buffer
- a 224-byte sun/key-light transform buffer

During the race-start blackout, the game writes very small light-scale
values into those buffers. The final dim image is what the rest of the
frame graph naturally produces from those already-attenuated lighting
inputs.

That is why so many "kill the pass" experiments failed. We kept trying
to find the pass that *caused* the dark image, but by then the dark
lighting state was already resident in memory and many later passes were
just faithfully consuming it.

The actual breakthrough was not just "pinning a UBO made it brighter".
The real breakthrough was the **three-state snapshot diff**:

- bright
- dim
- recovered

That turned an unbounded search into a finite set of buffers whose bytes
actually changed with the visual state.

The second real breakthrough was the first pin's weird result:

- pinning the 1936-byte recovered-state UBO did **not** remove dim
- it shifted the dim later by about 30 seconds

That was the "aha" moment. It proved two things at once:

- the pinned UBO was definitely live and load-bearing
- it was not the only fade driver

That is what led directly to the second 224-byte UBO and to the final
model: overlapping scripted fade plus normal time-of-day lighting.

## Could we have found this earlier?

Yes, but only after the investigation had already ruled out a few whole
classes of wrong models.

I do **not** think we could have jumped straight to Phase 26 from
Phase 1 or Phase 2. The early work still mattered because it eliminated:

- present/gamma/video-out misunderstandings
- obvious texture-cache theories
- declarative UI fade assets
- track-local pack edits
- brute-force `globaldata` patching
- obvious named `CameraFade` / `ScreenFadeOut` string leads

But in hindsight, the direct path to the final answer was available
earlier than we took it. The best candidate pivot points were:

- [Phase 10a](driveclub-investigation/phase-10a-rethink-040422.md):
  this was the first strong warning that "named fade things" were a bad
  model and that the real mechanism was a shared state transition.
- [Phase 13](driveclub-investigation/phase-13-ubo-dump-breakthrough.md):
  the moving UBO value proved that live per-frame state, not static
  assets, was driving visible darkness.
- [Phase 17](driveclub-investigation/phase-17-ubo-batch-smashing.md):
  once broad UBO smashing hit the crash ceiling, the next move should
  have been snapshot-and-diff, not more brute-force mutation.
- [Phase 20](driveclub-investigation/phase-20-shader-dump-tonemap-inline.md):
  this phase practically says the answer lives in a shared scene-state
  buffer read by many shaders.
- [Phase 23](driveclub-investigation/phase-23-cpu-exposure-intercept.md):
  this captured the game writing the fade curve in real time. At that
  point the search should have become "which CPU-written state tracks
  bright/dim/recovered?" rather than "which pass still darkens it?"

So the honest answer is:

- **yes**, we could have found it earlier
- but probably not dramatically earlier than the mid-to-late UBO phases
- the most realistic missed shortcut was "Phase 13/17/20 style clues
  should have triggered Phase 26-style memory snapshots sooner"

## What we missed

We missed the difference between a **carrier** and a **controller**.

Many phases correctly found places where the blackout was visible:

- late composite targets
- tonemap compute
- post-fx passes
- scene textures
- UI-adjacent imagery

But visibility is not control.

We repeatedly found layers that *carried* the dark image and treated
them as if they were the thing *causing* the dark image. In hindsight,
the investigation lost time whenever it assumed:

- the darkest visible layer must be the source
- the latest pass touching the image must own the bug
- a fade-looking symptom must belong to a fade-named asset or handler

The final result says the opposite:

- the dark image was often already "baked into the state" before the
  interesting later pass even ran
- many late passes were innocent readers
- the control lived in shared lighting state, not in the most obvious
  fade-shaped content

We also missed an important methodological signal:

- broad random corruption is great for ruling out whole classes
- but once the surviving target class is load-bearing, random
  corruption becomes the wrong instrument

By Phase 17 we already knew large shared UBOs were too important to
smash safely. From there, the better tool was always going to be
**state capture and diff**, not more aggressive smashing.

## Was the discovery truly hidden?

Partly yes, partly no.

It **was** genuinely hidden in the sense that the bug's visible
signature pointed in many wrong directions at once:

- UI remained bright
- mirror went black
- main scene stayed faintly visible
- tonemap skip changed the result
- textures and late buffers clearly correlated with the bad frame

That is a perfect recipe for false attribution. A pass can look guilty
simply because it is the last place where the wrong state is still easy
to see.

But the discovery was **not** completely hidden. Several phases already
contained the real clues:

- Phase 10a: stop chasing names and assets; think state machine
- Phase 13: a live moving UBO value correlates with the fade
- Phase 20: shared scene-state buffer read by many shaders
- Phase 22+: the real runtime shape is bright -> dim -> bright
- Phase 23: the fade is being written by the game, on the CPU side

So the fair conclusion is:

- the final answer was hidden by symptom overlap
- but it was not absent from the evidence
- we had the right clues earlier, but we did not combine them into the
  right search strategy quickly enough

## In hindsight, what tests should we have done earlier?

If I had to compress the 4-day lesson into a better sequence, it would
look like this:

1. Rule out present/video-out mistakes early.
2. Rule out obvious asset/UI carriers early.
3. As soon as multiple late-pass probes are clean misses, stop
   searching by names and start searching by **state transition**.
4. Once a stable repro exists, anchor three visual states:
   bright, dim, recovered.
5. Snapshot shared GPU/CPU-fed state at those anchors and diff it.
6. Only after the diff points at a candidate buffer, mutate that exact
   buffer in place.

The specific test we should have used earlier was:

- periodic UBO snapshots around the race-window gate
- three-way byte diff across bright/dim/recovered

That one test is what Phase 26 finally did, and it was more decisive
than dozens of skip/nuke/patch experiments because it asked the right
question:

- not "which pass looks suspicious?"
- but "which bytes actually change with the visual state?"

## What was the real aha moment?

In my view it was **not** the first tonemap patch and not even the
first UBO pin by itself.

The real aha moment was:

- the first lighting-UBO pin held race start bright
- then the scene dimmed anyway, later

That single result collapsed the old model. It proved this was not one
broken scalar or one broken pass. It was layered state:

- one fade source had been neutralized
- another independent fade source was still alive

From there the rest of Phase 26 became almost inevitable:

- diff again
- find the second UBO
- pin both
- then manage pin lifetime so natural time-of-day can resume

That is the moment where the investigation changed from "pattern hunt"
to "mechanism identified".

## Lessons for next time

- Separate **carrier**, **reader**, and **controller**. The place where
  the bug is visible is not automatically the place that owns it.
- When many late-pass interventions are clean misses, the bug is
  probably upstream state, not downstream math.
- Stop chasing names once the first few name-based probes miss.
  Runtime state beats string adjacency.
- Broad corruption is for class elimination, not final diagnosis.
  Once crashes dominate, switch to snapshots and diffs.
- Build probes that compare **known visual states**, not just
  "before/after patch". Bright/dim/recovered is far more informative.
- Keep one or two reliable visual oracles. For Driveclub, the mirror and
  HUD separation was invaluable even when some of our interpretations
  around it were wrong.
- When the game writes state from the CPU each frame, pass skipping will
  often look weaker than it should. The pass is only reading already-bad
  inputs.
- The best reusable debugging tool from this entire investigation is not
  a Driveclub hack. It is the ability to snapshot, diff, and selectively
  pin bound buffers at runtime.

## What the current "solution" really is

The current state is a **mitigation**, not a root-cause fix.

What we have now is:

- a Driveclub-specific pin of two recovered-state lighting UBOs during
  the scripted blackout window
- a tonemap output shaping patch that lifts perceived brightness while
  preserving hue better than blunt RGB gain

That combination makes Munnar 19:30 much more playable. It removes the
worst "go nearly black for 10-30 seconds" behavior. But it does **not**
prove that shadPS4 is now emulating Driveclub correctly.

The remaining caveats matter:

- overall luminance is still too dim
- the track still becomes very dark as night creeps in
- the pin snapshots are Munnar-derived and may not generalize cleanly
- the workaround depends on `if Driveclub then ...` style logic
- the real reason those game-written lighting values are acceptable on
  real hardware is still unknown

So the correct conclusion is: **yes, something is still missing in the
PS4 simulation for this game**.

I would phrase that carefully, though. We do not yet know whether the
missing piece is:

- a rendering-path mismatch
- a timing/synchronization difference
- a display/tonemap expectation mismatch
- a subtle lighting-state interpretation bug
- or a combination of those

What we do know is simpler:

- if Driveclub were fully correct in shadPS4 today, it would not need
  per-game UBO pins and a shader patch to stay comfortable to play
- therefore the present result is not "fixed Driveclub"
- it is "a targeted countermeasure that suppresses the ugliest symptom"

That is still useful. It made the game much less hostile. But it is not
an upstream-quality explanation yet.

## Final conclusion

The four days were not wasted. They closed entire false fronts and
built the runtime tools that finally made the right question possible.
But the final answer also says we spent too long searching for a
special pass, special texture, or special fade asset when the real bug
was shared lighting state evolving in memory.

In hindsight, the shortest honest summary is:

- the blackout looked like a rendering pass problem
- it was really a state problem
- we kept touching the readers before we diffed the writers

Phase 26 is the first phase that really deserves to be called an
explanation. Everything before it was either necessary elimination or
useful but incomplete mitigation.
