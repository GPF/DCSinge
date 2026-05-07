#!/usr/bin/env bash
set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <directory>"
    exit 1
fi

ROOT="$1"

# Detect ImageMagick tools
if command -v magick >/dev/null 2>&1; then
    IM_CONVERT="magick"
    IM_IDENTIFY="magick identify"
elif command -v convert >/dev/null 2>&1 && command -v identify >/dev/null 2>&1; then
    IM_CONVERT="convert"
    IM_IDENTIFY="identify"
else
    echo "❌ ImageMagick not found (need magick or convert+identify)"
    exit 1
fi

# Next power of two
next_pot() {
    local n=$1
    local p=1
    while [ $p -lt $n ]; do
        p=$((p << 1))
    done
    echo $p
}

find "$ROOT" -type f -iname "*.png" | while read -r img; do
    read w h <<< "$($IM_IDENTIFY -format "%w %h" "$img")"

    # Sanity guard
    if [ -z "$w" ] || [ -z "$h" ]; then
        echo "⚠️  Could not read size for $img, skipping"
        continue
    fi

    pot_w=$(next_pot "$w")
    pot_h=$(next_pot "$h")

    if [ "$w" -eq "$pot_w" ] && [ "$h" -eq "$pot_h" ]; then
        echo "✔ POT already: $img ($w x $h)"
        continue
    fi

    echo "🔧 Padding to POT: $img ($w x $h → $pot_w x $pot_h)"

    $IM_CONVERT "$img" \
        -background transparent \
        -gravity northwest \
        -extent "${pot_w}x${pot_h}" \
        "$img"
done

echo "✅ All PNGs padded to POT (top-left, no scaling)"
