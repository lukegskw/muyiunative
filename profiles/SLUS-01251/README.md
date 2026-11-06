# Final Fantasy IX

[All games](../README.md) · [Build a game app](../../README.md#build-from-the-launcher) · [Profile and input route](game.toml)

**Verified from reset through the first battle in the cargo ship.** The 24,720-frame route includes New Game, the opening movie, lighting the candle and the first encounter. It uses normal pad inputs and empty memory-card slots. Separately, [all three requested disc changes](#requested-disc-changes) pass when loading later-disc memory-card saves. The rest of the story, its end-of-disc handoffs and normal in-game saving have not been verified.

The source launcher can [build the complete disc set into one app](../../README.md#multi-disc-games): Disc 1 first, followed by Discs 2–4, selected together or listed in an M3U. Every disc's boot executable and captured route contribute to the same native binary; the Game menu selects bundled discs without building another app or changing memory cards. The build and controller paths pass synthetic four-disc tests, and the user's four-disc app passes the requested swaps described below. This profile's capture route still describes Disc 1 only; other discs use their own profile if available, otherwise the default capture route. Unseen code uses the interpreter.

| Field           | Value                                                              |
| --------------- | ------------------------------------------------------------------ |
| Region / serial | USA / SLUS-01251                                                   |
| Boot executable | `SLUS_012.51`                                                      |
| Boot SHA-256    | `e30e40745d079aed7071c130785fb42406d8857bdf5484a101ec9baed143ee1c` |
| Native CPU      | 7,904,116,658 instructions; none interpreted                       |
| Lockstep        | 177,178,253 stops; native code agrees with the interpreter         |
| Native display  | 320×218 at the recorded battle frame; hash `1f8773f9f941218a`      |

The complete recorded route also passes Metal 4× with native VRAM and texture/CLUT caches matching software at every frame. This is an automated correctness check, not a report that all visual or audible symptoms are gone. These fixes are included in preview 0.8.0; the earlier 0.7.0 launcher predates them.

The user reported acceptable software rendering, Metal audio cuts and trembling objects in this battle on 2026-11-02. Investigation found that the Metal renderer classified a whole texture page as feedback even when the requested UVs were disjoint from the destination. In one busy battle frame, 98 suspected feedback primitives reduce to two actual possible aliases. The renderer now includes the requested UV range, texture windows, wrapping, complete fetched cache lines and the full palette when proving independence. Disjoint draws preserve per-sample geometry and mask behavior; actual feedback retains ordered native results. Integer PS1 vertex coordinates and affine textures remain visible at higher resolutions; this is not a subpixel geometry reconstruction system.

The host also overlaps two submitted Metal frames with following CPU frames, so the next SPU samples can be published while rendering is still running. Headless comparisons remain synchronous. [Development](../../docs/development.md#additional-games-and-metal-frame-delivery) records validation, measured scope and remaining audio limitations. The static crate reproduction and correction are recorded below; a user check of the rebuilt app remains pending. Matching native VRAM alone does not establish perceptual smoothness.

On 2026-11-03, real AudioQueue output also passes a warmed 600-field window at both 2× and 4× with no empty queue, source-sample mismatch or ring overflow. This checks delivery in that scene, not whole-game audible accuracy.

## Static crate jitter at higher resolution

On 2026-11-03 the user identified the crate behind the opponent on the left of the first battle as trembling from Metal 2× onward. Continuing the validation route with port 1 released reproduces a static native image but alternating fine samples. The texture alias test padded the triangle's UV range by one; at an 8-bit texture-page edge, padding U=127 to 128 invented a wrapped source read at x=0. Only the x=0 framebuffer then used native feedback blocks, while the x=320 framebuffer used fine geometry for the identical face.

The corrected alias bounds use the integer vertex extrema: over a legal 1023×511 triangle, truncating 12-bit gradients contributes less than 1534/4096 texel of error, so nearest rounding cannot leave that range. Actual window/cache-line/palette aliases remain guarded. A synthetic translated-buffer regression fails at 2×/4× before the correction and passes afterward. In a 100-field comparison after 40 warmup fields, the native region (x=[0,40), y=[60,115)) stays static; the number of changing fine pixels falls from 201 to zero at 2× and from 1,036 to zero at 4×. Native image samples and VRAM/cache checks remain unchanged. This corrects the measured crate oscillation; it does not claim to remove all PS1 geometry/texture movement or replace the separate CPU/GTE correction documented in [Crash's profile](../SCES-01420/README.md#missing-faces-and-spurious-triangles).

The complete 24,720-field route is rechecked after this correction at Metal 4×: native VRAM/cache agree with software every field, all 7,904,116,658 CPU instructions remain native, and the updated fine display hash is `e96e2c2a5c0ac791` at 1280×872. The rebuilt local app also passes a 60-field bundled-resource boot with reference verification and a valid hardened-runtime signature.

The route is also rechecked after the shared CPU/GTE interrupt correction on 2026-11-03: all instruction counts, lockstep stops and the fine display hash above remain unchanged, and the crate region remains static at 2×/4×. Both local apps include that runtime correction.

The local quick-state bookmark resumes this first battle at frame 24760 after 40 fields with port 1 released. Save/load preserves the immediate 4× image and reproduces 120 subsequent fields with identical CPU, RAM, SPU, GPU and fine-image state. The normal app menu and ⌘S/⌘L are also checked across app restart and a change to Metal 2×. The private state file is kept only in Application Support.

## Quick states and audio

On 2026-11-03 the user confirmed saving/loading quick states and, after the playback correction, normal audio following a save. The production AudioQueue adapter also passes 720-field 2×/4× windows covering successful and failed state operations, with no source-sample mismatches, ring drops or queue errors. See [audio-after-save validation](../../docs/development.md#audio-after-saving-a-state). This does not establish normal memory-card save/load or whole-game audio accuracy.

## Requested disc changes

Local checks on 2026-11-05–06 use the user's four-disc app, preview 0.8.1 build 77 based on `f5b88a6f7c09` plus the uncommitted multi-disc changes. The supplied executable SHA-256 is `d02b313486f72c4b3f78e9c7cf0a65dfc327e7530d149bdd51fe6bbb14699bbf`. Discs 1–4 are USA SLUS-01251, SLUS-01295, SLUS-01296 and SLUS-01297. A separately signed copy uses identical compiled code and track contents, but a separate bundle ID and save directory. Executable differences are confined to the signature and its size fields. The original app and player saves are preserved.

Normal NTSC memory cards from [FantasyAnime's FFIX saves](https://fantasyanime.com/finalfantasy/ff9/ff9saves.htm), saves 15, 32 and 49, provide later-disc progress without playing the whole story. Loading each save on the preceding disc reaches FFIX's own “Change to DISC” screen. Only then does the test use **Game → Open Disc Cover**, followed by **Insert Disc** in the same running app.

| Request | Result after inserting the requested disc |
| ------- | ---------------------------------------- |
| Disc 1 → 2 | South Gate / Rest Stop loads; Dagger responds to movement. An initial wrong Disc 3 is rejected and the Disc 2 request returns. |
| Disc 2 → 3 | Alexandria arrival cutscene loads; Cross advances dialogue. |
| Disc 3 → 4 | Black Mage Village cutscene loads; Cross advances dialogue. |

The first request is reached from native Disc 1 reset. The later requests are initially prepared by booting the correct source disc in the reference interpreter, loading the normal card, and saving a compatible console state; restoring that state in the native app with identical compiled code precedes the menu swap. Independent 10,500-frame runs of the supplied native app then verify both preparations from reset in lockstep: Disc 2 agrees at 70,868,129 stops (99.79% native, display hash `534f77fd0e626432`), and Disc 3 at 70,867,736 stops (99.76% native, `96179efe711d3ee5`). Their final images show the Disc 3 and Disc 4 requests respectively. The interactive swaps themselves are not lockstep runs.

A separate premature swap, made on the memory-card selection menu before a disc request, hangs with repeating audio. Both native execution and an interpreter-only replay reach the same BIOS fatal loop with EPC `801f65a4`. The faulting data occurs one raw sector later on Disc 2 than on Disc 1, consistent with the game retaining file addresses from the previous disc. This is a recorded limitation, not a hardware-verified diagnosis or a runtime fix; the successful tests wait for the game's request.

Quick states before and after each swap, matching cards, provenance and local pictures stay outside the repository. The Disc 1 → 2 state also passes an app restart and restore, leaving the isolated app at the request for an interactive retest. These checks establish memory-card loading and requested media changes on these routes; story-ending handoffs, normal card writes, whole-game coverage and whole-game audio remain unverified.

The user confirmed the swap, then reported that restoring a state from another disc required manual insertion. The host now locates the state's exact disc inside the app before applying it, retaining format-2 compatibility. On 2026-11-06 the updated app resumes actual Disc 2, 3 and 4 states after ordinary Disc 1 launches using only ⌘L; movement or dialogue responds afterward. Restoring the original Disc 1 checkpoint while Disc 2 is mounted also passes. The four-disc rebuild agrees with the interpreter on all capture routes: 177,178,253 stops on Disc 1 and 14,309,615 / 14,310,340 / 14,309,968 on Discs 2–4. The updated Downloads executable SHA-256 is `8709b486f3b70af8d9891e6aebbe439b2fd1fccae8eba7f5c295ca0b4a123874`. Both old app bundles are backed up externally, the original player's save hashes are unchanged, and the isolated test slot now holds the Disc 2 state for a repeatable normal-launch test.
