# Final Fantasy IX

[All games](../README.md) · [Build a game app](../../README.md#build-from-the-launcher) · [Profile and input route](game.toml)

**Verified from reset through the first battle in the cargo ship.** The 24,720-frame route includes New Game, the opening movie, lighting the candle and the first encounter. It uses normal pad inputs and empty memory-card slots. Later scenes, the rest of Disc 1, disc changes and save/load have not been verified.

| Field           | Value                                                              |
| --------------- | ------------------------------------------------------------------ |
| Region / serial | USA / SLUS-01251                                                   |
| Boot executable | `SLUS_012.51`                                                      |
| Boot SHA-256    | `e30e40745d079aed7071c130785fb42406d8857bdf5484a101ec9baed143ee1c` |
| Native CPU      | 7,904,116,658 instructions; none interpreted                       |
| Lockstep        | 177,178,253 stops; native code agrees with the interpreter         |
| Native display  | 320×218 at the recorded battle frame; hash `1f8773f9f941218a`      |

The complete recorded route also passes Metal 4× with native VRAM and texture/CLUT caches matching software at every frame. This is an automated correctness check, not a report that all visual or audible symptoms are gone. The published 0.7.0 launcher predates these source changes.

The user reported acceptable software rendering, Metal audio cuts and trembling objects in this battle on 2026-11-02. Investigation found that the Metal renderer classified a whole texture page as feedback even when the requested UVs were disjoint from the destination. In one busy battle frame, 98 suspected feedback primitives reduce to two actual possible aliases. The renderer now includes the requested UV range, texture windows, wrapping, complete fetched cache lines and the full palette when proving independence. Disjoint draws preserve per-sample geometry and mask behavior; actual feedback retains ordered native results. Integer PS1 vertex coordinates and affine textures remain visible at higher resolutions; this is not a subpixel geometry reconstruction system.

The host also overlaps two submitted Metal frames with following CPU frames, so the next SPU samples can be published while rendering is still running. Headless comparisons remain synchronous. [Development](../../docs/development.md#additional-games-and-metal-frame-delivery) records validation, measured scope and remaining audio limitations. The static crate reproduction and correction are recorded below; a user check of the rebuilt app remains pending. Matching native VRAM alone does not establish perceptual smoothness.

On 2026-11-03, real AudioQueue output also passes a warmed 600-field window at both 2× and 4× with no empty queue, source-sample mismatch or ring overflow. This checks delivery in that scene, not whole-game audible accuracy.

## Static crate jitter at higher resolution

On 2026-11-03 the user identified the crate behind the opponent on the left of the first battle as trembling from Metal 2× onward. Continuing the validation route with port 1 released reproduces a static native image but alternating fine samples. The texture alias test padded the triangle's UV range by one; at an 8-bit texture-page edge, padding U=127 to 128 invented a wrapped source read at x=0. Only the x=0 framebuffer then used native feedback blocks, while the x=320 framebuffer used fine geometry for the identical face.

The corrected alias bounds use the integer vertex extrema: over a legal 1023×511 triangle, truncating 12-bit gradients contributes less than 1534/4096 texel of error, so nearest rounding cannot leave that range. Actual window/cache-line/palette aliases remain guarded. A synthetic translated-buffer regression fails at 2×/4× before the correction and passes afterward. In a 100-field comparison after 40 warmup fields, the native region (x=[0,40), y=[60,115)) stays static; the number of changing fine pixels falls from 201 to zero at 2× and from 1,036 to zero at 4×. Native image samples and VRAM/cache checks remain unchanged. This corrects the measured crate oscillation; it does not claim to remove all PS1 geometry/texture movement or replace the separate CPU/GTE correction documented in [Crash's profile](../SCES-01420/README.md#missing-faces-and-spurious-triangles).

The complete 24,720-field route is rechecked after this correction at Metal 4×: native VRAM/cache agree with software every field, all 7,904,116,658 CPU instructions remain native, and the updated fine display hash is `e96e2c2a5c0ac791` at 1280×872. The rebuilt local app also passes a 60-field bundled-resource boot with reference verification and a valid hardened-runtime signature.

The route is also rechecked after the shared CPU/GTE interrupt correction on 2026-11-03: all instruction counts, lockstep stops and the fine display hash above remain unchanged, and the crate region remains static at 2×/4×. Both local apps include that runtime correction.

The local quick-state bookmark resumes this first battle at frame 24760 after 40 fields with port 1 released. Save/load preserves the immediate 4× image and reproduces 120 subsequent fields with identical CPU, RAM, SPU, GPU and fine-image state. The normal app menu and ⌘S/⌘L are also checked across app restart and a change to Metal 2×. The private state file is kept only in Application Support.
