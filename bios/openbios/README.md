# OpenBIOS

`openbios.bin` is OpenBIOS, the open-source PlayStation BIOS from [nugget](https://github.com/pcsx-redux/nugget) (MIT), built from source; it is not a dump of a console's BIOS. ps1native runs it as the console's BIOS ([plan.md](../../plan.md#2-architecture): OpenBIOS, bundled). Its default shell, shello, is embedded: a cube drawn with psyqo that checks the CD-ROM drive for a disc.

| What       | Value                                                                                                                                                                      |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Source     | nugget at commit `175f329db302892e4aac146a22778d1876cae48f`, folder `openbios/`, default settings                                                                          |
| Built with | `docker run --rm -v "$PWD":/project -w /project/openbios -u "$(id -u):$(id -g)" ghcr.io/grumpycoders/pcsx-redux-build:latest make -j8`, from the root of a nugget checkout |
| Size       | 524,288 bytes (512 KB, the console's BIOS ROM)                                                                                                                             |
| SHA-256    | `caf2162eb498408f86e19f9b3a3c5e083e34c25a43baae33ffc3641afffa54c8` (the build is reproducible: a clean rebuild gives the same hash)                                        |

## What the shell shows

With no disc, the shell's CD check fails and the cube turns to its error colours. In 480-line interlaced mode the shell clears (0,256) 640x480 every frame (psyqo's `GPU::getNextClear`; a temporary log of the fills showed it in 295 of 300 frames), which wraps at VRAM's line 512 (psx-spx, GPU Memory Transfer Commands: Wrapping) and so never clears rows 224 to 255: the cube leaves trails there. The GPU wraps as psx-spx says; the trails are the shell's, not a runtime bug.

## As native code

The Mac host and the runtime's tests run OpenBIOS as native code (plan.md, phase 3), generated in each build from this binary and never committed. OpenBIOS copies its kernel and its shell into RAM and enters them through jumps a static analysis cannot follow, so the build first runs it in the interpreter for 60 frames (`tools/capture.c`): RAM as it is then and the 251 addresses the CPU called or jumped to through a register are what `tools/codegen.py --bios --capture` analyzes beside the ROM from its reset vector: 313 functions. Phase 4 also keeps 1,211 immutable blocks as they execute, including code overwritten before capture ends, and adds their native versions by content hash: 1,524 functions/blocks in all. From reset through 50 frames of the shell, all 4,287,029 instructions run natively and agree in lockstep with the interpreter (`t_lockstep`); a 300-frame run interprets 24 instructions on paths the capture did not reach.

## Licenses

| Part                                                     | License       | File                                  |
| -------------------------------------------------------- | ------------- | ------------------------------------- |
| OpenBIOS, shello and psyqo (nugget)                      | MIT           | [LICENSE](LICENSE)                    |
| EASTL, compiled into shello through psyqo                | BSD 3-Clause  | [LICENSE.EASTL](LICENSE.EASTL)        |
| EABase, EASTL's base headers                             | BSD 3-Clause  | [LICENSE.EABase](LICENSE.EABase)      |
| The kernel's `xprintf` (nugget, `common/libc/xprintf.c`) | Public domain | nugget's source header                |
| The fonts (`openbios/charset`, from the Shinonome font)  | Public domain | nugget's `openbios/charset/README.md` |

To update it, build the new commit as above, replace `openbios.bin`, the license files if they changed, and this page's commit and hash.
