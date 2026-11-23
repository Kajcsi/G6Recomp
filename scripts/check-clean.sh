#!/usr/bin/env bash
# Rejects anything CLAUDE.md's hard rules say must never be committed: files from ignored
# locations (private/, build/, out/, a game install in the repo root), executables, game and
# console package formats, media and shader containers, files over 2 MiB, and files whose
# SHA-256 matches a known game file (scripts/known-game-hashes.txt).
#
# Usage: scripts/check-clean.sh [--staged | --all]
#   --staged  check the content staged for the next commit (default; used by the pre-commit hook)
#   --all     check every tracked file in the index (used by CI)
set -uo pipefail

repo_root=$(git rev-parse --show-toplevel) || exit 2
cd "$repo_root" || exit 2

mode=staged
case "${1:-}" in
"" | --staged) ;;
--all) mode=all ;;
*)
    echo "usage: $0 [--staged | --all]" >&2
    exit 2
    ;;
esac

max_bytes=$((2 * 1024 * 1024))
hash_list="scripts/known-game-hashes.txt"
blocked_ext='\.(rpf|ttpf|xvd|xvc|msixvc|appx|appxbundle|msix|bk2|bik|awc|xma|wem|bnk|dds|ytd|ydr|ydd|yft|ymap|ytyp|ybn|ycd|ynv|ypt|ysc|exe|dll|sys|pdb|lib|obj|cso|dxil|dxbc)$'

declare -A known_hashes=()
if [[ -f $hash_list ]]; then
    while read -r h _; do
        [[ $h =~ ^[0-9a-f]{64}$ ]] && known_hashes[$h]=1
    done <"$hash_list"
fi

failures=0
fail() {
    echo "check-clean: $1: $2" >&2
    failures=$((failures + 1))
}

# Reads the staged blob for a path.
blob() { git cat-file blob ":$1" 2>/dev/null; }

# Hex of `count` bytes at `offset` in the staged blob.
bytes_hex() {
    blob "$1" | head -c $(($2 + $3)) | tail -c "$3" | od -An -tx1 | tr -d ' \n'
}

check_signature() {
    local path=$1 head xvd
    head=$(bytes_hex "$path" 0 8)
    case $head in
    4d5a*) fail "$path" "PE executable (MZ header)" ;;
    3[0-9]465052*) fail "$path" "RPF archive" ;;
    5244533[0-9]*) fail "$path" "RDS resource container (.ttpf)" ;;
    44445320*) fail "$path" "DDS texture" ;;
    42494b*) fail "$path" "Bink video" ;;
    4b4232*) fail "$path" "Bink 2 video" ;;
    52494646*) fail "$path" "RIFF media (audio/video)" ;;
    4f676753*) fail "$path" "Ogg media" ;;
    44584243*) fail "$path" "compiled shader container (DXBC/DXIL)" ;;
    72656766*) fail "$path" "registry hive" ;;
    esac
    xvd=$(bytes_hex "$path" 512 8)
    [[ $xvd == 6d7366742d787664 ]] && fail "$path" "XVD/XVC console package"
}

if [[ $mode == staged ]]; then
    list_cmd=(git diff --cached --name-only --diff-filter=ACMRT -z)
else
    list_cmd=(git ls-files -z)
fi

while IFS= read -r -d '' path; do
    lower=${path,,}

    if [[ $lower =~ ^(private|build|out)/ ]] || git check-ignore -q --no-index -- "$path"; then
        fail "$path" "comes from an ignored location (game files and generated output stay local)"
        continue
    fi
    if [[ $lower =~ $blocked_ext ]]; then
        fail "$path" "file type is a binary, game, or console package format"
        continue
    fi

    entry_mode=$(git ls-files -s -- "$path" | head -n 1 | cut -d' ' -f1)
    # Submodule links and symlinks have no file content of their own to check.
    [[ $entry_mode == 160000 || $entry_mode == 120000 ]] && continue

    size=$(git cat-file -s ":$path" 2>/dev/null || echo 0)
    if ((size > max_bytes)); then
        fail "$path" "is $size bytes; the limit is $max_bytes"
        continue
    fi

    check_signature "$path"

    if ((${#known_hashes[@]} > 0)); then
        sha=$(blob "$path" | sha256sum | cut -d' ' -f1)
        [[ -n ${known_hashes[$sha]:-} ]] && fail "$path" "matches a known game file hash"
    fi
done < <("${list_cmd[@]}")

if ((failures > 0)); then
    echo "check-clean: $failures problem(s). See CLAUDE.md, \"Hard rules\"." >&2
    exit 1
fi
exit 0
