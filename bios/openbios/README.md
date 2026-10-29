# OpenBIOS

`openbios.bin` is OpenBIOS, the open-source PlayStation BIOS from [nugget](https://github.com/pcsx-redux/nugget) (MIT), built from source; it is not a dump of a console's BIOS. ps1native runs it as the console's BIOS ([plan.md](../../plan.md#2-architecture): OpenBIOS, bundled). Its default shell, shello, is embedded: a cube drawn with psyqo that checks the CD-ROM drive for a disc.

| What       | Value                                                                                                                                                                      |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Source     | nugget at commit `175f329db302892e4aac146a22778d1876cae48f`, folder `openbios/`, default settings                                                                          |
| Built with | `docker run --rm -v "$PWD":/project -w /project/openbios -u "$(id -u):$(id -g)" ghcr.io/grumpycoders/pcsx-redux-build:latest make -j8`, from the root of a nugget checkout |
| Size       | 524,288 bytes (512 KB, the console's BIOS ROM)                                                                                                                             |
| SHA-256    | `caf2162eb498408f86e19f9b3a3c5e083e34c25a43baae33ffc3641afffa54c8` (the build is reproducible: a clean rebuild gives the same hash)                                        |

## Licenses

| Part                                                     | License       | File                                  |
| -------------------------------------------------------- | ------------- | ------------------------------------- |
| OpenBIOS, shello and psyqo (nugget)                      | MIT           | [LICENSE](LICENSE)                    |
| EASTL, compiled into shello through psyqo                | BSD 3-Clause  | [LICENSE.EASTL](LICENSE.EASTL)        |
| EABase, EASTL's base headers                             | BSD 3-Clause  | [LICENSE.EABase](LICENSE.EABase)      |
| The kernel's `xprintf` (nugget, `common/libc/xprintf.c`) | Public domain | nugget's source header                |
| The fonts (`openbios/charset`, from the Shinonome font)  | Public domain | nugget's `openbios/charset/README.md` |

To update it, build the new commit as above, replace `openbios.bin`, the license files if they changed, and this page's commit and hash.
