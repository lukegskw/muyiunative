# Crash Bandicoot 3 - Warped (Europe, Australia)

[All games](../README.md) · [Build a game app](../../README.md#build-from-the-launcher) · [Profile and input route](game.toml)

**Verified from reset through the title, opening cutscene and Time Twister hub.** The 14,400-frame route uses normal pad inputs and empty memory-card slots. No level completion, normal in-game saving, other region or revision has been verified.

| Field           | Value                                                              |
| --------------- | ------------------------------------------------------------------ |
| Region / serial | Europe, Australia / SCES-01420                                     |
| Boot executable | `SCES_014.20`                                                      |
| Boot SHA-256    | `69d1699f2792867055929bc6ff11ebdfe18a5d09c7974f0bf33f285578c738c3` |
| Native CPU      | 5,904,716,904 instructions; none interpreted                       |
| Lockstep        | 128,675,248 stops; native code agrees with the interpreter         |
| Native display  | 512×256 at the hub; hash `e324179600831c56`                        |

The complete recorded route also passes Metal 4× with native VRAM and texture/CLUT caches matching software at every frame. This is an automated correctness check, not a report that all visual or audible symptoms are gone. These fixes are included in preview 0.8.0; the earlier 0.7.0 launcher predates them.

The user reported acceptable software rendering, Metal audio cuts and flickering after starting the game on 2026-11-02. A 512×240 disjoint framebuffer copy took about 294 ms in the old single-lane 4× shader. The renderer now copies disjoint or identical regions in parallel, preserving the fine samples, masks and VRAM wrapping; other overlaps retain ordered row snapshots. Small triangles share ordered workgroups, and exact interpolation coefficients are prepared on the CPU. The host overlaps rendering with the following CPU frame to publish its audio before waiting for the preceding image.

[Development](../../docs/development.md#additional-games-and-metal-frame-delivery) records validation and audio-delivery measurements. On 2026-11-03, real AudioQueue output passes a warmed 600-field window at both 2× and 4× with no empty queue, source-sample mismatch or ring overflow. This checks delivery in that scene, not whole-game audible accuracy.

## Missing faces and spurious triangles

The user's 2026-11-03 video and paused images show large missing faces and long spurious triangles throughout the hub, with similar symptoms reported in the opening and levels. The user observed the problem at Metal 2×/4×. Local software snapshots also contain these large defects: native VRAM/cache equality missed a bug shared with the reference, while the earlier fine-only black-pixel trace concerned a different, small texture-edge sample.

An instruction trace finds an IRQ on RTPT followed by a BIOS return past the command, leaving the previous triangle's SXY coordinates in the GTE. The CPU dispatcher now issues an enabled GTE command at the interrupted PC before entering the handler, while retaining that PC in EPC. This implements [psx-spx, Interrupts vs GTE Commands](https://psx-spx.consoledev.net/dev/ps1/cpu/cpuspecifications/#interrupts-vs-gte-commands); there is no game-specific rendering rule, polygon suppression or timing adjustment.

Synthetic CPU and generated-native regressions fail with the old dispatcher and pass with the correction. In 120 consecutive hub fields at both 2× and 4×, the static background no longer has the missing faces or spurious triangles: its maximum transient black-pixel count at native samples is zero when the animated character and gears are excluded. Every field also passes native image-sample and VRAM/cache comparisons. The full 14,400-field route passes again, entirely native and in lockstep, with 4× display hash `40bcd031370c01b6`. The local app is rebuilt and signed; these changes are included in preview 0.8.0. The user subsequently reported that Crash appears to be working after this correction. No completed later level is claimed as verified.

## Quick states and audio

On 2026-11-03 the user confirmed saving/loading quick states and, after the playback correction, normal audio following a save. The production AudioQueue adapter also passes 720-field 2×/4× windows covering successful and failed state operations, with no source-sample mismatches, ring drops or queue errors. See [audio-after-save validation](../../docs/development.md#audio-after-saving-a-state). This does not establish normal memory-card save/load or whole-game audio accuracy.
