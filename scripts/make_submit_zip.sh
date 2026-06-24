#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
project_dir="$repo_root/rmdb"
out="${1:-$(cd "$repo_root/.." && pwd)/submit.zip}"

if ! command -v rsync >/dev/null 2>&1; then
    echo "error: rsync is required" >&2
    exit 1
fi

if ! command -v zip >/dev/null 2>&1; then
    echo "error: zip is required" >&2
    exit 1
fi

for path in "$project_dir/CMakeLists.txt" "$project_dir/src" "$project_dir/deps"; do
    if [[ ! -e "$path" ]]; then
        echo "error: required path missing: $path" >&2
        exit 1
    fi
done

stage="$(mktemp -d)"
cleanup() {
    rm -rf "$stage"
}
trap cleanup EXIT

rsync -a "$project_dir/" "$stage/" \
    --exclude='build/' \
    --exclude='rmdb_client/build/' \
    --exclude='bin/' \
    --exclude='lib/' \
    --exclude='cmake-build-*/' \
    --exclude='CMakeFiles/' \
    --exclude='Testing/' \
    --exclude='*_db/' \
    --exclude='*testdb/' \
    --exclude='testdb/' \
    --exclude='db.log' \
    --exclude='output.txt' \
    --exclude='*.pdf' \
    --exclude='*.zip' \
    --exclude='*.tar' \
    --exclude='*.tar.gz' \
    --exclude='*.tgz' \
    --exclude='*.rar' \
    --exclude='*.o' \
    --exclude='*.obj' \
    --exclude='*.a' \
    --exclude='*.so' \
    --exclude='*.dylib' \
    --exclude='*.dll' \
    --exclude='*.exe'

mkdir -p "$(dirname "$out")"
rm -f "$out"

(
    cd "$stage"
    zip -qr "$out" .
)

if command -v zipinfo >/dev/null 2>&1; then
    entries="$(zipinfo -1 "$out")"
    grep -qx 'CMakeLists.txt' <<<"$entries"
    grep -qx 'src/' <<<"$entries"
    grep -qx 'deps/' <<<"$entries"
fi

echo "Created $out"
ls -lh "$out"
