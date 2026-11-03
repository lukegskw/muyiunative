# Testing a game

Use this guide for a first compatibility report. A useful initial pass is 10–15 minutes of play after building, plus a comparison of the same scene in different renderers. You do not need to finish a game to contribute.

## Before starting

You need an Apple-silicon Mac with macOS 14 or later. An M1 MacBook Pro with 16 GB RAM is within the supported hardware requirements; performance on each game still needs testing. Build from your own `.cue`/`.bin` disc backup using the [Mac preview](https://github.com/lukegskw/muyiunative/releases/tag/v0.8.0) and [launcher instructions](../README.md#build-from-the-launcher). Building needs Apple's free Command Line Tools; playing the built app does not. OpenBIOS is included.

Record the version, build number and commit from **About muyiunative…** in the game app. The source checkout and README gallery can contain changes newer than the published preview. Keep the exact build with every result; retesting an older download does not verify a newer source fix. Check the [game notes](../profiles/README.md) for previously tested scenes and known issues.

## A first pass

1. **Build and boot.** Note whether the build succeeds, whether the title/menu appears and whether New Game reaches a controllable scene. If any step fails, report that result with the relevant log; a build failure is useful evidence too.
2. **Start with Software** in the Graphics menu. Play through the opening and some gameplay. Note the level or scene reached, approximate time played, cutscenes, graphics, sound, input and any crash, hang or slowdown.
3. **Compare Metal 1× in the same scene**, then try **2× and 4×** if playable. Record each result separately. Pay attention to missing faces, flicker, moving static objects, audio cuts and slow motion. If a fault appears, say which modes reproduce it and whether switching back changes it.
4. **Check your input method.** Keyboard only is fine. For a controller, include its model, USB/Bluetooth connection and player port. Mark a second controller or connection type as untested unless you tried it.
5. **Try normal saving if you reach a save point.** Save through the game's own menu, close the app, reopen it and load that memory-card save. Otherwise record saving as not tested. Quick states, if present in your build, are a separate feature and should be reported separately.
6. **Report exactly what you verified.** A title screen is a boot result; a short gameplay session is “Playable, not finished.” Choose “Finished” only after personally completing the game in the reported build. For performance observations, include the Mac chip, RAM and macOS version; mention battery/power or heavy background work if it affected the run.

You can paste this comparison into the report's **What was checked** field:

```text
Time played and furthest scene/level:
Software:
Metal 1x:
Metal 2x:
Metal 4x:
Normal memory-card save/load:
Steps to reproduce any problem:
```

Use “not tested” where appropriate. Useful reproduction steps name the scene, actions leading to the problem, whether it happens every time and the renderer/scale. FPS figures are optional; describe visible stutter or audio interruptions if you have no measurement.

## Where to report

Use the [compatibility report](https://github.com/lukegskw/muyiunative/issues/new?template=compatibility_report.yml) for each game and disc revision, including build failures. It collects the serial, region, boot executable hash, app build, device, renderer, controller and tested scope. The form explains how to find the hash in the built app without a terminal; enter “unavailable” if building failed before an app was created. Add a retest to an existing matching game/revision report when possible, with the new build clearly identified.

For a specific reproducible defect, **Report a Problem…** in the app opens a [bug report](https://github.com/lukegskw/muyiunative/issues/new?template=bug_report.yml) with build/device details filled in and reveals the run log. Keep detailed reproduction and logs in one issue and link related compatibility reports. Leave **Include Game Debug Output** off for the baseline unless more detail is requested.

Review logs for personal information and game data before sharing them. Send observations, identifiers, hashes and relevant reviewed logs. Keep disc images, BIOS dumps, built game apps, generated code, memory cards, quick states and extracted game data on your own Mac. See [Contributing](../CONTRIBUTING.md#reporting-a-problem) for reporting details and [Security](../SECURITY.md) for private vulnerability reports.
