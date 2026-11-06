The plan uses only permissively licensed parts in the app and its tools, builds everything on the Mac before the game ever runs, and puts the Mac first with iOS reusing the same runtime.

## 1. Approach

| Approach                                    | Description                                                                                                                                                                                                                                                               | Verdict                                         |
| ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------- |
| **A. Interpreter first, recompiler second** | Build the hardware runtime around a simple MIPS **interpreter** first: code that executes PS1 instructions one by one, like an emulator. Once games run correctly, add the recompiler. The interpreter remains as the safety net and as the reference to compare against. | ✅ **Recommended**                              |
| B. Recompiler first (the N64Recomp style)   | Translate to C immediately and build the runtime alongside it                                                                                                                                                                                                             | Hard to debug with nothing to compare against   |
| C. Replace the BIOS with our own code       | Skip the BIOS and implement its functions directly in C                                                                                                                                                                                                                   | Fragile across games; maybe later as a speed-up |

The end result of A and B is the same. A gets there in a safer order, which matters at a few hours a week.

## 2. Architecture

```mermaid
flowchart LR
  D[Your disc .cue/.bin] --> R[Disc reader]
  R --> P[Profile lookup<br/>serial + SHA-256]
  P --> A[Analyzer<br/>MIPS decoder]
  A --> G[Code generator<br/>MIPS → C]
  G --> C[clang]
  RT[C runtime<br/>CPU, GTE, GPU, SPU, CD, MDEC, pads] --> C
  C --> M[Mac .app]
  C --> I[iOS .ipa]
  H[Capture run on Mac<br/>interpreter] -->|code loaded from disc| A
```

**Components:**

- **Python tools**:
  - Disc reader: `.cue`/`.bin` and the CD's ISO9660 file system
  - Executable loader for the PS1's PS-X EXE format
  - Profile lookup
  - Analyzer: finds where functions start and end, and handles jump tables
  - Code generator: must handle MIPS **branch delay slots and load delay slots** (the instruction after a branch or load executes before its effect lands)
  - Capture runner and packager
- **C runtime**:
  - Memory map (2 MB RAM, 1 KB scratchpad, 512 KB BIOS)
  - CPU state and interrupt/exception handling (COP0)
  - Timers, DMA, a scheduler that counts CPU cycles
  - GTE (3D math), GPU (1 MB of video memory), SPU (sound)
  - CD-ROM drive (including XA audio and CD music)
  - MDEC (video decoder), controllers and memory cards
- **Lookup table**: maps an address to its native function; if none exists, the **interpreter** runs that code.
- **Graphics**: the software renderer keeps canonical console VRAM. An independent Metal command renderer matches it at native resolution and supplies 2×/4× geometry; the software renderer remains the reference and the default.
- **App shells**: AppKit/SwiftUI on Mac; UIKit with touch controls and Game Controller support on iOS.
- **BIOS**: OpenBIOS (MIT), bundled and itself recompiled.

**Core rule: code is never generated on the device.** Everything is compiled on the Mac at build time. That's what allows iOS at all, and it's compatible with Apple's hardened runtime on Mac.

## 3. Repository layout

```
muyiunative/
  tools/              Python: disc, exe, mips, analyze, codegen, overlays, replay, mac_build, pack_app; C: capture (what a program runs, at build time)
  scripts/            Run by the user: muyiunative (build an app), disc_info (identify a disc), native_game (native code checked in lockstep), survey_game (capture unknown games)
  runtime/            C: cpu, cpu_ops, interp, native, lockstep, capture, mem, gte, gpu_sw, render, spu, cdrom, mdec, sio, sched
  gfx/metal/          Metal command renderer, offline shaders and presentation
  shell/mac/  shell/ios/
  bios/openbios/      MIT, bundled
  profiles/<serial>/  game.toml: hash, overlays, function hints, patches
  tests/              homebrew test programs (no game data); unit tests sit beside their code (runtime/tests, shell/mac/tests, gfx/metal/tests, tools/tests)
  docs/               development: building, tests, CI, house style
```

## 4. Phases and exit criteria

