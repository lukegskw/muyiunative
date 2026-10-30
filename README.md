# ps1native

Play your own PlayStation (PS1) discs natively on the Mac, and later on the iPhone. Bring your game (a `.cue`/`.bin` dump of your disc); ps1native turns it into an app.

The aim is native code, not emulation: the game's MIPS code is translated to C and compiled ahead of time on your Mac, and runs as Apple-silicon code beside a runtime that models the console's hardware; an interpreter covers the code that could not be compiled ahead of time. Nothing from any game or from Sony's BIOS is in this repository: the BIOS ps1native runs is [OpenBIOS](bios/openbios/README.md), open source and built from source. The project is not affiliated with Sony Interactive Entertainment or any game publisher.

## Status

Phase 2 of [the plan](plan.md#4-phases-and-exit-criteria), hardware breadth, has met its exit criterion: the interpreter has the CD-ROM drive with its audio, the full software GPU, the GTE, the SPU, the MDEC, digital pads and memory cards, passes the hardware test programs of [ps1-tests](tests/ps1-tests/README.md) in CI, and takes Metal Slug X (USA) to its title menu. Gameplay, broader compatibility and timing accuracy still need validation.

## Build from the command line

You need a Mac with Apple silicon, [Xcode](https://apps.apple.com/app/xcode/id497799835) 27.1 and Python 3.12 or newer. In a clone of this repository:

```
make -C shell/mac app
open shell/mac/build/ps1native.app
```

The app boots OpenBIOS and shows its shell when no disc is mounted. It runs on macOS 14 or later. To run your external dump:

```sh
shell/mac/build/ps1native --disc /external/path/disc.cue
```

The keyboard controls a digital pad in port 1: arrows for the D-pad; Z/X/A/S for Cross/Circle/Square/Triangle; Q/W for L1/R1; 1/2 for L2/R2; Return for Start and Space for Select. Sound is played through the Mac's default output. Memory cards currently have in-memory protocol support; the host leaves the slots empty. Supported disc layouts, reproducible headless inputs and remaining gaps are in [Development](docs/development.md#phase-2-hardware-breadth).

### For developers

[Plan](plan.md) · [Development](docs/development.md) · [Contributing](CONTRIBUTING.md) · [Notes for agents](AGENTS.md)

### Acknowledgements

ps1native takes its build, its Mac front end and its rules for contributors and agents from **[nxnative](https://github.com/seedds/nxnative)** (MIT), which runs Switch games natively on Apple platforms. The console's hardware is modelled from **[psx-spx](https://psx-spx.consoledev.net/)**, the public PlayStation specifications. The BIOS is **OpenBIOS** from [nugget](https://github.com/pcsx-redux/nugget) (MIT), by the PCSX-Redux authors. The hardware test programs are **[ps1-tests](https://github.com/JaCzekanski/ps1-tests)** (MIT) by Jakub Czekański, built with [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) (MPL-2.0).

### License

MIT ([LICENSE](LICENSE)).
