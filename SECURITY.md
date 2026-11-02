# Security

muyiunative is an early Mac preview. Include the exact build information in a report so the problem can be reproduced.

## Report a vulnerability privately

Use GitHub's [Report a vulnerability](https://github.com/lukegskw/muyiunative/security/advisories/new) on the repository's Security tab. Include the app version and commit, Mac/macOS version, affected build or playback step, expected impact and reproduction steps using a synthetic input where possible. Do not open a public bug or compatibility report with vulnerability details.

Private vulnerability reporting must be enabled by the maintainer when the repository becomes public. If GitHub does not offer the reporting button, open an ordinary issue asking for a private reporting channel, without including vulnerability details or sensitive files. Do not attach copyrighted game data, even to a private report.

## Main attack surface

Disc images are untrusted input. CUE metadata, track contents, ISO directories and executable headers pass through the Python tools and C runtime during capture, code generation and playback. Malformed images may expose parser errors, memory corruption or excessive resource use. The interpreter and generated native code model the guest; this is not a security sandbox for hostile input. Builds also invoke the local compiler with the user's permissions.

Use your own disc dumps and launchers obtained from the project. A game app contains a local copy of its disc and must stay on your Mac. The launcher's signature and build-kit manifest protect the bundled tools' integrity; they do not establish that a disc is safe. Logs can contain guest output and paths beyond the home-directory redaction; review them before sharing. There is no automatic upload of logs or disc contents.
