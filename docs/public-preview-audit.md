# Public preview history audit

Audit date: 2026-11-02. Audited `main`: `64ed085fbefdcdd39d9cb9457e19a4a9d1df6a6a`, the merge of launcher-build PR #22. This is a record of the pre-publication review, not a claim that the repository is already public or that its settings have been changed.

## Scope and method

Fetched remote branches and tags, and all advertised GitHub `refs/pull/*/head` into local audit refs. `git rev-list --all` covered **63 commits**, including earlier/deleted branch histories still reachable through local refs or pull-request heads. Enumerating every commit's tree found **494 unique file blobs**, totaling **5,638,825 uncompressed bytes**. Repeated versions were scanned once per object; deleted files and alternate paths remained in scope.

Reviewed all historical paths, binary signatures and object sizes. Scanned every blob, plus commit/tag contents, for private-key blocks, GitHub/AWS/Google/Slack/Stripe/SendGrid/service token formats, JWTs, credential-bearing URLs and quoted credential assignments. Candidate output contained only object IDs, paths, pattern names and line numbers, never credential values. Checked every commit's author and committer email against `lucgiglio@outlook.com`, `noreply@github.com` and `*@users.noreply.github.com`.

This was a local standard-library pattern scan and manual review, not a proof that unknown or encoded secrets cannot exist. Unreachable local objects, ignored working files, issue attachments and GitHub's internal storage are outside the Git-history count. Repeat the review for new content before publication; a history rewrite was neither necessary nor performed.

After this baseline review, the user approved a narrow compatibility-gallery exception on 2026-11-02: selected actual gameplay screenshots with [capture provenance](images/README.md). The follow-up presentation change adds one unmodified 108,991-byte PNG, `docs/images/metal-slug-x.png`, SHA-256 `0dbc03530cc4f5fe7f5fd1416ea7d03085b1a84be740fa1e38e3ffefb58932c2`. It is an approved gallery image, not an unexplained binary or a test input. The baseline counts and findings below describe 64ed085 before that change; they do not claim the later gallery is image-free.

A supplemental read-only check of all 22 issue/pull-request bodies available on that date found no candidates from the same credential patterns and no attachment or gist links. There were no issue comments or pull-request review comments returned by their repository endpoints. This does not turn the Git audit into a guarantee about GitHub's internal storage.

## Findings

| Area              | Result                                                                                                                                                     |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Game data         | No retail disc, BIOS dump, card, save state, generated game code or screenshot found. ISO `CD001` matches are parser/synthetic-test literals and OpenBIOS. |
| Credentials       | No credential candidates from the listed patterns. No credential files or unexplained archives found in the historical paths.                              |
| Commit identities | All 63 authors use `lucgiglio@outlook.com`; 43 committers use that address and 20 merge committers use `noreply@github.com`. No corporate email found.     |
| Large objects     | No blob exceeds 1 MiB. The largest is the homebrew GTE test, 671,744 bytes; OpenBIOS is 524,288 bytes; the next is a homebrew GPU test, 77,824 bytes.      |
| Binary provenance | All 19 binary blobs accounted for: one OpenBIOS image and 18 upstream homebrew test executables.                                                           |

OpenBIOS's SHA-256 is `caf2162eb498408f86e19f9b3a3c5e083e34c25a43baae33ffc3641afffa54c8`, matching its [documented source build](../bios/openbios/README.md). All 18 historical homebrew executable blobs and seven console-log blobs are byte-identical to files in upstream [ps1-tests build-158](https://github.com/JaCzekanski/ps1-tests/releases/tag/build-158). The downloaded `tests.zip` has SHA-256 `5de0e82854c1a511697a7ade381c2d778ec4b2fff4a7d7ac79e8098c35eb859b`, as recorded in the [test provenance](../tests/ps1-tests/README.md). No retail files were needed for the audit.

## Baseline publication status

The repository was private during this review. The private-vulnerability-reporting API returned 404 for the private repository, so this audit does not claim it is enabled. Making the repository public, enabling private vulnerability reporting, protecting `main` and publishing a release each await the maintainer's explicit approval. No repository setting was changed by this PR.

The intended `main` protection requires these exact checks: `Mac app`, `Runtime and Mac host tests (plain, then ASan and UBSan)`, `Tool tests`, and `ruff and pyright`. Both workflows must run on every pull request; path-filtered workflows can leave a required check pending ([GitHub documentation](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks)). On GitHub Free this protection is available for public repositories ([protected branches](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches)). Enable and verify [private vulnerability reporting](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/configure-vulnerability-reporting/configure-for-a-repository) when publication is authorized.

At this audit baseline the per-log size cap, opt-in guest output and release format were undecided. On 2026-11-02 the user subsequently approved 10 MiB per run, session-only guest output opt-in and a Developer ID-signed, notarized launcher ZIP; [Releasing](releasing.md) records the implementation and pending real checks. The repository remains private until the downloaded-launcher/controller acceptance check succeeds, followed by explicit approval for public visibility and repository security settings. Repeat this baseline audit over the latest refs and actual release before publication.

## Final publication review

On 2026-11-02 the user confirmed the downloaded 0.7.0/build 66 launcher flow and authorized making the repository public, with a description and topics. The release targets `a6c39675c4b3541b777efb0dde9aa3a5c0030b2d`. A fresh fetch of remote branches, tags and all advertised pull-request heads covers **69 commits and 533 unique file blobs** at that source snapshot. The same local pattern scan and manual provenance review found no credential candidates, suspicious file paths or disallowed author/committer addresses. All 20 binary objects are accounted for: OpenBIOS, 18 upstream homebrew executables and the explicitly approved gallery PNG. No committed blob exceeds 1 MiB, and no history rewrite is needed.

The final launcher ZIP was also checked: only the generic launcher is distributed, its source kit uses the allowlist, and there is no game manifest, disc directory or gallery image in it. The notarized release downloaded from GitHub matches its published SHA-256, `584fa79917d932430df540586471d46cabafc7149f786569c07a780fa9d5ee39`. Account/signing keys and notary credentials remain in the local keychain, outside Git and the release.

This refresh uses the same bounded method and limitations as the baseline. It does not prove that an unknown or encoded secret cannot exist. Public visibility is authorized only after the successful player check; the publication setup includes private vulnerability reporting and protection of `main` with the four named CI checks above.

Publication was then applied and verified on 2026-11-02: visibility is public, the requested description and 13 relevant topics are present, and the private-vulnerability-reporting endpoint reports `enabled: true`. `main` requires all four named checks from GitHub Actions (app 15368), including for administrators; force-push and deletion are disabled. The policy does not add a second-reviewer requirement or require a branch refresh after every `main` change. A final read-only scan of the 24 issue/PR bodies found no credential-pattern candidates or attachment/gist references; repository issue and review comment endpoints returned no entries.
