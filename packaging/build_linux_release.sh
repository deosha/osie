#!/usr/bin/env bash
# Builds the Linux release archive, .deb and .rpm inside the release-builder
# image and writes them, with SHA-256 checksums, to the output directory.
# Runs natively on the host architecture (x86_64 or arm64).
#
# usage: packaging/build_linux_release.sh <out-dir>
set -euo pipefail
out=$(cd "${1:?output directory}" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)
docker build -q -t osie-linux-release -f "$root/packaging/linux-release.Dockerfile" "$root/packaging" >/dev/null
docker run --rm -v "$root":/src:ro -v "$out":/out osie-linux-release bash -euo pipefail -c '
  cmake -S /src -B /tmp/r -G Ninja -DCMAKE_BUILD_TYPE=Release -DOSIE_BUILD_TESTS=OFF \
        -DOSIE_BUILD_BENCHMARKS=OFF -DOSIE_PORTABLE=ON >/dev/null
  ninja -C /tmp/r osie >/dev/null
  cd /tmp/r && cpack -G "TGZ;DEB;RPM" >/dev/null
  cp /tmp/r/*.tar.gz /tmp/r/*.deb /tmp/r/*.rpm /out/
  objdump -T /tmp/r/osie | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1 > /out/glibc-min.txt
'
(cd "$out" && shasum -a 256 *.tar.gz *.deb *.rpm > SHA256SUMS-linux-$(uname -m).txt 2>/dev/null || sha256sum *.tar.gz *.deb *.rpm > SHA256SUMS-linux-$(uname -m).txt)
ls -l "$out"
