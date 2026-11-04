# WipEout 3 — Special Edition

Europe, SCES-02845, multilingual. [game.toml](game.toml) identifies the supplied 14-track disc and records an 18,000-field route from reset, with empty memory-card slots, through the menus into a Vector-class single race on Porto Kora.

This route was used to investigate a player's report that audio stuttered and stopped with Metal. The shared Mac playback correction suspends an exhausted output queue and refills its existing buffers before resuming. Measurements and their limits are recorded in [Development](../../docs/development.md#playback-recovery-after-metal-stalls).

The title-screen dots and thin lines at Metal 2×/4× were traced to neighboring glyphs in the texture atlas. The renderer now limits unused endpoint texels on rectilinear textured quads, retaining native texture/cache behavior and finer interior texture detail. The title route and corrected image hashes are recorded in the profile; [Development](../../docs/development.md#wipeout-title-screen-atlas-edges) records the regression checks.

On 2026-11-04 the user confirmed that the updated games were working. The remaining blockiness of this title screen's small lettering is present in the supplied correct reference: its small headings use four-texel-high bitmap strips, while copyright glyphs use eight-texel-high cells. Metal 2×/4× retains those source bitmaps; their low resolution is expected.

Reaching this race does not establish a completed lap or event. Broader visual accuracy remains unverified. Normal memory-card saving, controllers, other tracks and whole-game audible accuracy remain unverified for this disc. Local captures, compiled game code and checkpoints stay outside the checkout.
