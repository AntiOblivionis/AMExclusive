# Contributors

## Upstream — AntiOblivionis/AMExclusive

The original project and its contributors retain full credit. This fork keeps
the complete upstream commit history, so upstream authors also appear in
GitHub's contributor statistics for this repository. See the
[upstream Contributors page](https://github.com/AntiOblivionis/AMExclusive/graphs/contributors)
for the authoritative list.

Original copyright: `Copyright (c) 2026 AMExclusive contributors` — preserved
verbatim in [LICENSE](LICENSE).

## This fork — AppleMusic-OpenHome

**[@dxdxdxlm123](https://github.com/dxdxdxlm123)** — maintainer and author of
the derivative work (7 commits in the `v0.2.0` release):

- DLNA / OpenHome renderer output, SSDP discovery and format negotiation
- File-shaped live WAV streaming (`Content-Length`, `Accept-Ranges`, `206`)
- True track duration from the Windows media session
- Forced Hi-Res tier and a second verified CoreMedia build profile
- SHA-256 image fingerprints instead of embedded instruction bytes
- Locked real-time buffers and raised producer-thread priority
- Documentation, compliance files, CI/release engineering and prebuilt installers

A full change list is in [CHANGELOG.md](CHANGELOG.md) and [NOTICE.md](NOTICE.md).

## AI assistance

Part of this fork was developed with AI assistance. Every change was reviewed,
compiled, and validated on a physical AURALiC VEGA G2.2 renderer before being
committed. The upstream commit history also contains AI co-author credits.

## Why GitHub's sidebar may lag

GitHub computes contributor statistics asynchronously. The
`/stats/contributors` endpoint already reports `dxdxdxlm123` with 7 commits,
while the repository sidebar reads a slower-cached `/contributors` endpoint that
can take several hours to refresh — it may temporarily show only the upstream
authors inherited through the fork. Every commit in this fork is correctly
attributed (`author.login = dxdxdxlm123`), so this is a display cache, not an
attribution problem.

Note also that GitHub does not count commits made **in a fork** toward your
personal profile contribution graph unless they are merged into the upstream
repository via a pull request. That is a separate rule and does not affect this
repository's contributor statistics.
