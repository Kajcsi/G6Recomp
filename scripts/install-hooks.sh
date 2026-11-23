#!/usr/bin/env bash
# Points git at the versioned hooks in scripts/hooks (pre-commit runs check-clean.sh).
set -euo pipefail
repo_root=$(git rev-parse --show-toplevel)
git -C "$repo_root" config core.hooksPath scripts/hooks
echo "Hooks installed: core.hooksPath=scripts/hooks"
