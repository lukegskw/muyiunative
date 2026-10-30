# Development

[README.md](../README.md) says what ps1native is and how to build the app; [plan.md](../plan.md) has the design, the phases and the decision log; [AGENTS.md](../AGENTS.md) has the rules. This page is for working on ps1native itself: how it is built, tested and checked, and the house style.

## How it is built

| Part         | Where                                    | Built with                                                | State                                                                                                                                                         |
| ------------ | ---------------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime      | `runtime/` (C)                           | compiled into the host                                    | Interpreter, memory/COP0, interrupts, timers, DMA, software GPU, GTE, SPU, MDEC, external CUE/BIN discs with CD audio, digital pads and memory-card protocols |
| BIOS         | `bios/openbios/`                         | built from nugget's source, then committed (its README)   | OpenBIOS with its shell, which boots in phase 1                                                                                                               |
| Presentation | `gfx/metal/` (Objective-C, Metal)        | compiled into the host                                    | Shows the GPU's picture, stretched to the window                                                                                                              |
| Mac host     | `shell/mac/` (C and Objective-C, AppKit) | `make -C shell/mac` → `shell/mac/build/ps1native`         | Machine thread, Metal presentation, keyboard digital pad and stereo Audio Queue output; `--headless` supports frame-based input and local PNG capture         |
| Mac app      | `tools/pack_app.py`                      | `make -C shell/mac app` → `shell/mac/build/ps1native.app` | The host, with `bios/openbios` in its Resources, ad-hoc signed with the hardened runtime                                                                      |

There is no Xcode project for the Mac, and nothing to install beyond Xcode and Python: plain Makefiles drive clang (`make -C <folder>`), and Python scripts pack what clang built, as in nxnative. The iOS app (phase 7) will get its Xcode project generated (XcodeGen). Build outputs go to `build/` beside each Makefile, which git ignores.

