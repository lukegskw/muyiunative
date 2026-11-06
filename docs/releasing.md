# Releasing the Mac preview

The selected format is a **Developer ID-signed, notarized ZIP** of the generic launcher, with a SHA-256 checksum. The user's paid Apple Developer account supplies the certificate. Players need no developer account, signing key or separate Python. Their locally compiled game apps are ad-hoc signed on their own Mac; neither their discs nor their game apps are sent to Apple or GitHub by the build flow.

The repository stayed private through the downloaded-launcher player check, which the user confirmed on 2026-11-02, then became public with explicit approval. Private vulnerability reporting and `main` protection are enabled. Merging release tooling does not authorize publishing a release or changing repository visibility; those actions need the maintainer's explicit approval.

## One-time maintainer setup

Create a local **Developer ID Application** signing identity in Xcode → Settings → Accounts → select the paid team → Manage Certificates → + → Developer ID Application. The certificate and its private key must be available in the local keychain. [Apple's certificate instructions](https://developer.apple.com/documentation/xcode/sharing-your-teams-signing-certificates) and [Developer ID requirements](https://developer.apple.com/help/account/certificates/create-developer-id-certificates/) describe the Account Holder role. A cloud-only certificate is not a local command-line signing identity.

Configure `notarytool` credentials interactively in your keychain, for example:

```sh
xcrun notarytool store-credentials muyiunative-notary
```

Follow Apple's prompts for your account, team and app-specific password, or use an existing API-key-based profile. Enter secrets only in the local credential setup; never put a password, private key or certificate export in the repository, a command in these docs, an issue or chat. The release script takes only the profile's name. [Apple's notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow) documents credential profiles and submission.

This is a maintainer operation on the Mac holding the signing identity. No signing keys or notarization credentials are bundled in the launcher or copied to GitHub Actions by this flow.

The updater also requires the Sparkle EdDSA key in the local Keychain account `org.muyiunative.updates`. Its public key is pinned in [the Sparkle configuration](../tools/vendor/sparkle/config.json); the initial key was created on the maintainer's Mac on 2026-11-06. Keep a private offline backup through Sparkle's `generate_keys --account org.muyiunative.updates -x <private-local-file>` tool, and restore it through `-f` when moving release machines. Never attach or commit that private file. The tools are fetched by `make -C shell/mac app` into `shell/mac/build/sparkle/bin/`. Keychain may ask the maintainer to allow `sign_update` access. The release command checks that the local public key matches the source configuration before building. See [Sparkle's key setup and signing documentation](https://sparkle-project.org/documentation/).

## Build and validate a candidate

Use the supported Xcode/Metal toolchain and Python from [Development](development.md#how-it-is-built). Run the required local suites and ensure CI passes. Commit the reviewed release sources first: a distributable must identify a clean, complete source revision. The script refuses a dirty checkout and existing output/checksum paths.

Choose a new output path and your local identity, for example:

```sh
scripts/release_mac.py \
  --identity 'Developer ID Application: Your Name (TEAMID)' \
  --notary-profile muyiunative-notary \
  --output /external/output/muyiunative-0.9.1-mac-arm64.zip
```

The command:

1. Checks the local identity and notarization authentication before building.
2. Builds the launcher and its source kit in a unique ignored workspace, with version/build/commit metadata from the clean checkout.
3. Checks the kit allowlist, source-built OpenBIOS, precompiled metallib, arm64 executable and bundled 60-frame OpenBIOS boot. A game app or unexpected resource is refused.
4. Removes debug symbols from the release executable, then signs Sparkle's installer, relauncher and framework from the inside out, followed by the launcher, with Developer ID, hardened runtime and a secure timestamp.
5. Submits only the launcher to Apple's notary service, waits for `Accepted`, and staples/validates the ticket on the app. A rejected submission produces no final download.
6. Creates the final ZIP after stapling, extracts a new copy, and checks its signature, ticket, Gatekeeper assessment, kit and bundled boot again.
7. Signs the ZIP with the Sparkle Keychain key, creates and signs the adjacent `.appcast.xml`, then writes the ZIP, `.zip.sha256` and signed appcast as new files. Existing outputs are never replaced.

The app contains one arm64 host executable, the pinned Sparkle framework and its two installer/relauncher executables, the precompiled Metal library, OpenBIOS and the signed source kit. Sparkle's unused sandbox XPC services are omitted. Framework symlinks must resolve inside the framework; unexpected Mach-O files are refused. There is no nested Python runtime to sign. The source kit is sealed by the app signature; game data and gallery images are excluded. Notarization uploads the generic launcher only. The ZIP is recreated after stapling so a downloaded copy carries the ticket rather than depending on another online lookup. [Apple's distribution requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution) explain Developer ID, timestamping, hardened runtime and stapling.

The default tests simulate Apple signing/notary commands with synthetic host bytes. They verify accepted/rejected submissions, final-ticket packaging, extracted-copy checks, checksum correctness, source cleanliness and refusal to replace downloads or distribute game resources. Those tests do not claim a real notarization succeeded; record the real candidate's commit, SHA-256 and validation only after it does.

## Initial private release and player acceptance

The initial `v0.7.0` preview was published privately after candidate review and explicit approval, targeting the exact source commit in the app and marked as a pre-release. New releases attach **the generic launcher ZIP, its checksum and its signed `.appcast.xml`**, using the generated filenames unchanged and a `v<version>` tag. Never attach a game app, generated game code, disc, capture, memory card or save state. Release publication and repository visibility remain separate approval steps.

After publication, [the update-feed workflow](../.github/workflows/updates.yml) selects the published signed appcast with the highest build number, including previews, and copies it verbatim to `appcast.xml` on the dedicated `updates` branch. The launcher's feed URL targets that file; no server or signing secret is required in Actions. The job alone has contents-write permission, and does not write to protected `main`. It runs again when releases are edited/deleted and supports manual dispatch. Releases predating this updater have no appcast and are skipped. With none present, the workflow publishes [the signed empty feed](../updates/empty-appcast.xml). Verify the update-feed job after publishing; until this workflow is merged and a signed release is published, the public updater feed is not active.

Ask the user to complete this check on the Mac with their own disc:

1. Download the candidate ZIP through GitHub in the browser and extract it in Finder. Use this download, retaining its ordinary download attributes.
2. Move the launcher to Applications and open it through the normal macOS first-launch confirmation. An invalid-signature, damaged-app or failed-verification message is a failed check.
3. Check About muyiunative for the expected version/build/commit.
4. Choose Build Game App, select the CUE/BIN and choose an output app. If CLT is missing, use Install Tools and resume after Apple's installer finishes. No terminal is needed for the player.
5. Follow the build to success, open the produced game app and play with the connected controller. Record the game/serial and the controls and scene actually checked.
6. Verify that Report a Problem reveals the current log with the same source identity; review the bounded-log and session debug-output behavior.

Only after this succeeds, record the date, source commit, game and checked behavior in [plan.md §4](../plan.md#4-phases-and-exit-criteria) and [Development's phase-7 section](development.md#phase-7-the-public-preview). The user's earlier source-built-launcher checks do not substitute for a downloaded candidate.

Before repository publication, the [history audit](public-preview-audit.md) was repeated for the latest refs and release content, including the explicitly approved gallery image. The successful player check was followed by the user's public-visibility go, with private vulnerability reporting and `main` protection requiring all four CI checks. The release workflow never changes those settings itself.

## Validated candidate

On 2026-11-02, the real local signing/notarization pipeline passed on the M4 Pro with macOS 26.6.2:

| Field                  | Value                                                              |
| ---------------------- | ------------------------------------------------------------------ |
| Source commit          | `4813e3b79adb`                                                     |
| App identity           | 0.7.0 (build 64)                                                   |
| Apple submission       | `0d644f1d-448b-45e6-b713-8f89681d042a`, Accepted                   |
| Final launcher archive | `muyiunative-0.7.0-mac-arm64.zip`                                  |
| Final ZIP SHA-256      | `8608cf6a84cca176f1bc98df8b1a0ed4a8c31ecc06c49fe643ea8e765cd08a2f` |
| Extracted-copy checks  | Signature, stapled ticket, Gatekeeper, kit integrity, bundled boot |
| OpenBIOS 60-frame hash | `49d1a17d49708cb1`                                                 |

This is a private candidate built from the release implementation commit, before merging the tooling. It has not been published to GitHub Releases and does not complete the downloaded-launcher player check. The final release must identify its actual reviewed source commit and repeat the same packaging/validation flow; merging a later documentation or implementation commit does not silently retarget an existing archive.

## Released preview and player acceptance

[v0.7.0](https://github.com/lukegskw/muyiunative/releases/tag/v0.7.0) was published on 2026-11-02 from merged commit `a6c39675c4b3541b777efb0dde9aa3a5c0030b2d`, version 0.7.0 (build 66). The final archive is `muyiunative-0.7.0-mac-arm64.zip`, SHA-256 `584fa79917d932430df540586471d46cabafc7149f786569c07a780fa9d5ee39`. Apple accepted submission `12c59a63-89bb-455e-b518-014227e9874a`; the extracted archive passed signature, stapled-ticket, Gatekeeper, kit and bundled boot checks. The ZIP and adjacent checksum were downloaded back from GitHub and matched the local archive.

On the same date, the user confirmed the requested downloaded-launcher test worked: build through the GUI without a terminal and controller play. The resulting Metal Slug X, SLUS-01212 app identifies source `a6c39675c4b3`, 0.7.0 (build 66), in its metadata, matching the published launcher and build log. This completes the player acceptance criterion; it does not extend the previously verified gameplay route or establish whole-game, fresh-Mac, wired or two-player compatibility. The user then authorized making the repository public.

## Preview 0.8.0 artifact

On 2026-11-03 the user merged PR #26, confirmed quick-state saving/loading and corrected audio, and authorized a new preview. The signed launcher for [v0.8.0](https://github.com/lukegskw/muyiunative/releases/tag/v0.8.0) was built from the clean release-preparation commit below. Subsequent release-record documentation commits do not retarget that binary.

| Field | Value |
| --- | --- |
| Source commit | `6f2ab8e3fffc27a40aaf2ea66b4036f860674207` |
| App identity | 0.8.0 (build 72), commit `6f2ab8e3fffc` |
| Apple submission | `37e910fb-258f-4c72-a121-5435c8abf7ee`, Accepted |
| Launcher archive | `muyiunative-0.8.0-mac-arm64.zip` |
| ZIP SHA-256 | `ced07801e6dd72df2fc3116dd9423130ed53b8e79d0c0e9df68725350eac92cd` |
| Extracted-copy checks | Signature, stapled ticket, Gatekeeper, kit integrity, arm64 resources and bundled boot |
| OpenBIOS 60-frame hash | `49d1a17d49708cb1` |

The release contains the generic launcher and checksum only. Its source kit includes the FF IX and Crash profiles, shared CPU/Metal/audio fixes and quick states. Gallery images, game apps, discs, generated game code and saves are excluded. All 78 Python tests and pinned ruff/pyright passed during preparation; the real release pipeline verified the newly extracted ZIP after signing/notarization.

[Updating](../README.md#updating) requires replacing the launcher and rebuilding existing game apps. Normal memory-card saves retain their per-game identity. The earlier downloaded-launcher/controller acceptance check remains the 0.7.0 record above; no new fresh-Mac or full-game acceptance is inferred from this artifact validation.

## Preview 0.8.1 artifact

On 2026-11-04 the user confirmed the GT2/WipEout fixes were working and authorized the screenshots, commit/push and release. The signed launcher prepared for [v0.8.1](https://github.com/lukegskw/muyiunative/releases/tag/v0.8.1) identifies the clean implementation commit below. This subsequent artifact-record commit does not retarget the binary.

| Field | Value |
| --- | --- |
| Source commit | `7d0986bf8b25d700064372ecc971322cb99aa92c` |
| App identity | 0.8.1 (build 75), commit `7d0986bf8b25` |
| Apple submission | `557f3b96-d216-4a27-a1e4-14e05eb8eed6`, Accepted |
| Launcher archive | `muyiunative-0.8.1-mac-arm64.zip` |
| ZIP SHA-256 | `9e4cedb0fc5395955f00239b97c33bdd7ac9692a612d8b81641cdd362751c910` |
| Extracted-copy checks | Signature, stapled ticket, Gatekeeper, kit integrity, arm64 resources and bundled boot |
| OpenBIOS 60-frame hash | `49d1a17d49708cb1` |

The launcher kit includes the new GT2/WipEout profiles, audio recovery after rendering stalls, Metal triangle optimizations and the title-atlas correction. The gallery's approved game screenshots remain outside the kit and download; only the generic launcher ZIP and checksum are distributed. Local validation passes all 78 tool tests, pinned ruff/pyright, 1,800 runtime checks, 4,670 Mac checks and 8,270 Metal checks, including CLT ASan/UBSan runs. The two gallery captures also pass packaged-app per-frame native VRAM/cache comparisons from reset.

The player's confirmation covers the reported scenes and settings in the rebuilt apps. It does not replace the earlier downloaded-launcher acceptance record or establish whole-game, fresh-Mac or original-reporter M1 Max validation. Existing game apps must be rebuilt with the updated launcher to receive these runtime/shader changes.

## Preview 0.9.0 artifact

On 2026-11-06 the user authorized committing/pushing both feature branches and publishing a new preview with multi-disc apps and launcher updates. The signed launcher prepared for [v0.9.0](https://github.com/lukegskw/muyiunative/releases/tag/v0.9.0) identifies the clean implementation commit below. This subsequent artifact-record commit does not retarget the binary.

| Field | Value |
| --- | --- |
| Source commit | `2c6ea81d248eacb070ee337ffbd86f5d585f739b` |
| App identity | 0.9.0 (build 80), commit `2c6ea81d248e` |
| Apple submission | `648d5fed-e3c8-4a9b-9d40-321350f389ef`, Accepted |
| Launcher archive | `muyiunative-0.9.0-mac-arm64.zip` |
| ZIP SHA-256 | `931cb89c79e851b45b6868a6942baf36108fe2ea9563ec01665a0a855700b92d` |
| Extracted-copy checks | Signature, stapled ticket, Gatekeeper, kit integrity, arm64 resources and bundled boot |
| OpenBIOS 60-frame hash | `49d1a17d49708cb1` |

Publication uses the generic launcher ZIP, its checksum and its signed appcast. Normal game apps and game data remain excluded. The updater feed publishes signed metadata from the release, including previews. Existing published 0.8.1 launchers need a manual replacement for this first upgrade; subsequent versions can update from inside the launcher. Game apps continue to require a manual rebuild with the original disc set. Earlier quick states from published versions use an incompatible format; ordinary memory-card saves retain their game identity.

The implementation passes the 92-tool-test suite with the two release-version expectations updated for 0.9.0, pinned ruff and pyright, and isolated UI checks of automatic notification, changed-package rejection and full install/relaunch. The preceding multi-disc checks cover synthetic portable apps and the requested FFIX swaps reached through memory-card loading; they do not establish story-ending handoffs or whole-game compatibility.

## Preview 0.9.1 artifact

On 2026-11-06 the user confirmed the Final Fantasy VII movie fixes, requested its README gallery entry and authorized committing, merging and publishing the next preview. The signed launcher prepared for [v0.9.1](https://github.com/lukegskw/muyiunative/releases/tag/v0.9.1) identifies the clean implementation commit below. This subsequent artifact-record commit does not retarget the binary.

| Field | Value |
| --- | --- |
| Source commit | `dbb66903d96b76d3f2cb57438723318268b45304` |
| App identity | 0.9.1 (build 84), commit `dbb66903d96b` |
| Apple submission | `28a5923e-5390-47fd-9d69-96dd588d0d93`, Accepted |
| Launcher archive | `muyiunative-0.9.1-mac-arm64.zip` |
| ZIP SHA-256 | `42df3e55958c2aed92dd7ec901dc5b77dcfbcfd7b3b288d4b3ef9d5b4c6b4071` |
| Extracted-copy checks | Signature, stapled ticket, Gatekeeper, kit integrity, arm64 resources and bundled boot |
| OpenBIOS 60-frame hash | `49d1a17d49708cb1` |

The launcher kit includes the shared MDEC reset/processing corrections and the new SCES-00867 opening profile. The requested gameplay PNG is presentation only, with [capture provenance](images/README.md#final-fantasy-vii), and is excluded from the launcher kit and download. The release distributes the generic launcher ZIP, checksum and signed appcast; no game data is uploaded to Apple or GitHub.

Local verification passes runtime and Mac host tests normally and under CLT ASan/UBSan, all 92 tool tests, pinned ruff/pyright, 8,270 Metal checks and the documented FF VII interpreter/Metal/audio routes. The user confirmed the corrected movie and game locally; this does not establish later battles, disc-change handoffs or whole-game compatibility. Decoder processing time remains an explicit approximation informed by public console measurements. Existing game apps require a manual rebuild to receive the changes. Older quick states use an incompatible runtime layout; normal memory-card saves retain their game identity. The signed appcast and extracted archive pass local verification.
