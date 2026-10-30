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
- **Graphics**: a **software renderer first**, shown on screen through Metal. A Metal hardware renderer with higher resolution comes later; the software one stays as the reference.
- **App shells**: AppKit/SwiftUI on Mac; UIKit with touch controls and Game Controller support on iOS.
- **BIOS**: OpenBIOS (MIT), bundled and itself recompiled.

**Core rule: code is never generated on the device.** Everything is compiled on the Mac at build time. That's what allows iOS at all, and it's compatible with Apple's hardened runtime on Mac.

## 3. Repository layout

```
ps1native/            (working name)
  tools/              Python: disc, exe, mips, analyze, codegen, capture, package
  scripts/            Python, run by the user: disc_info (what identifies a disc in its profile)
  runtime/            C: cpu, cpu_ops, interp, native, lockstep, mem, gte, gpu_sw, spu, cdrom, mdec, sio, sched
  gfx/metal/          Metal presentation, later the hardware renderer
  shell/mac/  shell/ios/
  bios/openbios/      MIT, bundled
  profiles/<serial>/  game.toml: hash, overlays, function hints, patches
  tests/              homebrew test programs (no game data); unit tests sit beside their code (runtime/tests, tools/tests)
  docs/               development: building, tests, CI, house style
```

## 4. Phases and exit criteria

Phases 0–2 have met their exit criteria. Phase 2's homebrew tests are the hardware test programs of [ps1-tests](tests/ps1-tests/README.md), which pass in CI, and its [first game](profiles/SLUS-01212/game.toml) reaches its title menu, checked again on 2026-10-30 with the runtime of commit a0d26af. [Development](docs/development.md#phase-2-hardware-breadth) records the checks and the remaining gaps.

| #   | Phase                                                                                                                                                                                                 | Done when                                                                                   |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| 0   | **Foundations**: repository, MIT license, CI (macOS GitHub Actions), tool and runtime skeletons                                                                                                       | CI builds an empty `.app`                                                                   |
| 1   | **Interpreter + boot**: R3000A interpreter, memory, COP0, interrupts, timers, DMA, minimal software GPU                                                                                               | **OpenBIOS shell shows in a Mac window**                                                    |
| 2   | **Hardware breadth**: CD-ROM, full software GPU, SPU, GTE, MDEC, pads, memory cards                                                                                                                   | Homebrew CPU/GTE/GPU tests pass; **first game reaches its title screen** on the interpreter |
| 3   | **Recompiler v1**: exe loader, function discovery, MIPS→C, cycle counting, recompiled OpenBIOS                                                                                                        | Main executable runs native and **matches the interpreter** in lockstep comparison          |
| 4   | **Overlays and profiles**: capture runs on Mac, overlay compilation keyed by content hash, a check that the RAM still holds the expected bytes before running native code, a survey for unknown games | First game mostly native and playable through its first level                               |
| 5   | **Mac product**: `ps1native build disc.cue --platform mac-app`, launcher, saves in Application Support                                                                                                | A double-clickable `.app` built from your disc                                              |
| 6   | **Metal renderer**: hardware rendering and higher resolution                                                                                                                                          | Matches the software renderer frame by frame                                                |
| 7   | **iOS**: same runtime, touch controls, generated Xcode project signed with your developer account                                                                                                     | **First PS1 game running natively on your iPhone**                                          |
| 8   | **More games**: profiles 2 and 3, chosen from different engines                                                                                                                                       | Each new game needs only a profile, no runtime changes                                      |

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

| Decision                                                                                    | Alternatives                            | Why                                                                                            |
| ------------------------------------------------------------------------------------------- | --------------------------------------- | ---------------------------------------------------------------------------------------------- |
| MIT license                                                                                 | GPL-3.0                                 | Maximum reuse; independence                                                                    |
| Mac first, then iOS                                                                         | iOS first                               | Mac is the development and debugging machine                                                   |
| Python tools, C runtime, Metal                                                              | Rust, C++                               | Fast iteration; C runs everywhere, including iOS                                               |
| Interpreter first                                                                           | Recompiler first                        | Gives a reference to compare against and a permanent fallback                                  |
| Software GPU first                                                                          | Metal first                             | Correctness before speed                                                                       |
| No code generation on the device                                                            | Compile on the device (as 2Xtreme does) | Required for iOS; works with Apple's hardened runtime                                          |
| OpenBIOS bundled                                                                            | Retail BIOS required                    | Legal, and no BIOS dump needed                                                                 |
| Clean-room, permissive dependencies only                                                    | Fork psxrecomp                          | Independence and license freedom                                                               |
| Conventions from nxnative (MIT): build, front end, tools, rules for contributors and agents | Own conventions                         | A working project of the same shape: native games on Apple platforms                           |
| Plain Makefiles and clang; Python scripts pack the apps                                     | CMake, an Xcode project, SwiftPM        | Nothing to install beyond Xcode and Python; the iOS project is generated (XcodeGen) in phase 7 |
| Objective-C with AppKit for the Mac front end                                               | Swift, SwiftUI                          | Calls the C runtime directly, as nxnative's host does                                          |
| Python tools: standard library, unittest, ruff (bug rules) and pyright; not a package       | pytest, a formatter, a pip package      | They run in place, with no install                                                             |
| Apple silicon, macOS 14 or later                                                            | Intel Macs, universal binaries          | One architecture to build and test, as on GitHub's runners                                     |
| OpenBIOS committed as a binary, built from source (its commit and hash in bios/openbios)    | Build it from a submodule; download it  | Building it needs Docker, beyond Xcode and Python; CI's macOS runners have none                |
| [Metal Slug X (USA), SLUS-01212](profiles/SLUS-01212/game.toml) as the first game           | The puzzle games suggested above        | User's choice; its dump stays outside the repository                                           |
| [ps1-tests](tests/ps1-tests/README.md) (MIT) as the homebrew test programs, as released     | Download or build them; write our own   | The console's logs fit these builds; an exception to permissive-only: PSn00bSDK is MPL-2.0     |
| A disc's licence string (sector 4) stands in for the SCEx string a BIN lacks                | Every disc unlicensed, or all accepted  | Games read their region from GetID; an unlicensed disc is refused, as a console does           |
| Own MIPS decoder in the Python tools, tested against the interpreter                        | rabbitizer (MIT), installed with pip    | Nothing to install beyond Xcode and Python; MIPS I is small, and the interpreter is its check  |
| Cycles per instruction: 1 from the cache, uncached fetches and loads by region, MULT/DIV    | An average per instruction; cache model | Lockstep needs exact counts; ps1-tests measures each cost; a cache costs native code dearly    |
| Interrupts taken at the end of a block of code, in the interpreter and in native code       | Between any two instructions            | Both sides see each interrupt at the same point, so the lockstep comparison is exact           |
| Generated C only in build folders, never in the repository, OpenBIOS's included             | Commit OpenBIOS's generated C           | A game's code is disc data; OpenBIOS's is rebuilt from its committed binary in each build      |
