#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/clean_mp3.sh [directory]

Strips MP3 metadata (ID3/Xing) so files begin with MPEG frame data, which is
more compatible with the Dreamcast MP3 path in DCSinge.

Arguments:
  directory   Root directory to scan recursively (default: data)
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

ROOT_DIR="${1:-data}"

if [[ ! -d "$ROOT_DIR" ]]; then
    echo "Directory not found: $ROOT_DIR" >&2
    exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg is required but not found in PATH." >&2
    exit 1
fi

tmp_file="$(mktemp)"
trap 'rm -f "$tmp_file"' EXIT

total=0
cleaned=0
skipped=0
failed=0

while IFS= read -r -d '' mp3_file; do
    total=$((total + 1))
    out_file="${mp3_file}.clean.$$"

    echo "Cleaning: $mp3_file"
    if ffmpeg -nostdin -hide_banner -loglevel error -y \
        -i "$mp3_file" \
        -map 0:a:0 \
        -c copy \
        -f mp3 \
        -map_metadata -1 \
        -write_xing 0 \
        -id3v2_version 0 \
        -write_id3v1 0 \
        "$out_file"; then
        mv -f "$out_file" "$mp3_file"
        cleaned=$((cleaned + 1))
    else
        rm -f "$out_file"
        echo "Failed: $mp3_file" >&2
        failed=$((failed + 1))
        continue
    fi

    head -c 3 "$mp3_file" > "$tmp_file" || true
    if grep -q '^ID3$' "$tmp_file"; then
        echo "Warning: still starts with ID3: $mp3_file" >&2
        skipped=$((skipped + 1))
    fi
done < <(find "$ROOT_DIR" -type f \( -iname "*.mp3" \) -print0)

echo
echo "MP3 clean summary"
echo "  scanned : $total"
echo "  cleaned : $cleaned"
echo "  failed  : $failed"
echo "  warned  : $skipped"
