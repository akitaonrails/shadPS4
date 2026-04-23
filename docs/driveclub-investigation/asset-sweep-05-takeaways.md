## Asset-side takeaways so far

What is now load-bearing:

- track-local prerace/postfx assets are real and relevant
- Munnar-local patches change the image latched under blackout
- shared `globaldata` fade/transition content is a plausible master
  layer

What is now ruled out or deprioritized:

- the idea that the blackout is purely in host present, host gamma, or
  final output
- the idea that track-local camera `Fade` alone owns the blackout
- brute-force editing of `globaldata.rpk` as a safe runtime test path

Recommended direction from this point:

- keep `globaldata` at baseline while testing
- use track-local packs like Munnar to understand what content is
  carried under blackout
- treat `globaldata` as a shared transition/analysis target, not as a
  broad shotgun target
  values
