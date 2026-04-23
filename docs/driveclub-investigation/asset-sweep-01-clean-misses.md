## Clean misses after the fixed race gate

With the hardened race-window guard in place, the following additional
families were probed and still did **not** materially disturb the
blackout itself:

1. Scheduler / timeline slowdown family
   - forcing `Finish()` around the race-start window made the game slow
     but left the blackout unchanged
   - this rules out the coarse "GPU is simply finishing too late"
     timing theory
2. `sceVideoOutAdjustColor` / host gamma family
   - stubbing out the gamma write and forcing presenter gamma to stay
     at `1.0` did not move the blackout
3. Host final-output family
   - bypassing host FSR and host post-process and presenting the raw
     guest video-out image still left the blackout intact
   - this is a clean miss against the "final overlay / final postfx"
     theory
4. Video-out label / flip-wait family
   - forcing VO labels ready and bypassing the VO wait path during the
     race window fell back to baseline behavior with the blackout still
     intact

Current clean takeaway:

- the blackout is upstream of host present
- it is not explained by `sceVideoOutAdjustColor`
- it is not explained by coarse GPU slowdown / forced finish
- it is not explained by the VO label / flip wait path

That means the remaining lead should move away from final output and
toward a game-side camera / postfx / prerace system or another shared
upstream branch.