Phases 0–7 have met their exit criteria. Phase 2's homebrew tests are the hardware test programs of [ps1-tests](tests/ps1-tests/README.md), which pass in CI, and its [first game](profiles/SLUS-01212/game.toml) reaches its title menu, checked again on 2026-10-30 with the runtime of commit a0d26af. [Development](docs/development.md#phase-2-hardware-breadth) records the checks and the remaining gaps. Phase 3's main executable, the first game's, runs as native code and agrees with the interpreter in lockstep through the same route, on 2026-10-30 with the runtime of commit 52cedd3 ([Development](docs/development.md#phase-3-the-recompiler)). Its performance profile found the time in a looping GPU DMA list executing without guest time passing. Phase 4 schedules those packets in guest time and preserves overwritten code versions by content hash. On 2026-10-30 the first game clears Mission 1 entirely natively and agrees with the interpreter through the results screen: 22,985 frames and 6,553,245,179 instructions, with normal pad inputs and five continues. [Development](docs/development.md#phase-4-overlays-and-profiles) records the route, the lockstep comparison and the remaining hardware-timing gaps. On 2026-10-31 phase 5 builds its signed, double-clickable Mac app from the disc, rechecking the same route in lockstep, with persistent cards in Application Support, keyboard remapping and a launcher. The tool tests also boot a synthetic app independently of its original input disc ([Development](docs/development.md#phase-5-the-mac-product)).

Phase 6 independently draws PS1 operations in Metal, with native VRAM and persistent texture/CLUT caches matching software throughout the first game's 22,985-frame first-level route. Offline shaders and the Graphics menu provide 1×/2×/4× geometry; software remains the canonical console model and default. The performance follow-up parallelizes independent textured rectangles/triangles and untextured triangles while preserving cache order, and models measured flat-rectangle occupancy so GPU DMA waits for readiness rather than flooding the renderer. On 2026-11-01, the full Metal 4× route on the M4 Pro takes 3.04 ms per frame on average, 5.95 ms at p95 and 11.46 ms at maximum, with zero frames over 16.7 ms versus 616 before. Both final image hashes are unchanged. All 6,553,267,283 CPU instructions run natively and agree with the interpreter at 161,859,326 lockstep stops; 4,083 GPU checks cover the parallel paths ([Development](docs/development.md#phase-6-the-metal-renderer)).

The host publishes produced sound before waiting for rendering and refills returned AudioQueue buffers only when a full block of real samples is ready, using the same three buffers and ring capacity. The exact adapter passes live playback checks in warmed title, transition and results windows: no empty queue, padded/missing samples or source changes, with at least two buffers always queued. The CPU-only buffering tests pass 3,969 checks. This completes the measured first-game performance follow-up. Phase 7 also meets its downloaded-launcher/controller exit criterion on 2026-11-02; iOS remains phase 8. Other GPU drawing costs, FIFO/bus accuracy, whole-route audible accuracy and broader game compatibility remain documented limitations.

