# muyiunative

Play your own PlayStation (PS1) discs natively on the Mac, and later on the iPhone. Bring your game (a `.cue`/`.bin` dump of your disc); muyiunative turns it into an app.

The aim is native code, not emulation: the game's MIPS code is translated to C and compiled ahead of time on your Mac, and runs as Apple-silicon code beside a runtime that models the console's hardware; an interpreter covers the code that could not be compiled ahead of time. Nothing from any game or from Sony's BIOS is in this repository: the BIOS muyiunative runs is [OpenBIOS](bios/openbios/README.md), open source and built from source. The project is not affiliated with Sony Interactive Entertainment or any game publisher.

## Status

Phase 6 of [the plan](plan.md#4-phases-and-exit-criteria) adds independent Metal rendering at 1×, 2× and 4×. Native Metal VRAM and texture caches match the software renderer throughout Metal Slug X (USA)'s first mission. The software renderer remains the default and the console reference; higher-resolution Metal output retains that CPU work ([Development](docs/development.md#phase-6-the-metal-renderer)). Phase 7, the Mac public preview, is in progress: game apps now support two controllers, alongside the launcher, persistent memory cards and keyboard remapping. Building from the launcher, problem reports and a downloadable preview remain to come. Broader compatibility, audible game output and hardware-timing accuracy remain open.

## Build from the command line

You need a Mac with Apple silicon, [Xcode](https://apps.apple.com/app/xcode/id497799835) 27.1 with its Metal Toolchain component, and Python 3.12 or newer. Install the component in Xcode Settings → Components, or run `xcodebuild -downloadComponent MetalToolchain` ([Apple's instructions](https://developer.apple.com/documentation/xcode/downloading-and-installing-additional-xcode-components)). In a clone of this repository:

```sh
scripts/muyiunative build /external/path/disc.cue --platform mac-app
```

The command captures the game's code, compiles it, checks the route against the interpreter and creates `shell/mac/build/apps/<game>.app`. Double-click that app to play on macOS 14 or later. Its signed bundle includes OpenBIOS, the native game code and a local copy of the disc's tracks; it works after moving the app or the original dump. Keep game apps and generated code on your Mac.

Add `--output /path/Game.app` to choose another location. A known profile supplies the title and capture route: `first_level` when available, otherwise `validation`. Use `--route <table>`, `--frames <n>` and repeatable `--input <frame>:<hex-buttons>` to choose a route. Unknown games default to a 2,400-frame capture; code not seen during capture runs in the interpreter. Supported disc layouts are in [Development](docs/development.md#phase-2-hardware-breadth).

For the shorter command in the plan, add this checkout's scripts to your terminal's path:

```sh
export PATH="$PWD/scripts:$PATH"
muyiunative build /external/path/disc.cue --platform mac-app
```

The optional launcher opens game apps and remembers recent ones:

```sh
make -C shell/mac app
open shell/mac/build/muyiunative.app
```

### Memory cards

Game apps connect two memory cards and save them in `~/Library/Application Support/muyiunative/Saves/<serial>-<boot-sha256>/card1.mcr` and `card2.mcr`. They are raw 128 KB images, created only when missing, saved once per emulated second when dirty and again before quitting. Moving or rebuilding the same game app preserves its saves. Close the game before backing up or replacing its cards; a second copy cannot use the same save folder simultaneously. Invalid cards are reported and kept intact.

### Graphics

Choose Software or Metal at 1×, 2× or 4× in the game app's Graphics menu. Switching keeps the game running. Software remains the default. Metal draws the PS1 commands independently on the GPU; 2× and 4× add geometry samples, while textures retain their source resolution. Native-width lines, 24-bit video and texture-feedback regions retain native pixel detail. The software GPU still maintains the console's reference VRAM, so this version prioritizes higher-resolution output and exact native rendering over a CPU speed-up.

To make an app open with Metal at 2×:

```sh
scripts/muyiunative build /external/path/disc.cue --platform mac-app --renderer metal --scale 2
```

The shader library is built and bundled with the app; playing needs no compiler. Headless runs default to software for reproducible validation. Developers can use `--renderer metal --scale 1 --verify-renderer` to compare native Metal VRAM and caches against software every frame ([Development](docs/development.md#phase-6-the-metal-renderer)).

### Controllers

Connect a macOS-supported extended gamepad over USB, or pair it in System Settings → Bluetooth. Game apps use Apple's GameController framework for wired and wireless input. The keyboard and first controller share port 1; a second controller uses port 2. Controllers can connect and disconnect during play. A remaining controller keeps its port; another controller fills the first vacant port. When the game window loses focus, both ports release their buttons until it regains focus.

| Controller control             | PlayStation button                |
| ------------------------------ | --------------------------------- |
| South / East / West / North     | Cross / Circle / Square / Triangle |
| Left / Right shoulder          | L1 / R1                           |
| Left / Right trigger           | L2 / R2                           |
| Menu / Options                 | Start / Select                    |
| D-pad or left stick            | Up / Left / Down / Right          |

Face buttons follow their positions rather than their printed letters. The left stick becomes digital at ±0.5 on each axis; triggers activate at 0.5. D-pad and stick directions combine. A controller without an Options button can use the keyboard's Select binding on port 1. Physical wired/wireless play and two-player hardware checks are still pending ([Development](docs/development.md#phase-7-the-public-preview)).

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

On a Mac keyboard, Backspace is the key labelled Delete (⌫). With Metal Slug X's default controls, J fires, K jumps and L throws a grenade. Choose **Keyboard Controls…** in the game app's menu (⌘,) to change a binding: click its button, then press a key. Assigning an occupied key moves it to the new button; Unassign clears a binding, and Restore Defaults restores this table. Changes apply to all game apps.

### For developers

`make -C shell/mac` builds the developer host. Run `shell/mac/build/muyiunative --disc /external/path/disc.cue` to interpret an external disc, or add `--bios bios/openbios/openbios.bin` without a disc to open the BIOS shell. `--headless --lockstep` checks native execution against the interpreter. Headless runs leave card slots empty unless `--save-dir <directory>` is given, so existing validation routes remain reproducible.

`scripts/native_game.py /external/path/disc.cue --route first_level` reproduces phase 4's acceptance checks and timings. `scripts/survey_game.py /external/path/disc.cue --frames 2400` captures an unknown game and writes a draft profile and local picture to inspect. Captures, game apps and generated C stay in ignored build folders ([Development](docs/development.md#phase-5-the-mac-product)).

[Plan](plan.md) · [Development](docs/development.md) · [Contributing](CONTRIBUTING.md) · [Notes for agents](AGENTS.md)

### Acknowledgements

muyiunative takes its build, its Mac front end and its rules for contributors and agents from **[nxnative](https://github.com/seedds/nxnative)** (MIT), which runs Switch games natively on Apple platforms. The console's hardware is modelled from **[psx-spx](https://psx-spx.consoledev.net/)**, the public PlayStation specifications. The BIOS is **OpenBIOS** from [nugget](https://github.com/pcsx-redux/nugget) (MIT), by the PCSX-Redux authors. The hardware test programs are **[ps1-tests](https://github.com/JaCzekanski/ps1-tests)** (MIT) by Jakub Czekański, built with [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) (MPL-2.0).

### License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license, beside it: [OpenBIOS](bios/openbios/README.md) (MIT, with BSD parts), and the test programs of [ps1-tests](tests/ps1-tests/README.md) (MIT, with PSn00bSDK's libraries under MPL-2.0, which never reach the app).
