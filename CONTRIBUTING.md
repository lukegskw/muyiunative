# Contributing

Issues and pull requests are welcome. This page says how a change gets in; what the code does and where things are is in [AGENTS.md](AGENTS.md), [plan.md](plan.md) and [docs/development.md](docs/development.md).

## Reporting a problem

Choose Report a Problem in the app menu to reveal the current Finder run's log and open the bug form with its app version, commit, Mac/macOS, game serial and renderer filled in. Describe what happened and how it was built and run. Review the log for personal information and game data before attaching it. About muyiunative also shows the version and commit. For a terminal run, review and copy its output; headless checks keep their output in the terminal or calling build tool. General game results belong in the [compatibility form](https://github.com/lukegskw/muyiunative/issues/new?template=compatibility_report.yml), with exactly what was checked. Vulnerabilities use [private reporting](SECURITY.md).

## Never game data

Never commit or attach disc images, BIOS dumps, memory cards, save states, generated game code, game apps or anything extracted from a game or a BIOS. Reviewed logs, screenshots, serials, hashes and addresses can describe a result without sharing those files.

## Clean room

muyiunative is MIT-licensed and written from public documentation of the hardware ([psx-spx](https://psx-spx.consoledev.net/)) and from observation. Do not copy, translate or closely paraphrase code from emulators or recompilers whose license is not permissive, and do not write a part after reading such code for it. Third-party code must be under a permissive license (MIT, zlib, BSD) and keeps its license file beside it ([AGENTS.md](AGENTS.md)). The one exception is the test programs of [ps1-tests](tests/ps1-tests/README.md), which carry PSn00bSDK's libraries under MPL-2.0: the runtime runs them as a console runs a disc, and nothing of them is linked into muyiunative or put in its app ([decision log](plan.md#8-decision-log)).

## Before you start

- Read [AGENTS.md](AGENTS.md), the rules, and the current phase in [plan.md](plan.md#4-phases-and-exit-criteria).
- Prefer a fix that holds for every game to a game-specific rule, and fix a bug at its cause.

## A pull request

- **One change per PR, off `main`.** Rebase on `main` rather than merging it in.
- **Commits.** Each commit message says what changed and why, in plain English sentences: the symptom, the cause, the fix, and what was checked.
- **Tests.** A change comes with the test that shows it works. Run the [tests](docs/development.md#tests) and, for a Python change, ruff and pyright ([Continuous integration](docs/development.md#continuous-integration)); CI runs the same.
- **Docs.** Where a change makes plan.md, docs/development.md or AGENTS.md wrong, update them in the same PR; a new decision gets its row in plan.md's decision log.
- **The PR text.** Fill in the template: what and why, what was checked, which docs changed.

## Large changes and other platforms

- **Agree on the scope first.** Open an issue before a change that adds a platform, adds a second implementation of an existing path, or runs to more than about 1,000 lines.
- **Supported** are Macs with Apple silicon and macOS 14 or later; the iPhone comes in phase 8. Intel Macs or older systems would add paths that every later change has to keep working: ask in an issue first.

## How changes get in

The maintainer reviews every PR. A PR may be taken in as a squashed or reworked commit on `main` that credits its author (`Co-authored-by`).
