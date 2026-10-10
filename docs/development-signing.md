# Mac signing and permission identity

GlideBeam retains the bundle identifier `dev.remote-native.host`, executable `RemoteHost`, and existing `RemoteNative` configuration paths. Changing the public display name does not intentionally create a new application identity.

## Default builds

Without an explicitly configured local identity, `build-macos.sh` applies an ad-hoc signature and runs `codesign --verify --strict`. Hosted CI disables automatic use of local development signing. Public artifacts do not contain a signing private key or a development keychain.

An ad-hoc signature can change its designated requirement when its code changes. macOS may therefore require Screen Recording or Accessibility permission again after an update. Reusing a stable development identity is useful for iterative local deployments, but does not constitute Developer ID distribution or notarization.

## Existing local development identity

Local builds can reuse a previously configured, dedicated signing identity from `~/Library/Application Support/RemoteNative/signing`. That private directory contains a dedicated keychain, the identity identifier, and its unlock material. Preserve it locally and keep it out of source, archives, and logs.

`REMOTE_SIGNING_IDENTITY` and `REMOTE_SIGNING_KEYCHAIN` can explicitly select an already configured local identity. The signing helper temporarily adds the dedicated keychain to the search list and restores the previous list. `REMOTE_USE_LOCAL_SIGNING=0` disables automatic local reuse. The build does not change TCC databases, SIP, or certificate trust settings.

Keep the same certificate/private key and bundle identifier when preserving an existing deployed identity. Moving from ad-hoc signing to a new certificate identity can still require initial permission approval. Recreating the certificate also changes that identity. Actual permission continuity should be checked on the target Mac after the update.

See Apple's [Code Signing Tasks](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/Procedures/Procedures.html), [TN2206](https://developer.apple.com/library/technotes/tn2206/), and [TCC identity discussion](https://developer.apple.com/forums/thread/663889) for the platform rules.
