# Games

Compatibility is recorded for a particular disc revision and route, not inferred from having a profile. The [README gallery](../README.md#compatibility) links to each game's notes below. Progress beyond these routes remains unverified.

| Game                                                     | Serial     | Region            | Verified progress                     | Platform |
| -------------------------------------------------------- | ---------- | ----------------- | ------------------------------------- | -------- |
| [Metal Slug X](SLUS-01212/README.md)                     | SLUS-01212 | USA               | Mission 1 through its results         | Mac      |
| [Final Fantasy IX, Disc 1](SLUS-01251/README.md)         | SLUS-01251 | USA               | First battle in the cargo ship        | Mac      |
| [Crash Bandicoot 3 - Warped](SCES-01420/README.md)       | SCES-01420 | Europe, Australia | Opening cutscene and Time Twister hub | Mac      |
| [Gran Turismo 2 - Simulation Mode](SCUS-94488/README.md) | SCUS-94488 | USA               | Japan Nationals, Midfield replay      | Mac      |
| [WipEout 3 - Special Edition](SCES-02845/README.md)      | SCES-02845 | Europe            | Porto Kora race start                 | Mac      |

Each `profiles/<serial>/` folder keeps `game.toml`, the boot revision and reproducible input routes, beside a `README.md` describing what was tested and what remains open. Game-specific knowledge belongs here; the runtime applies general rules. Adding a profile alone does not add a verified game to the gallery.

To share results, use the [compatibility report](https://github.com/lukegskw/muyiunative/issues/new?template=compatibility_report.yml), including the boot executable SHA-256, app version/commit, renderer, controller and the levels or modes actually checked. Never upload a disc, generated code, game app, card or save state. Selected gallery images are captured output, with their provenance recorded in [docs/images](../docs/images/README.md); the routine frame-comparison images remain local.