On 2026-11-02 the user requested FF IX and Crash Bandicoot 3 - Warped. Their [profiles](profiles/README.md) now record native/interpreter agreement through the first battle and Time Twister hub respectively. Investigation also required general Metal copy, triangle/cache and frame-delivery changes, documented in [Development](docs/development.md#additional-games-and-metal-frame-delivery). This does not meet phase 9's “profile only” exit criterion, does not complete a new level, and does not change the iOS phase's status.

On 2026-11-04 the user requested multi-disc games as one self-contained app. The source launcher accepts an ordered disc set, captures/compiles/checks each disc and packages all images with one native binary and shared cards. The Game menu models the disc cover and selects bundled discs without another build. Quick-state loading locates its original disc inside the app automatically, including after a default Disc 1 launch. [Development](docs/development.md#multi-disc-changes) distinguishes synthetic four-disc validation and FFIX's requested swaps from unverified story-ending handoffs and whole-game coverage.

On 2026-11-03 the user confirmed the quick-state/audio correction, merged PR #26 and authorized preview 0.8.0. The release packages these changes for players; [release records](docs/releasing.md) identify its source and artifact. It adds no new full-game or phase-exit claim.

Phase 7 opens the project to players before iOS, under the name muyiunative (ps1native was its working name). Its parts, in this order: game controllers, wired and wireless, through Apple's GameController framework, with a second controller on port 2; a log file and a Report a Problem item that opens a prefilled issue with the app's version; building a game app from the launcher by choosing a `.cue` or `.bin`, with the shader library prebuilt and the CLT's Python plus MIT Tomli, so that only Apple's Command Line Tools are needed; then public-repository preparation: the preview's status and compatibility gallery in the README, versioned bug and compatibility forms, SECURITY.md with private vulnerability reporting, and CI checks ready to protect `main`. The selected release is a notarized Developer ID-signed launcher ZIP, tested privately before repository publication.

The controller part samples captured profiles on the machine thread at each frame boundary, combining the keyboard and first controller on port 1 and connecting the second controller on port 2. Plain C policy covers positional mapping, 0.5 stick/trigger thresholds, stable ports, hotplug and focus release. On 2026-11-02 the user confirmed Xbox Bluetooth input in the diagnostic and Metal Slug X with commit fdb7a6e. Wired and two-player hardware checks remain pending; the downloaded-launcher player check is recorded below ([Development](docs/development.md#phase-7-the-public-preview)).

The logging part gives every packed app version 0.7.0, its commit-count build number and short revision. About shows that identity, and Finder runs keep the last five per-app logs with home paths redacted. Report a Problem reveals the log and opens the bug form with version, commit, Mac/macOS, game serial and renderer filled through their field IDs.

The launcher build part carries a signed source kit and the precompiled metallib, uses CLT Python 3.9 with MIT Tomli, offers Save As and shows a live cancellable build log. Builds copy the kit to a disposable cache outside the app and checkout; version identity comes from its JSON manifest. The same GUI entry builds a synthetic app headlessly with only CLT and no Metal compilation, then replays its bundled disc in lockstep ([Development](docs/development.md#phase-7-the-public-preview)). The user accepted PR #22 on 2026-11-02; it merged as 64ed085.

On 2026-11-06 the user requested GitHub release notifications and updating the launcher from inside the app, with game rebuilds remaining manual. The launcher integrates Sparkle with signed update archives and feeds; release publication updates a dedicated metadata branch without distributing game data. [Releasing](docs/releasing.md) describes key custody, packaging and feed publication.

Public-repository preparation documents the preview's limits, installation/build flow and the single verified game, with a compatibility-report form and [private security reporting policy](SECURITY.md). The [pre-publication history audit](docs/public-preview-audit.md) covers all 63 commits and 494 file blobs reachable at 64ed085, including pull-request heads: no game data, credential candidates or disallowed author/committer emails found; all 19 baseline binary objects have documented OpenBIOS/homebrew provenance. The four intended required CI checks now run on every PR. The downloaded-launcher check succeeded and the user authorized public visibility on 2026-11-02; the repository security configuration and final audit are recorded in [the publication review](docs/public-preview-audit.md).

The user requested an nxnative-style compatibility gallery and approved selected actual gameplay screenshots on 2026-11-02. The README's image/title links open per-game notes beside the profile, with a [game index](profiles/README.md) and [capture provenance](docs/images/README.md). This changes presentation, not the verified game scope. Routine comparison images still stay local; gallery images are a narrow exception, never test inputs or build-kit resources.

The user merged public-readiness PR #23 as a73d7ff and selected a Developer ID-signed, notarized ZIP for preview 0.7.0 on 2026-11-02. The repository remains private until the downloaded-launcher player check succeeds. [Release tooling](docs/releasing.md) prepares the generic launcher with local keychain credentials, staples its accepted notarization ticket, then checks an extracted archive before writing the download/checksum. Finder logs now retain at most 10 MiB per run, preserving their identity header and recent records; guest debug text is opt-in for one session. On 2026-11-02 the real 0.7.0 (build 64) candidate from commit 4813e3b79adb is accepted by Apple's notary service, and its extracted copy passes signature, ticket, Gatekeeper, kit and boot checks. The final build 66 release from a6c39675c4b3 was published and passed the downloaded-launcher player check, recorded below.

On 2026-11-02 the user confirmed the final player check with the downloaded 0.7.0 (build 66) launcher, source `a6c39675c4b3541b777efb0dde9aa3a5c0030b2d`: build Metal Slug X, SLUS-01212, through the launcher without a terminal, open its generated app and play with the Xbox controller. The app metadata and build log identify the same release source. This meets phase 7’s exit criterion; it does not claim any new scene completion beyond the existing Mission 1 evidence. The user then authorized public visibility and requested repository description/topics ([Development](docs/development.md#phase-7-the-public-preview)).

| #   | Phase                                                                                                                                                                                                 | Done when                                                                                   |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| 0   | **Foundations**: repository, MIT license, CI (macOS GitHub Actions), tool and runtime skeletons                                                                                                       | CI builds an empty `.app`                                                                   |
| 1   | **Interpreter + boot**: R3000A interpreter, memory, COP0, interrupts, timers, DMA, minimal software GPU                                                                                               | **OpenBIOS shell shows in a Mac window**                                                    |
| 2   | **Hardware breadth**: CD-ROM, full software GPU, SPU, GTE, MDEC, pads, memory cards                                                                                                                   | Homebrew CPU/GTE/GPU tests pass; **first game reaches its title screen** on the interpreter |
| 3   | **Recompiler v1**: exe loader, function discovery, MIPS→C, cycle counting, recompiled OpenBIOS                                                                                                        | Main executable runs native and **matches the interpreter** in lockstep comparison          |
| 4   | **Overlays and profiles**: capture runs on Mac, overlay compilation keyed by content hash, a check that the RAM still holds the expected bytes before running native code, a survey for unknown games | First game mostly native and playable through its first level                               |
| 5   | **Mac product**: `muyiunative build disc.cue --platform mac-app`, launcher, saves in Application Support, keyboard remapping                                                                          | A double-clickable `.app` built from your disc                                              |
| 6   | **Metal renderer**: hardware rendering and higher resolution                                                                                                                                          | Matches the software renderer frame by frame                                                |
| 7   | **Public preview (Mac)**: the name muyiunative, game controllers, a log and problem reports, building a game app from the launcher without a terminal, a public repository                            | A player builds a game app without a terminal and **plays it with a controller**            |
| 8   | **iOS**: same runtime, touch controls, generated Xcode project signed with your developer account                                                                                                     | **First PS1 game running natively on your iPhone**                                          |
| 9   | **More games**: profiles 2 and 3, chosen from different engines                                                                                                                                       | Each new game needs only a profile, no runtime changes                                      |

## 5. Main risks

| Risk                                                                      | Mitigation                                                                                     |
| ------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| Code loaded from disc during play, or code that modifies itself           | Capture runs on Mac; native code runs only after a hash check; the interpreter covers the rest |
| Timing (CD-ROM, VSync, sound interrupts)                                  | Count cycles per block of code; compare against the interpreter                                |
| GPU accuracy (semi-transparency, VRAM copies, texture palettes, mask bit) | Software renderer as reference; Metal renderer comes later                                     |
| No symbol names in the game binaries                                      | Analysis heuristics plus calls observed during capture runs; hints in each profile             |
| iOS limits (no runtime code generation, memory limits)                    | Already designed for: everything compiled ahead of time, interpreter fallback                  |
| Legal                                                                     | Clean-room, permissive code only in the app and tools, no disc-derived data in the repo        |

## 6. Testing

- **Instruction tests**: each MIPS instruction checked in recompiled form against the interpreter.
- **Homebrew test programs in CI**: built with homebrew SDKs; check each license.
- **Lockstep comparison**: run recompiled code and the interpreter side by side and compare registers and memory after each call.
- **Frame hashes**: screenshots stay local, only hashes go in the repo. An external emulator can be run separately for comparison (black box, never linked).

## 7. Choosing the first game

- **2D**, with **a single executable** and few disc-loaded code sections
- **No LibCrypt**: an anti-copy protection used on many European (PAL) discs
- Little video playback
- Puzzle games are ideal, e.g. Puzzle Bobble / Bust-a-Move, Tetris variants, Mr. Driller

Dump it with an accurate ripping tool (e.g. redumper) to `.cue`/`.bin`.

## 8. Decision log

| Decision                                                                                          | Alternatives                            | Why                                                                                                                                                               |
| ------------------------------------------------------------------------------------------------- | --------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| MIT license                                                                                       | GPL-3.0                                 | Maximum reuse; independence                                                                                                                                       |
| Mac first, then iOS                                                                               | iOS first                               | Mac is the development and debugging machine                                                                                                                      |
| Python tools, C runtime, Metal                                                                    | Rust, C++                               | Fast iteration; C runs everywhere, including iOS                                                                                                                  |
| Interpreter first                                                                                 | Recompiler first                        | Gives a reference to compare against and a permanent fallback                                                                                                     |
| Software GPU first                                                                                | Metal first                             | Correctness before speed                                                                                                                                          |
| No code generation on the device                                                                  | Compile on the device (as 2Xtreme does) | Required for iOS; works with Apple's hardened runtime                                                                                                             |
| OpenBIOS bundled                                                                                  | Retail BIOS required                    | Legal, and no BIOS dump needed                                                                                                                                    |
| Clean-room, permissive dependencies only                                                          | Fork psxrecomp                          | Independence and license freedom                                                                                                                                  |
| Conventions from nxnative (MIT): build, front end, tools, rules for contributors and agents       | Own conventions                         | A working project of the same shape: native games on Apple platforms                                                                                              |
| Plain Makefiles and clang; Python scripts pack the apps                                           | CMake, an Xcode project, SwiftPM        | Nothing to install beyond Xcode and Python; the iOS project is generated (XcodeGen) in phase 8                                                                    |
| Objective-C with AppKit for the Mac front end                                                     | Swift, SwiftUI                          | Calls the C runtime directly, as nxnative's host does                                                                                                             |
| Python tools: standard library, unittest, ruff (bug rules) and pyright; not a package             | pytest, a formatter, a pip package      | They run in place, with no install                                                                                                                                |
| Apple silicon, macOS 14 or later                                                                  | Intel Macs, universal binaries          | One architecture to build and test, as on GitHub's runners                                                                                                        |
| OpenBIOS committed as a binary, built from source (its commit and hash in bios/openbios)          | Build it from a submodule; download it  | Building it needs Docker, beyond Xcode and Python; CI's macOS runners have none                                                                                   |
| [Metal Slug X, SLUS-01212](profiles/SLUS-01212/game.toml) as the first game                       | The puzzle games suggested above        | User's choice; its dump stays outside the repository                                                                                                              |
| [ps1-tests](tests/ps1-tests/README.md) (MIT) as the homebrew test programs, as released           | Download or build them; write our own   | The console's logs fit these builds; an exception to permissive-only: PSn00bSDK is MPL-2.0                                                                        |
| A disc's licence string (sector 4) stands in for the SCEx string a BIN lacks                      | Every disc unlicensed, or all accepted  | Games read their region from GetID; an unlicensed disc is refused, as a console does                                                                              |
| Own MIPS decoder in the Python tools, tested against the interpreter                              | rabbitizer (MIT), installed with pip    | Nothing to install beyond Xcode and Python; MIPS I is small, and the interpreter is its check                                                                     |
| Cycles per instruction: 1 from the cache, uncached fetches and loads by region, MULT/DIV          | An average per instruction; cache model | Lockstep needs exact counts; ps1-tests measures each cost; a cache costs native code dearly                                                                       |
| Interrupts taken at the end of a block of code, in the interpreter and in native code             | Between any two instructions            | Both sides see each interrupt at the same point, so the lockstep comparison is exact                                                                              |
| Generated C only in build folders, never in the repository, OpenBIOS's included                   | Commit OpenBIOS's generated C           | A game's code is disc data; OpenBIOS's is rebuilt from its committed binary in each build                                                                         |
| Keyboard: WASD + IJKL, Q/E, 1/3, Enter and Backspace; remapping in phase 5                        | Arrows + Z/X/A/S; a numeric keypad      | User's choice for a two-hand keyboard layout without a numeric keypad                                                                                             |
| Metal comparisons in `gfx/metal/tests`, in CI and locally; skip exit 77 without a device          | CPU-only tests; local-only GPU checks   | Exception to GPU-free tests: synthetic input, real renderer; CI skips absent Metal devices                                                                        |
| Offline metallib required by every host build, bundled by `make app`                              | `newLibraryWithSource:` at run time     | No shader source compiled on device; cost: Xcode's Metal Toolchain download, even for software                                                                    |
| Mac host tests in `shell/mac/tests`, plain and under ASan/UBSan in CI                             | Keep them in `runtime/tests`            | Tests beside their code; keyboard, replay, audio and saves need no GPU or Apple frameworks                                                                        |
| muyiunative as the project's name, replacing the working name ps1native                           | battonative, hitokiNative, inunative    | User's choice; unused on GitHub and the web, and recalls no trademark, franchise or game                                                                          |
| A public preview on the Mac (phase 7) before iOS                                                  | iOS first; stay private until iOS       | User's choice: feedback from players on their own discs and Macs before iOS reuses the runtime                                                                    |
| The preview builds game apps; no mode that plays a disc directly                                  | Open a disc and play, as emulators do   | User's choice: native code compiled on the Mac is the premise; direct play is emulation                                                                           |
| Controllers: [positional mapping](README.md#controllers), 0.5 stick/trigger thresholds            | Printed letters; other thresholds       | User approved 2026-11-02: positions match the PS1 layout; analog inputs drive digital pads                                                                        |
| App version 0.7.0; build = commit count; short revision in logs and About                         | Another version scheme                  | User approved 2026-11-02: identify each preview build and trace it to its source                                                                                  |
| UTF-8 logs in `~/Library/Logs/muyiunative/<bundle id>/`, retaining the last five runs             | One overwritten log; another location   | User approved 2026-11-02: separate apps and recent runs, with home paths replaced by `~`                                                                          |
| Player builds use CLT Python 3.9+ and vendored Tomli 2.2.1 (MIT)                                  | Bundle portable CPython 3.12.16         | User approved 2026-11-02 after measurement: equivalent synthetic build time; avoids 55 MiB and 11 signed Python binaries                                          |
| Signed launcher kit is a folder in `Contents/Resources/BuildKit`, with a JSON manifest            | ZIP extracted into the cache            | User approved 2026-11-02: sources and licenses are inspectable; version identity needs no Git                                                                     |
| Save As for each game app, initially in `~/Applications/muyiunative/`                             | Fixed output directory                  | User approved 2026-11-02: the player chooses the app's name and location                                                                                          |
| Temporary builds in `~/Library/Caches/muyiunative/Builds/`, removed on success, failure or cancel | Retain build files; another directory   | User approved 2026-11-02: generated game data stays outside the checkout; run log is retained                                                                     |
| Player builds reuse the launcher's offline metallib; source builds still compile it               | Require Metal Toolchain for players     | User's phase-7 requirement: supersedes “Offline metallib required by every host build” for player builds; only CLT is needed                                      |
| Compatibility gallery with actual gameplay screenshots and per-game notes beside profiles         | Text-only table; public cover artwork   | User requested nxnative's presentation and approved the selected gallery-screenshot exception on 2026-11-02; provenance is recorded, comparison images stay local |
| Preview 0.7.0 as a Developer ID-signed, Apple-notarized launcher ZIP                              | Ad-hoc ZIP; source only                 | User approved 2026-11-02 and has paid Apple Developer membership; the download identifies its developer and carries a notarization ticket                         |
| Keep the repository private until the downloaded-launcher player check succeeds                   | Publish before the release check        | User decided 2026-11-02; public visibility and repository security settings still need an explicit go afterward                                                   |
| Finder logs capped at 10 MiB per run, keeping the header and newest records, five runs            | No per-file cap                         | User approved 2026-11-02: bound per-app log files at about 50 MiB and mark omitted older output                                                                   |
| Finder guest debug output is opt-in for the current session, through the app menu                 | All guest output enabled by default     | User approved 2026-11-02: retain host/build errors by default; terminal and headless output remain unchanged                                                      |
| Publish the repository after the successful downloaded-launcher/controller check                  | Continue private testing                | User confirmed the release flow and authorized public visibility, description and topics on 2026-11-02                                                            |
| Quick states in built game apps | In-game saves only; replay the opening | User approved 2026-11-03: resume a chosen point directly when testing scenes such as FF IX's first battle |
| Notify about GitHub releases and update the launcher from inside the app | Manual launcher ZIP replacement | User requested 2026-11-06; rebuilding existing game apps remains the user's responsibility |
