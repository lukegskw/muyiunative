# ps1native

Play your own PlayStation (PS1) discs natively on the Mac, and later on the iPhone. Bring your game (a `.cue`/`.bin` dump of your disc); ps1native turns it into an app.

The aim is native code, not emulation: the game's MIPS code is translated to C and compiled ahead of time on your Mac, and runs as Apple-silicon code beside a runtime that models the console's hardware; an interpreter covers the code that could not be compiled ahead of time. Nothing from any game or BIOS is in this repository, and the project is not affiliated with Sony Interactive Entertainment or any game publisher.

## Status

Phase 0 of [the plan](plan.md#4-phases-and-exit-criteria): the repository, CI and an empty Mac app. Nothing plays yet.

## Build from the command line

You need a Mac with Apple silicon, [Xcode](https://apps.apple.com/app/xcode/id497799835) 27.1 and Python 3.12 or newer. In a clone of this repository:

```
make -C shell/mac app
open shell/mac/build/ps1native.app
```

The app opens an empty, black window. It runs on macOS 14 or later.

### For developers

[Plan](plan.md) · [Development](docs/development.md) · [Contributing](CONTRIBUTING.md) · [Notes for agents](AGENTS.md)

### Acknowledgements

ps1native takes its build, its Mac front end and its rules for contributors and agents from **[nxnative](https://github.com/seedds/nxnative)** (MIT), which runs Switch games natively on Apple platforms. The console's hardware is modelled from **[psx-spx](https://psx-spx.consoledev.net/)**, the public PlayStation specifications.

### License

MIT ([LICENSE](LICENSE)).
