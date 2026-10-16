# Installing OSIE

OSIE is a single binary plus a ~1 MB intent model. It needs no runtime
dependencies beyond the C library, no privileges and no network.

## Quick install

| Platform | Command |
|---|---|
| macOS 13+ (arm64, x86_64) | `brew install deosha/osie/osie` |
| Linux with Homebrew | `brew install deosha/osie/osie` |
| Debian 12+ / Ubuntu 22.04+ | `sudo apt install ./osie_<version>_<amd64\|arm64>.deb` (download from the release) |
| Fedora 36+ / RHEL family with glibc ≥ 2.35 | `sudo dnf install https://github.com/deosha/osie/releases/download/v<version>/osie-<version>-1.<x86_64\|aarch64>.rpm` |
| Other Linux, glibc ≥ 2.35 | release `.tar.gz` (below) |

Releases: <https://github.com/deosha/osie/releases>. The Homebrew formula
lives in the tap [`deosha/homebrew-osie`](https://github.com/deosha/homebrew-osie).
It installs the same prebuilt, checksum-pinned archives.

## Optional background agent (Linux)

Packages also install `osie-agent`, an opt-in background observer that keeps
a bounded in-memory history. The systemd unit is installed **disabled** and is
never started by the package:

```bash
sudo systemctl enable --now osie-agent    # opt in
sudo systemctl disable --now osie-agent   # opt out
```

It runs as a transient unprivileged user, with no capabilities, no IP sockets
and no writes to disk. Configuration (optional): `/etc/osie/agent.conf`; an
example is in `share/osie/agent.conf.example`. Removing the package stops it.
The details are in [`AGENT.md`](AGENT.md). On macOS, `osie-agent` can be run
by hand; no launchd job is installed.

### Optional local diagnostic API

The agent can answer questions from local programs over HTTP/JSON on a Unix
domain socket. It is off unless enabled, and never listens on an IP port.

```bash
sudo tee -a /etc/osie/agent.conf <<'CONF'
api_enabled = true
api_group = osie
CONF
sudo usermod -aG osie "$USER"             # then log in again
sudo systemctl restart osie-agent
curl --unix-socket /run/osie/agent.sock http://localhost/v1/health/ready
```

What the packages and the unit do for it:
- The packages create the system group `osie`, with no members.
- The unit creates `/run/osie` (mode 0750, group `osie`) at start and
  removes it at stop.
- Without `api_group`, only root can use the socket.

Upgrading from 0.2.x keeps the configuration and whether the service is
enabled. It adds the empty `osie` group, the empty `/run/osie` directory, and
a task limit of 32 instead of 16; nothing listens until you enable the API.
From a tarball, create the group yourself (`groupadd --system osie`) if you
use the unit. See [`API.md`](API.md).

## Upgrading

Installing a newer `.deb` or `.rpm` over an older one replaces the binary and
the models (`sudo apt install ./osie_<new>.deb`, `sudo dnf upgrade ./osie-<new>.rpm`
or `rpm -U`). `packaging/upgrade_test.sh` checks a clean install, an upgrade
from the previous release and an uninstall that leaves no files, for the
`.deb` (Ubuntu 22.04), the `.rpm` (Fedora 40) and the tarball. For v0.2.0 it
passed locally on arm64; CI runs it on x86_64 (`packaging` job). Homebrew:
`brew upgrade osie`.

## Platform support

What was verified for v0.2.0:
- the release workflow built and attested all four platforms;
- `brew upgrade` from 0.1.0 on macOS 15 (M4 Pro), followed by `brew test`,
  `brew audit --strict --online` and the model checksums;
- the published `.deb` (arm64), checksum-verified and smoke-tested in a clean
  Ubuntu 22.04 container;
- clean install, upgrade from v0.1.0 and uninstall: `packaging/upgrade_test.sh`
  (arm64 locally, x86-64 in CI).

What was verified for v0.1.0:

| Platform | Package | Verified |
|---|---|---|
| macOS arm64 | Homebrew, `.tar.gz` | CI smoke test (macos-14). Installed with `brew install deosha/osie/osie` on macOS 15 (M4 Pro); `brew test` and `brew audit --strict --online` pass. |
| macOS x86_64 | Homebrew, `.tar.gz` | CI smoke test (macos-15-intel). |
| Linux x86_64 | `.deb`, `.rpm`, `.tar.gz` | CI: the tarball on Debian 12, the `.deb` installed and removed on Ubuntu 22.04, the `.rpm` installed and removed on Fedora 40. Release downloads re-tested on Ubuntu 22.04 (`.deb`) and Fedora 40 (`.rpm`). |
| Linux arm64 | `.deb`, `.rpm`, `.tar.gz` | The same CI checks. Release downloads re-tested on Debian 12 (`.deb`) and Fedora 40 (`.rpm`). |
| Linux with Homebrew | Homebrew | The formula uses the same Linux archives, but has not been tested through `brew` on Linux. |

glibc 2.35 or newer covers Debian 12+, Ubuntu 22.04+, Fedora 36+ and
RHEL/Rocky/Alma 10. RHEL 9 ships glibc 2.34 and is **not** supported by the
prebuilt binaries; build from source there.

**Uninstall (Homebrew):** `brew uninstall osie && brew untap deosha/osie`

## Install from a release archive

```bash
# 1. Download osie-<version>-<os>-<arch>.tar.gz and SHA256SUMS from
#    https://github.com/deosha/osie/releases, then verify:
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

The CPU world model used by `osie predict` (Linux) is looked up the same way:
1. `--world-model PATH`
2. `$OSIE_WORLD_MODEL`
3. the user data directory
4. `<prefix>/share/osie/models/world_cpu.osiewm`

Prediction is **experimental and off by default** (v0.2). Enable it per call
with `osie predict --experimental ...` or for a session with
`OSIE_EXPERIMENTAL_PREDICT=1`. `osie version` shows the installed world
model, its format and the environments it was validated in; outside them
OSIE withholds the prediction and says why.

Installed model files can be checked against the shipped checksums:

```bash
cd "$(dirname "$(command -v osie)")/../share/osie/models" && sha256sum -c SHA256SUMS
```

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
