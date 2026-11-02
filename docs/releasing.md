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

## Build and validate a candidate

Use the supported Xcode/Metal toolchain and Python from [Development](development.md#how-it-is-built). Run the required local suites and ensure CI passes. Commit the reviewed release sources first: a distributable must identify a clean, complete source revision. The script refuses a dirty checkout and existing output/checksum paths.

Choose a new output path and your local identity, for example:

```sh
scripts/release_mac.py \
  --identity 'Developer ID Application: Your Name (TEAMID)' \
  --notary-profile muyiunative-notary \
  --output /external/output/muyiunative-0.7.0-mac-arm64.zip
```

The command:

1. Checks the local identity and notarization authentication before building.
2. Builds the launcher and its source kit in a unique ignored workspace, with version/build/commit metadata from the clean checkout.
3. Checks the kit allowlist, source-built OpenBIOS, precompiled metallib, arm64 executable and bundled 60-frame OpenBIOS boot. A game app or unexpected resource is refused.
4. Removes debug symbols from the release executable, then signs the complete launcher with Developer ID, hardened runtime and a secure timestamp.
5. Submits only the launcher to Apple's notary service, waits for `Accepted`, and staples/validates the ticket on the app. A rejected submission produces no final download.
6. Creates the final ZIP after stapling, extracts a new copy, and checks its signature, ticket, Gatekeeper assessment, kit and bundled boot again.
7. Writes the ZIP and adjacent `.zip.sha256`, then removes the temporary workspace. Existing archives are never replaced.

The app contains one Mach-O executable, system-framework dependencies, the precompiled Metal library, OpenBIOS and the signed source kit. There is no nested Python runtime to sign. The source kit is sealed by the app signature; game data and gallery images are excluded. Notarization uploads the generic launcher only. The ZIP is recreated after stapling so a downloaded copy carries the ticket rather than depending on another online lookup. [Apple's distribution requirements](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution) explain Developer ID, timestamping, hardened runtime and stapling.

The default tests simulate Apple signing/notary commands with synthetic host bytes. They verify accepted/rejected submissions, final-ticket packaging, extracted-copy checks, checksum correctness, source cleanliness and refusal to replace downloads or distribute game resources. Those tests do not claim a real notarization succeeded; record the real candidate's commit, SHA-256 and validation only after it does.

## Initial private release and player acceptance

The initial `v0.7.0` preview was published privately after candidate review and explicit approval, targeting the exact source commit in the app and marked as a pre-release. For every release, attach **only the generic launcher ZIP and its checksum**. Never attach a game app, generated game code, disc, capture, memory card or save state. Release publication and repository visibility remain separate approval steps.

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

| Field                    | Value                                                              |
| ------------------------ | ------------------------------------------------------------------ |
| Source commit            | `4813e3b79adb`                                                     |
| App identity             | 0.7.0 (build 64)                                                   |
| Apple submission         | `0d644f1d-448b-45e6-b713-8f89681d042a`, Accepted                   |
| Final launcher archive   | `muyiunative-0.7.0-mac-arm64.zip`                                  |
| Final ZIP SHA-256        | `8608cf6a84cca176f1bc98df8b1a0ed4a8c31ecc06c49fe643ea8e765cd08a2f` |
| Extracted-copy checks    | Signature, stapled ticket, Gatekeeper, kit integrity, bundled boot |
| OpenBIOS 60-frame hash   | `49d1a17d49708cb1`                                                 |

This is a private candidate built from the release implementation commit, before merging the tooling. It has not been published to GitHub Releases and does not complete the downloaded-launcher player check. The final release must identify its actual reviewed source commit and repeat the same packaging/validation flow; merging a later documentation or implementation commit does not silently retarget an existing archive.

## Released preview and player acceptance

[v0.7.0](https://github.com/lukegskw/muyiunative/releases/tag/v0.7.0) was published on 2026-11-02 from merged commit `a6c39675c4b3541b777efb0dde9aa3a5c0030b2d`, version 0.7.0 (build 66). The final archive is `muyiunative-0.7.0-mac-arm64.zip`, SHA-256 `584fa79917d932430df540586471d46cabafc7149f786569c07a780fa9d5ee39`. Apple accepted submission `12c59a63-89bb-455e-b518-014227e9874a`; the extracted archive passed signature, stapled-ticket, Gatekeeper, kit and bundled boot checks. The ZIP and adjacent checksum were downloaded back from GitHub and matched the local archive.

On the same date, the user confirmed the requested downloaded-launcher test worked: build through the GUI without a terminal and controller play. The resulting Metal Slug X (USA), SLUS-01212 app identifies source `a6c39675c4b3`, 0.7.0 (build 66), in its metadata, matching the published launcher and build log. This completes the player acceptance criterion; it does not extend the previously verified gameplay route or establish whole-game, fresh-Mac, wired or two-player compatibility. The user then authorized making the repository public.
