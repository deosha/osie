#!/usr/bin/env bash
# Release smoke test: installs an OSIE archive into an empty temporary prefix
# and runs it with an empty environment, an empty HOME and the working
# directory set to /, so nothing can come from a source checkout.
#
# usage: packaging/smoke_test.sh osie-<version>-<platform>.tar.gz
#    or: packaging/smoke_test.sh --installed /usr/bin/osie   (deb/rpm checks)
set -euo pipefail

fail() { echo "SMOKE FAIL: $*" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/home"

if [[ "${1:-}" == "--installed" ]]; then
  osie="${2:?path to installed osie}"
else
  archive="${1:?archive path}"
  tar -xzf "$archive" -C "$work"
  osie=$(echo "$work"/osie-*/bin/osie)
fi
[[ -x "$osie" ]] || fail "binary not found"

run() { (cd / && env -i HOME="$work/home" PATH=/usr/bin:/bin "$osie" "$@"); }

# 1. The binary must not reference a build or source directory.
if strings "$osie" | grep -E '/(Users|home)/[^/]+/' | grep -v '^/home/runner/work' >/dev/null; then
  strings "$osie" | grep -E '/(Users|home)/[^/]+/' | head -3
  fail "binary embeds a user/source path"
fi

# 2. Version and model discovery from the installed layout only.
v=$(run version) || fail "version exited non-zero"
echo "$v"
echo "$v" | grep -q "installed with the binary" || fail "bundled model not found"

# 3. Default (rules) and hybrid parsers, no observation.
run parse "why is my computer slow" | grep -q '"intent":"diagnose_slow"' || fail "rules parse"
run --parser hybrid parse "what is using the most cpu" | grep -q '"parser":"hybrid' || fail "hybrid parser not used"

# 4. A real question with JSON output against the live kernel.
out=$(run ask --json "how many cores do I have") || fail "ask exited non-zero"
echo "$out" | grep -q '"validation_errors":\[\]' || fail "ask json"

# 5. Read-only: every action capability is denied; a kill request is refused.
run caps | awk '$1 ~ /^act\./ && $2 != "DENY" {bad=1} END {exit bad}' || fail "an action capability is allowed"
run ask "kill pid 1" | grep -q "won't" || fail "action not refused"

# 6. An explicit but missing model is an error, not a silent fallback.
if run --model /nonexistent.osiem --parser hybrid ask hi >/dev/null 2>&1; then fail "missing --model accepted"; fi

# 7. A corrupt model is rejected cleanly (no crash).
printf 'OSIEM1\narch bag\ntensor emb f32 4194304 4194304 1024\ndata\n' > "$work/bad.osiem"
set +e
run --model "$work/bad.osiem" --parser hybrid ask hi >/dev/null 2>"$work/err"
rc=$?
set -e
[[ $rc -eq 1 ]] || fail "corrupt model: expected exit 1, got $rc"

echo "SMOKE OK"
