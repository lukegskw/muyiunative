# Compatibility gallery images

The user approved selected screenshots of games running in muyiunative for the compatibility gallery on 2026-11-02. This is the narrow presentation exception in [AGENTS.md](../../AGENTS.md): routine frame-comparison screenshots stay local, and disc images, extracted game code, BIOS dumps, cards and save states remain excluded. Gallery images are never runtime or tool-test inputs and are not included in the launcher's build kit.

The [main gallery](../../README.md#compatibility) links each image to its [game notes](../../profiles/README.md). Images show actual application output, with no generated artwork, retouching or simulated UI. The layout follows [nxnative's gallery](https://github.com/seedds/nxnative#games); add another entry when its verified scope and capture provenance are available.

## Metal Slug X

![Metal Slug X (USA), Mission 1 running in muyiunative](metal-slug-x.png)

| Capture field        | Value                                                                 |
| -------------------- | --------------------------------------------------------------------- |
| File                 | `metal-slug-x.png`, 108,991 bytes                                     |
| Captured             | 2026-11-02, Apple M4 Pro, macOS 26.6.2                                |
| App                  | User-built launcher game app, 0.7.0 (build 59), commit `2beb751d663e` |
| Game / region        | Metal Slug X / USA, SLUS-01212                                        |
| Boot executable hash | `0c36eb1775c844e991fe136d6e1a66dad8d79eeda4be1bdd577715fc35af98e0`    |
| Route                | First 7,000 presented frames of the profile's `first_level` route     |
| Renderer / output    | Metal 4×, original unmodified PNG, 1280×960                           |
| Display FNV-1a hash  | `9e695abc708ab89c`                                                    |
| PNG SHA-256          | `0dbc03530cc4f5fe7f5fd1416ea7d03085b1a84be740fa1e38e3ffefb58932c2`    |

The inputs and pulses in [game.toml](../../profiles/SLUS-01212/game.toml) were expanded by [replay.py](../../tools/replay.py), stopping at 7,000 frames. The existing game app then ran headlessly with `--input-file <expanded-route> --frames 7000 --renderer metal --scale 4 --verify-renderer --png <output>`. Native Metal VRAM/cache agreed with software on every frame; all 1,972,644,276 CPU instructions ran natively. This capture check does not extend the game's [verified scope](../../profiles/SLUS-01212/README.md).

The local game app and its bundled disc were read without modifying them; headless capture used no persistent memory cards. Only this selected output image is committed, not the disc, generated code or raw run log. The depicted game's artwork remains its rights holders' work; the project's MIT software license does not license that artwork.
