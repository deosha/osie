# Installing OSIE

OSIE is a single binary plus a ~1 MB intent model. It needs no runtime
dependencies beyond the C library, no privileges and no network.

## Platform support

| Platform | Package | Status |
|---|---|---|
| macOS 13+ on Apple Silicon (arm64) | `.tar.gz` | Built and smoke-tested locally on macOS 15. Release CI configured (`macos-14`). |
| macOS 13+ on Intel (x86_64) | `.tar.gz` | Release CI configured (`macos-15-intel`); not yet run. |
| Linux arm64, glibc ≥ 2.35 | `.tar.gz`, `.deb`, `.rpm` | Built in `packaging/linux-release.Dockerfile`. The tarball is smoke-tested on Debian 12 and Ubuntu 22.04, the `.deb` on Ubuntu 22.04 and 24.04, the `.rpm` on Fedora 40. |
| Linux x86_64, glibc ≥ 2.35 | `.tar.gz`, `.deb`, `.rpm` | Same build script; release CI configured (`ubuntu-24.04`); not yet run. |
| Homebrew | — | Not available yet. A formula needs a published release to point at. |

glibc 2.35 or newer covers Debian 12+, Ubuntu 22.04+, Fedora 36+ and
RHEL/Rocky/Alma 10. RHEL 9 ships glibc 2.34 and is **not** supported by the
prebuilt binaries; build from source there.

> **Release status:** the release workflow
> (`.github/workflows/release.yml`) publishes these artifacts when a `v*`
> tag is pushed. Until the first tag exists, there are no downloads: build
> from source (below), or build the packages yourself with
> `packaging/build_linux_release.sh <out-dir>` (needs Docker).

## Install from a release archive

```bash
# 1. Download osie-<version>-<os>-<arch>.tar.gz and SHA256SUMS from the release page, then verify:
sha256sum --check --ignore-missing SHA256SUMS        # macOS: shasum -a 256 -c SHA256SUMS --ignore-missing
# Optional: verify that GitHub Actions built it from this repository
gh attestation verify osie-<version>-<os>-<arch>.tar.gz --repo deosha/osie

# 2. Unpack into a prefix you own; no root is needed
mkdir -p ~/.local
tar -xzf osie-<version>-<os>-<arch>.tar.gz -C ~/.local --strip-components=1
export PATH="$HOME/.local/bin:$PATH"    # add to your shell profile

# 3. Check it
osie version
osie ask "how many cores do I have?"
```

The archive layout is `bin/osie`, `share/osie/models/intent.osiem` and
`share/doc/osie/`. Keep `bin/` and `share/` together; the binary finds its
model relative to its own location.

**macOS note:** the binaries are not signed or notarized. A browser download
is quarantined, and Gatekeeper will refuse to run it. You have two options:
- Download with `curl`, which does not set the quarantine attribute.
- After verifying the checksum and attestation as above, remove the
  attribute from that one file:
  `xattr -d com.apple.quarantine ~/.local/bin/osie`.

Do not disable Gatekeeper globally.

**Uninstall:**
`rm ~/.local/bin/osie && rm -r ~/.local/share/osie ~/.local/share/doc/osie`.

## Install a Linux package

```bash
sudo apt install ./osie_<version>_amd64.deb          # Debian/Ubuntu (arm64: _arm64.deb)
sudo dnf install ./osie-<version>-1.x86_64.rpm       # Fedora/RHEL family (aarch64: .aarch64.rpm)
```

The packages install `/usr/bin/osie`, `/usr/share/osie/models/intent.osiem`
and the license and README under `/usr/share/doc/osie/`. They create no
users, services, cron jobs or configuration files, and run no install
scripts.

**Uninstall:** `sudo apt remove osie`, or `sudo dnf remove osie`.

## Build from source

Requirements:
- A C++20 compiler (Apple clang 17, GCC 12+ or clang 16+).
- CMake ≥ 3.24 and Ninja.
- GoogleTest, for the tests.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/osie version
cmake --install build --prefix ~/.local     # optional: same layout as the archive
```

`cpack` (run in the build directory) produces the same archive and packages
that the release workflow does.

## Where OSIE looks for the model

The default parser is the zero-parameter rule parser, so **no model file is
needed**. The model is used only with `--parser hybrid` or
`--parser neural`, and is looked up in this order:

1. `--model PATH`
2. `$OSIE_MODEL`
3. The user data directory, `<dir>/models/intent.osiem`, where `<dir>` is:
   - macOS: `~/Library/Application Support/osie`
   - Linux: `$XDG_DATA_HOME/osie`, or `~/.local/share/osie`
4. The model installed with the binary,
   `<prefix>/share/osie/models/intent.osiem`. The build tree uses the same
   layout.
5. None found: OSIE uses the rule parser and says so.

The **current working directory is never searched**, so a stray model file
cannot change how OSIE reads your questions. If you name a model in (1) or
(2) and it fails to load, OSIE exits with an error instead of quietly using
something else. `osie version` prints the search order and which model, if
any, was found.

## Resource use

Measured on an Apple M4 Pro with a release build. Linux figures are similar
(see `BENCHMARKS.md`).

| | |
|---|---|
| Installed size | ≈ 2–3 MB: the binary (0.8 MB on macOS, ≈ 2 MB on Linux with the C++ runtime linked in) plus a 1.0 MB model |
| Memory per question | 8–11 MB peak RSS |
| Time per question | one 0.5 s sampling window plus ≈ 40 ms of work |
| `--watch 5` | ≈ 0.3% of one core; history ≈ 2.9 KiB per sample, capped at 720 samples (≈ 2.1 MiB) |

OSIE writes nothing to disk. The only exception is `--audit FILE`, which
appends policy decisions to the file you name.
