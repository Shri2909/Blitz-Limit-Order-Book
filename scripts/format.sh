#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [[ $# -gt 1 ]]; then
    echo "Usage: bash scripts/format.sh [--check|--write]" >&2
    exit 2
fi

case "${1:---check}" in
    --check) flags=(--dry-run --Werror) ;;
    --write) flags=(-i) ;;
    *) echo "Usage: bash scripts/format.sh [--check|--write]" >&2; exit 2 ;;
esac

formatter="${CLANG_FORMAT:-clang-format}"
if ! command -v "$formatter" >/dev/null 2>&1; then
    echo "Install clang-format: python3 -m pip install clang-format==21.1.8" >&2
    exit 1
fi
version="$("$formatter" --version)"
if [[ ! "$version" =~ version\ 21\.1\.8([^0-9.]|$) ]]; then
    echo "Expected clang-format 21.1.8; found: $version" >&2
    exit 1
fi

files=()
while IFS= read -r -d '' file; do
    if [[ -f "$file" ]]; then
        files+=("$file")
    fi
done < <(git ls-files -z -- '*.cpp' '*.hpp' '*.c' '*.h')
if [[ ${#files[@]} -gt 0 ]]; then
    "$formatter" --style=file "${flags[@]}" "${files[@]}"
fi
