#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/png_pad_to_pot.sh [options] <directory>

Pads PNG files to power-of-two dimensions and generates Dreamcast .dt textures.

Options:
  --no-pad             Do not rewrite PNGs; only generate .dt files
  --no-dt              Only pad PNGs; do not run pvrtex
  --pvrtex <path>      pvrtex executable (default: /opt/toolchains/dc/kos/utils/pvrtex/pvrtex)
  -h, --help           Show this help

Output:
  foo.png -> foo.dt
EOF
}

PAD=1
MAKE_DT=1
PVRTEX="/opt/toolchains/dc/kos/utils/pvrtex/pvrtex"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-pad)
            PAD=0
            shift
            ;;
        --no-dt)
            MAKE_DT=0
            shift
            ;;
        --pvrtex)
            if [ $# -lt 2 ]; then
                echo "ERR --pvrtex requires a path" >&2
                exit 1
            fi
            PVRTEX="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "ERR unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            break
            ;;
    esac
done

if [ $# -ne 1 ]; then
    usage >&2
    exit 1
fi

ROOT="$1"

if [ ! -d "$ROOT" ]; then
    echo "ERR not a directory: $ROOT" >&2
    exit 1
fi

if command -v magick >/dev/null 2>&1; then
    IM_CONVERT=(magick)
    IM_IDENTIFY=(magick identify)
    USE_PIL=0
elif command -v convert >/dev/null 2>&1 && command -v identify >/dev/null 2>&1; then
    IM_CONVERT=(convert)
    IM_IDENTIFY=(identify)
    USE_PIL=0
elif python3 -c 'import PIL' >/dev/null 2>&1; then
    USE_PIL=1
else
    echo "ERR ImageMagick or Python Pillow not found (need magick, convert+identify, or PIL)" >&2
    exit 1
fi

if [ "$MAKE_DT" -eq 1 ] && [ ! -x "$PVRTEX" ]; then
    echo "ERR pvrtex not executable: $PVRTEX" >&2
    exit 1
fi

next_pot() {
    local n=$1
    local p=1
    while [ "$p" -lt "$n" ]; do
        p=$((p << 1))
    done
    if [ "$p" -lt 8 ]; then
        p=8
    fi
    if [ "$p" -gt 1024 ]; then
        p=1024
    fi
    echo "$p"
}

processed=0
padded=0
converted=0

while IFS= read -r -d '' img; do
    if [ "$USE_PIL" -eq 1 ]; then
        size_info=$(python3 -c 'from PIL import Image; import sys; im = Image.open(sys.argv[1]); print(im.size[0], im.size[1])' "$img")
    else
        size_info=$("${IM_IDENTIFY[@]}" -format "%w %h" "$img")
    fi
    read -r w h <<< "$size_info"

    if [ -z "${w:-}" ] || [ -z "${h:-}" ]; then
        echo "WARN could not read size for $img, skipping"
        continue
    fi

    pot_w=$(next_pot "$w")
    pot_h=$(next_pot "$h")
    processed=$((processed + 1))

    if [ "$PAD" -eq 1 ] && { [ "$w" -ne "$pot_w" ] || [ "$h" -ne "$pot_h" ]; }; then
        echo "PAD  $img  ${w}x${h} -> ${pot_w}x${pot_h}"
        if [ "$USE_PIL" -eq 1 ]; then
            python3 -c 'from PIL import Image; import sys; src, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); im = Image.open(src).convert("RGBA"); out = Image.new("RGBA", (w, h), (0, 0, 0, 0)); out.paste(im, (0, 0)); out.save(src)' "$img" "$pot_w" "$pot_h"
        else
            "${IM_CONVERT[@]}" "$img" \
                -resize "${pot_w}x${pot_h}>" \
                -background transparent \
                -gravity northwest \
                -extent "${pot_w}x${pot_h}" \
                "$img"
        fi
        padded=$((padded + 1))
    else
        echo "OK   $img  ${w}x${h}"
    fi

    if [ "$MAKE_DT" -eq 1 ]; then
        dt="${img%.*}.dt"
        "$PVRTEX" -i "$img" -o "$dt" >/dev/null
        echo "DT   $dt"
        converted=$((converted + 1))
    fi
done < <(find "$ROOT" -type f -iname "*.png" -print0)

echo
echo "Scanned $processed PNG file(s); padded $padded; generated $converted DT texture(s)."
