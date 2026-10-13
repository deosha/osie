#!/usr/bin/env bash
# Clean install, upgrade from the previous release, and uninstall of the
# Linux packages, each in a fresh container.
#
# usage: packaging/upgrade_test.sh <dir with new packages> <dir with previous release packages>
# Both directories must hold the .deb/.rpm for this machine's architecture.
set -euo pipefail
new=$(cd "${1:?new package dir}" && pwd)
old=$(cd "${2:?previous release dir}" && pwd)
root=$(cd "$(dirname "$0")/.." && pwd)

deb_arch=$(dpkg --print-architecture 2>/dev/null || { [[ $(uname -m) == arm64 || $(uname -m) == aarch64 ]] && echo arm64 || echo amd64; })
rpm_arch=$([[ $deb_arch == arm64 ]] && echo aarch64 || echo x86_64)

echo "== deb: clean install, upgrade, uninstall (ubuntu:22.04)"
docker run --rm -v "$new":/new:ro -v "$old":/old:ro -v "$root/packaging":/p:ro ubuntu:22.04 bash -euo pipefail -c "
  apt-get update -qq >/dev/null && apt-get install -y -qq binutils >/dev/null
  dpkg -i /new/osie_*_${deb_arch}.deb >/dev/null && osie version | head -1
  /p/smoke_test.sh --installed /usr/bin/osie | tail -1
  dpkg -r osie >/dev/null
  dpkg -i /old/osie_*_${deb_arch}.deb >/dev/null && osie version | head -1
  dpkg -i /new/osie_*_${deb_arch}.deb >/dev/null && osie version | head -1
  osie version | grep -q '^osie 0.2' || { echo 'upgrade did not replace the binary'; exit 1; }
  /p/smoke_test.sh --installed /usr/bin/osie | tail -1
  dpkg -r osie >/dev/null
  test ! -e /usr/bin/osie && test ! -e /usr/share/osie/models/world_cpu.osiewm && echo 'uninstall: no files left'
"

echo "== rpm: clean install, upgrade, uninstall (fedora:40)"
docker run --rm -v "$new":/new:ro -v "$old":/old:ro -v "$root/packaging":/p:ro fedora:40 bash -euo pipefail -c "
  dnf install -y -q binutils procps-ng >/dev/null 2>&1
  rpm -i /new/osie-*.${rpm_arch}.rpm && osie version | head -1
  /p/smoke_test.sh --installed /usr/bin/osie | tail -1
  rpm -e osie
  rpm -i /old/osie-*.${rpm_arch}.rpm && osie version | head -1
  rpm -U /new/osie-*.${rpm_arch}.rpm && osie version | head -1
  osie version | grep -q '^osie 0.2' || { echo 'upgrade did not replace the binary'; exit 1; }
  /p/smoke_test.sh --installed /usr/bin/osie | tail -1
  rpm -e osie
  test ! -e /usr/bin/osie && test ! -e /usr/share/osie/models/world_cpu.osiewm && echo 'uninstall: no files left'
"

echo "== tarball: clean install into an empty prefix"
docker run --rm -v "$new":/new:ro -v "$root/packaging":/p:ro ubuntu:22.04 bash -euo pipefail -c "
  apt-get update -qq >/dev/null && apt-get install -y -qq binutils >/dev/null
  /p/smoke_test.sh /new/osie-*-linux-*.tar.gz | tail -1
"
echo "UPGRADE TEST OK"