The host is built for Apple silicon (`arm64`) and macOS 14 or later (`MINOS` in `shell/mac/Makefile`). `pack_app.py` reads the deployment target back from the host (`LC_BUILD_VERSION`) for `LSMinimumSystemVersion`, so the two cannot disagree, and signs the app with the hardened runtime: nothing is generated or mapped executable at run time ([plan.md](../plan.md#2-architecture), core rule). It replaces only an app it packed itself (the same bundle ID).

## Tests

Tests need no game files, retail BIOS or GPU: they build their inputs in code or take them from [tests/ps1-tests](../tests/ps1-tests/README.md), and run the bundled OpenBIOS where they need a BIOS. They sit beside the code they test.

| What                                                               | Command                                                                  |
| ------------------------------------------------------------------ | ------------------------------------------------------------------------ |
| Runtime unit tests, and OpenBIOS booted to its shell (`t_boot`)    | `make -C runtime/tests run`                                              |
| The same under ASan and UBSan, in a build folder of their own      | `make -C runtime/tests asan`                                             |
| Tool and script tests, the docs' links and AGENTS.md's length      | `python3 -m unittest discover -s tools/tests -v`                         |
| The app                                                            | `make -C shell/mac app && open shell/mac/build/ps1native.app`            |
| The host without a window: 300 frames, the last one's hash and PNG | `shell/mac/build/ps1native --headless --frames 300 --png /tmp/frame.png` |

A C test (`runtime/tests/t_<what>.c`) includes `ps1.h` and links the whole runtime, built once into objects; it counts its checks with `CHECK`, prints `<what>: N checks, M failed` and exits non-zero on a failure; add it to `TESTS` in `runtime/tests/Makefile`. A Python test is a `unittest` file, `tools/tests/test_<what>.py`, whose docstring says what it checks. A `[RED]` line in a test's output is the runtime reporting what it does not model yet, not a failure. A PNG of a frame stays outside the repository; a frame is compared by its hash (`ps1_frame_hash`).

### Phase 2: hardware breadth

The host accepts `--disc /external/path/disc.cue` (or a single raw `.bin`). It opens the external files read-only and keeps them mounted across machine resets; it never packages or copies them. `t_disc` builds synthetic files in `/tmp` to test CUE addressing, multiple files and tracks, stored pregaps, validation and independent machines. `t_cdread` checks the controller ports, 75/150 Hz sector delivery, 2048/2340-byte FIFOs, BFRD data locking, DMA3 and Pause. `t_cdaudio` checks the commands' parameters, SetSession and GetQ, XA-ADPCM filtering and decoding, CD-DA Play with Report and AutoPause, the volume matrix and the SPU's CD input.

Supported CUE tracks use `MODE1/2352`, `MODE2/2352` or `AUDIO` in `BINARY` files, with `INDEX 00`/`01`. Other layouts, including synthesized `PREGAP`/`POSTGAP`, are rejected. A BIN lacks the SCEx string of the disc's wobble groove, which the drive checks, so the drive reads the licence string in sector 4 instead ([decision log](../plan.md#8-decision-log)): a licensed disc reads as on a console of its region, and a disc without one is refused, as by a console without a modchip. `scripts/disc_info.py <disc.cue>` prints what identifies a dump in its profile: the serial, the licence's region, and the SHA-256 of the boot executable and of each track, but never the dump's path.

`t_texture` covers 4/8/15-bit textures, CLUT and texture caches, affine polygons, windows/flips, modulation, transparency and masks. `t_gte` covers all documented commands, transfer side effects, overflow/saturation and the integer reciprocal. `t_spu` covers ADPCM/Gaussian interpolation, voices, ADSR, sweeps, noise, pitch modulation, reverb, capture and interrupts. `t_sio` sends complete digital-pad and memory-card transactions through the timed ports. `t_mdec` covers RLE, uploaded quantization/IDCT tables, pixel formats, FIFO backpressure and reordered DMA output. `t_timers` covers the counters' clocks, targets and IRQs, and their synchronization to hblanks and vblanks.

`t_ps1tests` runs 18 hardware test programs of [ps1-tests](../tests/ps1-tests/README.md), plan.md's homebrew test programs, under the bundled OpenBIOS: what a program prints must be the console's log, line for line, and the VRAM of a program that draws must hash as the console's picture does (`tools/ps1tests.py` hashes the picture, and shows where a dump of the runtime's VRAM differs). That README lists the programs left out, and why. `t_homebrew` adds three small programs built in code from MIPS encodings. All run in CI. This follows nxnative's separation of [synthetic CI tests](https://github.com/seedds/nxnative/blob/main/.github/workflows/tests.yml) from [local game bring-up evidence](https://github.com/seedds/nxnative/blob/main/docs/agents/README.md#checklist-for-a-new-title).

The [first game](../profiles/SLUS-01212/game.toml) reaches its title/main menu by the route in its profile: on 2026-10-30 with the runtime of commit 8680bf2, and again with a0d26af, to the same frame hash. Guest output is discarded during game validation; screenshots stay outside the repository and only frame hashes are recorded. Game validation is separate from CI, which needs no dump.

For a local headless route, `--input <frame>:<hex-buttons>` changes the port-1 active-high button mask at a zero-based presented-frame index and holds it until the next event. Repeat the option in frame order. For example, Start (bit 3) for six frames:

```sh
shell/mac/build/ps1native --disc /external/path/disc.cue --headless \
  --frames 2400 --input 1800:8 --input 1806:0 --png /tmp/frame.png
```

The window uses the [keyboard mapping](../README.md#build-from-the-command-line). Controller and card state belongs to each machine; the host connects one digital pad and leaves card slots empty. Tests insert synthetic 128 KB cards in memory. Host-file save persistence belongs to the product work in Phase 5. SPU samples are drained on the machine thread; the Mac front end owns a separately locked playback ring, so its audio callback never accesses machine state.

Remaining gaps, each reported by a `[RED]` line when a program meets it: the CD-ROM's mechanical and seek timing, partial-sector overruns and sound-map XA-ADPCM; DMA timing; GTE writes during a command; the SPU's sub-sample RAM, key-on and FIFO timing; MDEC's IDCT rounding and decode timing; which lines 480-line interlaced drawing leaves out, which psx-spx does not say; controllers other than the digital pad, and the serial link. The CPU charges an average of 2 cycles an instruction until phase 3 counts them per block, so the ps1-tests programs that measure timing are left out. On its way to the title menu, the first game twice sends a GPU list that comes back on itself (`[RED] dma: the GPU list at 0d4ae4 ...`): the same pair of packets, one word and then three, is linked under two neighbouring entries of the list in one frame, so the list runs through the pair into the second entry and back to the pair. A console never completes such a list either. The loop is the same whether the CPU charges 1 or 2 cycles an instruction, so the CPU's speed is not the cause; the next suspect is the DMA, which here completes the moment it starts, while a console's runs as long as the GPU draws. Whether the game builds the loop on a console is still open. Gameplay, audible game output, broader hardware conformance and other games remain unverified.

## Continuous integration

`.github/workflows/tests.yml` runs on every pull request and push to `main`, on GitHub's `xcode-27` macOS image with Xcode 27.1 named by path in `DEVELOPER_DIR` (a new default Xcode on the image does not move CI): the runtime's unit tests, plain and under ASan and UBSan; the tool tests; and the Mac app, built, packed and checked (`plutil -lint`, `codesign --verify --strict`, the hardened runtime flag, `lipo -archs`), then run without a window from inside the bundle, which boots the OpenBIOS in its Resources.

`.github/workflows/lint-python.yml` runs ruff and pyright over the tools and scripts (configured in `pyproject.toml`), at the versions pinned in `.github/requirements/lint.txt`. Every finding fails the job: fix it, or silence it on its line (`# noqa: <rule>`, `# pyright: ignore[<rule>]`) with a reason. With the same versions locally, in a venv outside the repository:

```sh
python3 -m venv /path/to/lint-venv && /path/to/lint-venv/bin/pip install -r .github/requirements/lint.txt
/path/to/lint-venv/bin/python3 -m ruff check . && /path/to/lint-venv/bin/python3 -m pyright
```

Actions are pinned to commit SHAs, with the tag in a comment: a tag can be moved, a SHA cannot. Each job reads the repository only (`permissions: contents: read`), has a timeout, and is cancelled by a newer run of the same branch.

## House style

The code is dense and its comments carry the knowledge, as in nxnative:

- **Comments.** Each file starts with a comment on what it is, where it sits in plan.md, and the hardware facts it relies on, with their psx-spx section. A comment says what the code cannot: why, what was measured, what is still open; one short line where that is enough.
- **C** (the runtime and the hosts): C17 as clang builds it, free of warnings under `-Wall -Wextra`. Short names prefixed by their part (`mem_`, `front_`, `present_`), all machine state in the `ps1` struct ([AGENTS.md](../AGENTS.md)), several short statements on a line where they read as one step. Objective-C, with ARC, only where an Apple framework needs it (`shell/mac/front_mac.m`, `gfx/metal/`): the runtime is plain C, so iOS builds it unchanged.
- **Python** (the tools): the standard library first, and a package only for what it cannot do, under a permissive license (rabbitizer for MIPS decoding, phase 3). A script takes its options with argparse, prints a line per step, and fails with `raise SystemExit(<the reason>)`. No formatter: ruff runs rules that find bugs, not style rules, and pyright checks types in basic mode.
- **Logs.** English, one line each, prefixed by their part (`host: `, `mem: `); a line that marks something unimplemented or wrong starts with `[RED] ` (`RED()` in the runtime).
