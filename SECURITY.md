# Security Policy

## Scope

Orion (repository `AppleMusic-OpenHome`) is a local Windows DLL injected into the
Apple Music process. It hosts a small HTTP server on the LAN for DLNA rendering,
installs a scheduled task, and writes to `%PUBLIC%\AMExclusive`.

In scope:

- Memory-safety defects in the hook, PCM pipeline or HTTP stream server.
- The DLNA HTTP endpoint being reachable or exploitable beyond its intended
  local-network renderer use.
- Privilege or persistence issues in the installer / companion registration.
- Unsigned or unverified payload handling in the setup program.

Out of scope:

- Anything requiring physical access to the user's own machine.
- Apple Music behaviour itself; report that to Apple.
- Feature requests (use a normal issue).
- Claims that hooking a third-party application is itself a vulnerability — see
  the [disclaimer](README.md#license-and-disclaimer).

## Reporting

**Please do not open a public issue for a vulnerability.** Use GitHub's
**Security → Report a vulnerability** (private vulnerability reporting) on
[this repository](https://github.com/dxdxdxlm123/AppleMusic-OpenHome/security/advisories/new),
or contact the maintainer privately through their GitHub profile.

Include:

- Affected version or commit, Windows version, Apple Music version.
- Output mode (`wasapi` or `dlna`) and renderer model.
- Steps to reproduce and, if safe, a minimal proof of concept.
- Redacted logs (see [CONTRIBUTING.md](CONTRIBUTING.md)) — never include real
  device UDNs, LAN addresses, or personal media.

You can expect an acknowledgement within 7 days. If a report is accepted we will
agree on a disclosure timeline with you (normally 90 days), ship a fix, and credit
you in the release notes unless you prefer to stay anonymous.

## Supported versions

| Version | Supported |
|---|---|
| `0.2.x` | ✅ |
| `0.1.x` (upstream baseline) | ❌ — report to [upstream](https://github.com/AntiOblivionis/AMExclusive) |

## Notes on the trust model

- The quality lock patches private code only after verifying a SHA-256 image
  fingerprint. An unrecognised Apple Music build disables the lock rather than
  detouring unknown code.
- The installer is **not code-signed**; Windows SmartScreen will warn. Always
  verify the SHA-256 published in the GitHub Release before running it.
- The DLNA HTTP server binds inside the Apple Music process and serves only the
  active track's PCM stream. It is intended for a trusted local network.
