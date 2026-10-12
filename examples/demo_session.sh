#!/usr/bin/env bash
# Reproduces the multi-turn example from the project brief against the live machine.
set -euo pipefail
OSIE="${OSIE:-$(dirname "$0")/../build/osie}"
printf '%s\n' \
  "What is using CPU?" \
  "Which one is using the most memory?" \
  "Why might it be consuming so much memory?" \
  "what about cpu?" \
  "Why is my Mac using so much CPU?" \
  "how does memory compression work?" \
  "kill it" \
  | "$OSIE" "$@"
