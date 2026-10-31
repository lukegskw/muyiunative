# ps1native

Play your own PlayStation (PS1) discs natively on the Mac, and later on the iPhone. Bring your game (a `.cue`/`.bin` dump of your disc); ps1native turns it into an app.

The aim is native code, not emulation: the game's MIPS code is translated to C and compiled ahead of time on your Mac, and runs as Apple-silicon code beside a runtime that models the console's hardware; an interpreter covers the code that could not be compiled ahead of time. Nothing from any game or from Sony's BIOS is in this repository: the BIOS ps1native runs is [OpenBIOS](bios/openbios/README.md), open source and built from source. The project is not affiliated with Sony Interactive Entertainment or any game publisher.

## Status

Phase 4 of [the plan](plan.md#4-phases-and-exit-criteria), overlays and profiles, has met its exit criterion: Metal Slug X (USA) clears Mission 1 entirely as native code and agrees with the interpreter in lockstep through its results screen. Captures preserve overwritten code versions by content hash, and the runtime checks their bytes before use; scheduled GPU DMA packets remove the looping-list performance stall found after phase 3. The hardware test programs of [ps1-tests](tests/ps1-tests/README.md) still pass. Broader compatibility, audible game output and hardware-timing accuracy remain open ([Development](docs/development.md#phase-4-overlays-and-profiles)). Phase 5, the Mac product, is next.

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

The keyboard controls a digital pad in port 1; its [mapping is listed below](#keyboard-controls). Sound is played through the Mac's default output. Memory cards currently have in-memory protocol support; the host leaves the slots empty. Supported disc layouts, reproducible headless inputs and remaining gaps are in [Development](docs/development.md#phase-2-hardware-breadth).

OpenBIOS runs as native code, compiled with the app from its bundled binary; a game's code is interpreted until you make it native, as below. `--interpret` runs everything in the interpreter, and `--headless --lockstep` runs an interpreting machine beside the native one and stops at their first difference ([Development](docs/development.md#phase-3-the-recompiler)).

To turn a game with a profile in `profiles/` into native code on your Mac, and check it against the interpreter along the profile's route:

```sh
scripts/native_game.py /external/path/disc.cue
```

Use `--route first_level` to reproduce the first game's full first-level acceptance route. For an unknown game, `scripts/survey_game.py /external/path/disc.cue --frames 2400` captures the supplied route and writes a draft profile and a local picture to inspect ([Development](docs/development.md#phase-4-overlays-and-profiles)).

It writes the game's code into `shell/mac/build`, where it stays: never commit or share it. It also builds a host with that code, which runs the game in a window: `shell/mac/build/game/ps1native --disc /external/path/disc.cue`.

### Keyboard controls

Use the left hand for movement and the right hand for the face buttons:

| Keyboard key  | PlayStation button       |
| ------------- | ------------------------ |
| W / A / S / D | Up / Left / Down / Right |
| I             | Triangle (△)             |
| J             | Square (□)               |
| K             | Cross (✕)                |
| L             | Circle (○)               |
| Q / E         | L1 / R1                  |
| 1 / 3         | L2 / R2                  |
| Enter/Return  | Start                    |
| Backspace     | Select                   |

On a Mac keyboard, Backspace is the key labelled Delete (⌫). With Metal Slug X's default controls, J fires, K jumps and L throws a grenade. Keyboard remapping is planned for phase 5; the current layout is fixed.

### For developers

[Plan](plan.md) · [Development](docs/development.md) · [Contributing](CONTRIBUTING.md) · [Notes for agents](AGENTS.md)

### Acknowledgements

ps1native takes its build, its Mac front end and its rules for contributors and agents from **[nxnative](https://github.com/seedds/nxnative)** (MIT), which runs Switch games natively on Apple platforms. The console's hardware is modelled from **[psx-spx](https://psx-spx.consoledev.net/)**, the public PlayStation specifications. The BIOS is **OpenBIOS** from [nugget](https://github.com/pcsx-redux/nugget) (MIT), by the PCSX-Redux authors. The hardware test programs are **[ps1-tests](https://github.com/JaCzekanski/ps1-tests)** (MIT) by Jakub Czekański, built with [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) (MPL-2.0).

### License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license, beside it: [OpenBIOS](bios/openbios/README.md) (MIT, with BSD parts), and the test programs of [ps1-tests](tests/ps1-tests/README.md) (MIT, with PSn00bSDK's libraries under MPL-2.0, which never reach the app).
