# Security policy

NVMetrics RM control is experimental, not a certified public release. Do not use
unattended manual fan control or install on unqualified GPU/driver combinations.
Report vulnerabilities privately through the repository's security reporting
feature where available. Do not post session tokens, private hardware identifiers,
or privileged logs publicly.

Manual fan control requires a verified AUTO baseline, valid telemetry, two
tachometers, a guarded written/read-back transaction and independent rescue.
If AUTO cannot be verified, stop further manual operation and investigate locally.

The current Unix socket authenticates kernel peer UID, not app identity. A process
running as the authorized desktop user is within this trust boundary. Signed XPC
with code-signing requirements, Developer ID signing/notarization, hardware ABI
qualification and an external review remain prerequisites to public release.
