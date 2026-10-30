# ps1-tests

Test programs from [ps1-tests](https://github.com/JaCzekanski/ps1-tests) by Jakub Czekański and its contributors: homebrew PlayStation programs that check the hardware, each released with what it printed or drew on a console. [t_ps1tests](../../runtime/tests/t_ps1tests.c) runs the ones below under the bundled OpenBIOS ([plan.md](../../plan.md#6-testing): homebrew test programs in CI) and checks them against the console. They are not game data.

| What    | Value                                                                                                                                                                             |
| ------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Source  | release [build-158](https://github.com/JaCzekanski/ps1-tests/releases/tag/build-158), `tests.zip`, built by the suite's CI from commit `5f73d61b27a8d51269b54dde1c456d2d2f7dc865` |
| SHA-256 | `5de0e82854c1a511697a7ade381c2d778ec4b2fff4a7d7ac79e8098c35eb859b` (`tests.zip`)                                                                                                  |
| Here    | each program's `.exe` as released and, for a program that prints its results, the console's `psx.log`                                                                             |

## What runs

A program that prints its results must print the console's log, line for line. A few of the released programs are newer than their logs and print lines for tests added later; t_ps1tests lists those lines (`extra`), and only those are let through. A program that draws must leave VRAM as the console's picture of it in the release shows (`vram.png`), compared by a hash of the pixels (screenshots stay out of the repository, [AGENTS.md](../../AGENTS.md)).

| Program                                                                                                                                                      | Checked against                                                                                                          |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------ |
| cpu/cop, cpu/code-in-io, gte/test-all, gpu/gp0-e1, gpu/mask-bit, dma/otc-test, dma/dpcr                                                                      | the console's log                                                                                                        |
| gpu/triangle, gpu/quad, gpu/uv-interpolation, gpu/rectangles, gpu/texture-flip, gpu/texture-overflow, gpu/vram-to-vram-overlap, gpu/clut-cache, gpu/clipping | the console's VRAM                                                                                                       |
| gpu/lines                                                                                                                                                    | the console's VRAM but two 33x33 squares at (150,140) and (210,140), whose polylines use a colour the program never sets |
| gpu/transparency                                                                                                                                             | the console's VRAM in its 320x240 picture: elsewhere the console's VRAM holds what was there before                      |

## What does not run, and why

| Program                                                                                     | Why                                                                                                                                                   |
| ------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| cpu/access-time, timers                                                                     | they print times in cycles; the CPU's costs are access-time's measurements rounded, and timers comes within two cycles of the log                     |
| gpu/bandwidth, cdrom/timing, dma/chopping                                                   | they print times measured in cycles, and the runtime does not model the timing of the devices                                                         |
| dma/chain-looping                                                                           | it times the CPU's work while the DMA runs; its other result, a list that never ends keeping the channel busy, is in `t_dma`                          |
| cpu/io-access-bitwidth                                                                      | it prints what reads return where no device drives the bus, which psx-spx does not document; `t_mem` checks the stores it shows                       |
| spu/memory-transfer                                                                         | it times SPU DMA, and the released program runs tests its log predates                                                                                |
| mdec/4bit, mdec/8bit, mdec/frame, mdec/movie, mdec/step-by-step-log                         | the IDCT does not round as the console's does yet (a `[RED]` line says so); `t_mdec` has what the 4-bit and 8-bit logs show about blocks and rounding |
| cdrom/getloc, cdrom/disc-swap                                                               | they run from their own CD on the console, and disc-swap has the lid opened and closed                                                                |
| gte-fuzz                                                                                    | gte/test-all was generated from it                                                                                                                    |
| timer-dump                                                                                  | it needs a modified console                                                                                                                           |
| cdrom/terminal, input/pad, spu/\*, gpu/animated-triangle, gpu/benchmark, gpu/version-detect | they are interactive or released without a record from a console                                                                                      |

## Licenses

| Part                                             | License | File                                   |
| ------------------------------------------------ | ------- | -------------------------------------- |
| ps1-tests: the programs, their logs and pictures | MIT     | [LICENSE](LICENSE)                     |
| PSn00bSDK's libraries, linked into each program  | MPL-2.0 | [LICENSE.PSn00bSDK](LICENSE.PSn00bSDK) |

PSn00bSDK's source is at [github.com/Lameguy64/PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK). The suite's CI built this release with the Docker image `jaczekanski/psn00bsdk:latest`, pushed on 2020-11-22 and not since (digest `sha256:15057bc87f01caf8a849468f5c62c68a6995072705a01d385c3ada9ef212f5e7`): PSn00bSDK as of November 2020, in that repository's history. The programs name no version; they say "Built using GCC and PSn00bSDK libraries", and libpsxgpu "2020 PSn00bSDK Project". They are here as released, unmodified, the one exception to the project's rule of permissive licenses only ([decision log](../../plan.md#8-decision-log)). The runtime runs the programs as a console would; it does not link them or take code from them.

## Updating, and finding a difference

To update: download the release, check its SHA-256, and copy the programs and logs that run into this folder, keeping the release's layout; for a program that draws, `python3 tools/ps1tests.py hash <release>/gpu/<name>/vram.png` (with `--area` and `--skip` as in t_ps1tests) gives the hash for t_ps1tests. A tool test checks that t_ps1tests and this folder name the same files.

When a drawing program fails, `PS1TESTS_DUMP=/tmp/vram make -C runtime/tests run` writes each one's VRAM there, and `python3 tools/ps1tests.py diff <release>/gpu/lines/vram.png /tmp/vram/gpu-lines.vram --png /tmp/lines.png` lists the differing pixels and draws the console's VRAM, the runtime's, and the difference side by side (keep the picture local).
