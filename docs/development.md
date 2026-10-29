# Development

[README.md](../README.md) says what ps1native is and how to build the app; [plan.md](../plan.md) has the design, the phases and the decision log; [AGENTS.md](../AGENTS.md) has the rules. This page is for working on ps1native itself: how it is built, tested and checked, and the house style.

## How it is built

| Part         | Where                                    | Built with                                                | State                                                                                                                                                           |
| ------------ | ---------------------------------------- | --------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Runtime      | `runtime/` (C)                           | compiled into the host                                    | The interpreter, memory, COP0 and interrupts, timers, DMA and the software GPU; the CD-ROM drive (no disc), the SPU's registers and the controller port (empty) |
| BIOS         | `bios/openbios/`                         | built from nugget's source, then committed (its README)   | OpenBIOS with its shell, which boots in phase 1                                                                                                                 |
| Presentation | `gfx/metal/` (Objective-C, Metal)        | compiled into the host                                    | Shows the GPU's picture, stretched to the window                                                                                                                |
| Mac host     | `shell/mac/` (C and Objective-C, AppKit) | `make -C shell/mac` → `shell/mac/build/ps1native`         | Runs the machine on a thread of its own at the console's speed; `--headless` runs it without a window                                                           |
| Mac app      | `tools/pack_app.py`                      | `make -C shell/mac app` → `shell/mac/build/ps1native.app` | The host, with `bios/openbios` in its Resources, ad-hoc signed with the hardened runtime                                                                        |

There is no Xcode project for the Mac, and nothing to install beyond Xcode and Python: plain Makefiles drive clang (`make -C <folder>`), and Python scripts pack what clang built, as in nxnative. The iOS app (phase 7) will get its Xcode project generated (XcodeGen). Build outputs go to `build/` beside each Makefile, which git ignores.

The host is built for Apple silicon (`arm64`) and macOS 14 or later (`MINOS` in `shell/mac/Makefile`). `pack_app.py` reads the deployment target back from the host (`LC_BUILD_VERSION`) for `LSMinimumSystemVersion`, so the two cannot disagree, and signs the app with the hardened runtime: nothing is generated or mapped executable at run time ([plan.md](../plan.md#2-architecture), core rule). It replaces only an app it packed itself (the same bundle ID).

## Tests

Tests need no game files, retail BIOS or GPU: they build their inputs in code, and the boot test runs the bundled OpenBIOS. They sit beside the code they test.

| What                                                               | Command                                                                  |
| ------------------------------------------------------------------ | ------------------------------------------------------------------------ |
| Runtime unit tests, and OpenBIOS booted to its shell (`t_boot`)    | `make -C runtime/tests run`                                              |
| The same under ASan and UBSan, in a build folder of their own      | `make -C runtime/tests asan`                                             |
| Tool tests: the app packer, the docs' links and AGENTS.md's length | `python3 -m unittest discover -s tools/tests -v`                         |
| The app                                                            | `make -C shell/mac app && open shell/mac/build/ps1native.app`            |
| The host without a window: 300 frames, the last one's hash and PNG | `shell/mac/build/ps1native --headless --frames 300 --png /tmp/frame.png` |

A C test (`runtime/tests/t_<what>.c`) includes `ps1.h` and links the whole runtime, built once into objects; it counts its checks with `CHECK`, prints `<what>: N checks, M failed` and exits non-zero on a failure; add it to `TESTS` in `runtime/tests/Makefile`. A Python test is a `unittest` file, `tools/tests/test_<what>.py`, whose docstring says what it checks. A `[RED]` line in a test's output is the runtime reporting what it does not model yet, not a failure. A PNG of a frame stays outside the repository; a frame is compared by its hash (`ps1_frame_hash`).

## Continuous integration

`.github/workflows/tests.yml` runs on every pull request and push to `main`, on GitHub's `xcode-27` macOS image with Xcode 27.1 named by path in `DEVELOPER_DIR` (a new default Xcode on the image does not move CI): the runtime's unit tests, plain and under ASan and UBSan; the tool tests; and the Mac app, built, packed and checked (`plutil -lint`, `codesign --verify --strict`, the hardened runtime flag, `lipo -archs`), then run without a window from inside the bundle, which boots the OpenBIOS in its Resources.

`.github/workflows/lint-python.yml` runs ruff and pyright over the tools (configured in `pyproject.toml`), at the versions pinned in `.github/requirements/lint.txt`. Every finding fails the job: fix it, or silence it on its line (`# noqa: <rule>`, `# pyright: ignore[<rule>]`) with a reason. With the same versions locally, in a venv outside the repository:

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
