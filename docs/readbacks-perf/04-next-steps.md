## Where to go from here — practical options

Phase C closed with a stronger conclusion than expected (Precise
is viable for the games we tested on this hardware), but also
with a hard wall (we can't validate on AMD / mid-range / Apple).
The user's observation that "5090 brute-forcing through it" is
the correct framing — the 7.23ms/f Bloodborne tax is real, just
absorbed by GPU headroom.

Given that constraint, here are four concrete things we could do
that are **actually practical** (i.e. don't require hardware we
don't have or assumptions about behaviour on gear we can't
measure).

### Option 1 — Ship the harness upstream as a standalone PR

**What:** `readback_metrics.{h,cpp}` + instrumentation hooks +
bench scripts, behind `SHADPS4_READBACKS_METRICS=<csv>` env var.
Zero-cost when unset.

**Why it's practical:**
- No correctness risk — it's pure instrumentation.
- Fills a gap every prior readback PR has had (#3404, #4085,
  #4120 all argued from hand-measured numbers with different
  methodologies).
- Gives future contributors a shared A/B framework so the next
  "Precise vs Relaxed" debate has real numbers attached.
- Small and self-contained. Easy for maintainers to review and
  merge.

**Risk:** Maintainers may prefer Tracy zones over a custom CSV.
Mitigation: we can add Tracy-only wrappers in parallel; the CSV
path stays opt-in.

**Effort:** ~2 hours to polish, upstream-fork the commit, open
PR. Pure downside-limited contribution.

### Option 2 — Phase D (deferred readback batching) in this fork

**What:** New `GpuReadbacksMode::Batched` that coalesces
`ReadMemory` calls within a fault window into one
`Scheduler::Finish` per PM4 sync packet.

**Why it's practical on this hardware:** We can validate the
mechanism. On the 5090, if batching collapses Bloodborne gameplay
from 106 Finish/f to ≤2, and p99 stays at 50ms, we've proven the
architecture works regardless of per-Finish-wait cost. That
proof point is valid even if we can't test the downstream
hardware impact ourselves.

**Why it's durable:** Batching eliminates the hardware
dependency. If the per-Finish wait is 68µs on 5090 and 200µs on
AMD RDNA 3, one batched Finish costs roughly the same on both.
This is the only change on the table that makes readbacks
hardware-agnostic.

**Risks:**
- Correctness. Ordering guarantees: if the guest reads a range
  the batch hasn't flushed yet, we must force-flush before the
  read. That's the same correctness trap #3404 fell into
  (Uncharted 3 gray characters). Needs careful testing.
- AMD / mid-range validation still missing — we can prove
  "batching collapses the count" but not "batching fixes the
  AMD regression" without the hardware.
- Implementation effort: ~2-4 days of careful work across
  `buffer_cache.cpp`, `liverpool.cpp`, `memory_tracker.h`.

**Pre-flight safeguard:** Open a discussion / draft PR with the
design **before** coding. PR #3404 sat stalled because it was
written as a full patch and then blocked on correctness review.
Getting maintainer sign-off on the approach first avoids that.

### Option 3 — Open an upstream GitHub discussion

**What:** A single issue / discussion titled something like
"Measurement framework for readbacks + single-hardware data
points" that dumps the Phase A-C findings, cites the research
dossier, and asks specifically:

- Does the maintainer team want the harness landed? (Option 1.)
- If we land batching (Option 2), would they merge it?
- Are there known contributors with AMD hardware who would run
  the same bench script against their setup?

**Why it's practical:** Zero implementation risk, gets maintainer
alignment early, and potentially unblocks AMD validation via the
community rather than us trying to guess.

**Effort:** 1 hour to write, screenshot the table, link the
dossier.

### Option 4 — Reproduce upstream's specific "Bloodborne hang"

**What:** Try to hit the specific repro in issue #3826 (teleport
a few times, game hangs on next load). We didn't trigger it in
our 3.7-min gameplay cell. If we can reproduce it, we have a
concrete correctness blocker to fix. If we can't, that's also
data: "not reproducible on Nvidia with latest master."

**Why it's practical:** Narrow, bounded, gives a definitive
yes/no data point either way.

**Effort:** 15-30 min of Bloodborne gameplay, if we have a save
that lets us teleport quickly.

### Recommendation

**Do Option 3 first** — open the discussion with the data we
already have. That's free and it sets expectations with the
maintainers. Based on their response:

- If they say "yes, we want the harness" → do Option 1 next
  (small, high-confidence merge).
- If they say "batching is welcome but show us the numbers" →
  do Option 2 with a draft PR.
- If they say "the AMD path is the blocker, find someone with
  hardware" → park the perf work and focus on other fronts.

Option 4 can run in parallel any time — it's just gameplay.

Option 1 can also run before Option 3 if we want a concrete
artefact to attach to the discussion.

### What we should NOT do

- **Flip the global default.** Not defensible on our hardware
  alone. Don't open a PR that does this.
- **Remove Relaxed mode.** Even though we found no advantage,
  removing it affects users on hardware we don't have.
- **Chase fence-detection (#3404)** as a first move. That's
  higher risk than batching and was blocked on correctness. If
  batching lands, #3404 becomes incremental polish.
- **Claim "Precise is safe as default on Nvidia"** without
  saying we only tested two games on one GPU. The claim is true
  but easy to overreach on.
