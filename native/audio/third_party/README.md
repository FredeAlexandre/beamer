# Vendored third-party code (native/audio)

Each library is copied unmodified from its official release archive, keeping its own licence file.
All licences are permissive and compatible with Beamer's GPL-3.0 (`LICENSE` at the repo root):
BSD-3-Clause, BSD-2-Clause and MIT are on the FSF's list of GPL-compatible free licences, and
CC0-1.0 releases Monocypher to the public domain (its BSD-2-Clause option is the fallback). The
combined helper binary is distributed under GPL-3.0 with these notices kept.

| Directory | Version | Upstream archive | Verification | Licence | What is vendored |
|---|---|---|---|---|---|
| `opus-1.6.1/` | 1.6.1 | https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz | sha256 `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`, equal to xiph's published `SHA256SUMS.txt` and to Phase 1's `proto/transport` build | BSD-3-Clause (`COPYING`) | `celt/ silk/ src/ include/`, the `*_sources.mk` / `*_headers.mk` lists, `AUTHORS COPYING NEWS README package_version`. Not vendored: `dnn/` (22 MB of DRED/OSCE/deep-PLC models; the float build without `ENABLE_DEEP_PLC`/`ENABLE_DRED`/`ENABLE_OSCE` does not use it), autotools/CMake/meson files, `tests/`, `doc/` |
| `monocypher-4.0.2/` | 4.0.2 | https://monocypher.org/download/monocypher-4.0.2.tar.gz | SHA-512 `bf275d4c53ff94af6cdc723a4e002e9f080f4d1436c86c76bb37870b34807f1d7b32331d8ff8a1aeb369e946f3769021e03e63efac25b82efc5abf54dc084714`, equal to the published `monocypher-4.0.2.tar.gz.sha512` | BSD-2-Clause or CC0-1.0 (`LICENCE.md`) | `src/` (`monocypher.{c,h}`, `optional/` unused), `AUTHORS.md CHANGELOG.md LICENCE.md README.md` |
| `nlohmann-json-3.11.3/` | 3.11.3 | https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz | sha256 `d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d` and `json.hpp` sha256 `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`, both equal to the release notes; GPG signature `json.tar.xz.asc` good, key `7971 67AE 41C0 A6D9 232E 4845 7F3C EA63 AE25 1B69` (Niels Lohmann) | MIT (`LICENSE.MIT`) | `single_include/nlohmann/json.hpp`, `LICENSE.MIT` |

Notes

- Monocypher 4.0.3 (2026-06-15) exists; its only security fix is an EdDSA/Ed25519 timing leak. The
  engine uses only ChaCha20 and Poly1305, so 4.0.2 (the version PHASE2 names) is kept.
- How they are built: libopus from its own `.mk` lists (`CELT_SOURCES SILK_SOURCES
  SILK_SOURCES_FLOAT OPUS_SOURCES OPUS_SOURCES_FLOAT`), float, `-DOPUS_BUILD -DUSE_ALLOCA
  -DHAVE_LRINTF -DPACKAGE_VERSION="1.6.1"`, no intrinsics/RTCD (see `../engine/build-tests.sh`,
  `.cmd`); `monocypher.c` as C, listed in `../engine/sources.txt`; json is header-only.
- `speexdsp-1.2.1/` (the echo canceller) is added by WP9, with its own row here.
