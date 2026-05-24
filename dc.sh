#!/usr/bin/env bash

set -euo pipefail

FILTER_NATIVE_ASSETS=1

while [ $# -gt 0 ]; do
    case "$1" in
        --native-assets-only|--exclude-original-assets)
            FILTER_NATIVE_ASSETS=1
            shift
            ;;
        --include-original-assets)
            FILTER_NATIVE_ASSETS=0
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--native-assets-only|--include-original-assets]"
            echo
            echo "  --native-assets-only        Build CDI from a generated file list that omits"
            echo "                              .png files with sibling .dt files and .wav files"
            echo "                              with sibling .dca files. This is the default."
            echo "                              The image tree is staged with hardlinks, not"
            echo "                              copied file payloads."
            echo "  --include-original-assets   Build CDI from a generated file list, including"
            echo "                              original .png and .wav files."
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

GAME_NAME=$(awk -F= '
    /^[[:space:]]*game_name[[:space:]]*=/ {
        sub(/^[[:space:]]*/, "", $2);
        sub(/[[:space:]]*$/, "", $2);
        print $2;
        exit
    }
' data/singe.cfg)

GAME_DIR=$(awk -F= '
    /^[[:space:]]*game_dir[[:space:]]*=/ {
        sub(/^[[:space:]]*/, "", $2);
        sub(/[[:space:]]*$/, "", $2);
        print $2;
        exit
    }
' data/singe.cfg)
GAME_DIR=${GAME_DIR%/}

SORT_FILE="dcsinge.sort"
DISC_FILE_LIST="dcsinge.files"
STAGE_DIR=".dcsinge-cdi-stage"

cat > "$SORT_FILE" <<'EOF'
/singe.cfg 10000
/*.cfg 9900
/*.singe 9800
/*.txt 9700
/*.dcmv -10000
EOF

if [ -n "$GAME_DIR" ]; then
    {
        echo "/$GAME_DIR/singe/*/*.cfg 9600"
        echo "/$GAME_DIR/singe/*/*.singe 9500"
        echo "/$GAME_DIR/singe/*/*.txt 9400"
        echo "/$GAME_DIR/singe/*/*.ttf 9300"
        echo "/$GAME_DIR/singe/*/*.dt 8500"
        echo "/$GAME_DIR/singe/*/*.dca 8400"
        echo "/$GAME_DIR/singe/*/*.png 8000"
        echo "/$GAME_DIR/singe/*/*.wav 7000"
    } >> "$SORT_FILE"
fi

DISC_NAME="DCSinge"
OUTPUT_FILE="dcsinge.cdi"
if [ -n "$GAME_NAME" ]; then
    DISC_NAME="$GAME_NAME - DCSinge"
    GAME_SLUG=$(printf '%s' "$GAME_NAME" | tr '[:upper:]' '[:lower:]' | tr ' /' '__' | tr -cd 'a-z0-9_-' )
    OUTPUT_FILE="${GAME_SLUG}_dcsinge.cdi"
fi

should_include_file() {
    local file="$1"
    local native

    case "$file" in
        *.bak|*.BAK)
            echo "SKIP backup file: $file" >&2
            return 1
            ;;
    esac

    if [ "$FILTER_NATIVE_ASSETS" -eq 1 ]; then
        case "$file" in
            *.png|*.PNG)
                native="${file%.*}.dt"
                if [ -f "$native" ]; then
                    echo "SKIP original PNG: $file" >&2
                    return 1
                fi
                ;;
            *.wav|*.WAV)
                native="${file%.*}.dca"
                if [ -f "$native" ]; then
                    echo "SKIP original WAV: $file" >&2
                    return 1
                fi
                ;;
        esac
    fi

    return 0
}

echo "Writing CDI file list to $DISC_FILE_LIST"
: > "$DISC_FILE_LIST"
for ROOT in data resources; do
    [ -d "$ROOT" ] || continue
    while IFS= read -r -d '' FILE; do
        if should_include_file "$FILE"; then
            printf '%s\n' "$FILE" >> "$DISC_FILE_LIST"
        fi
    done < <(find "$ROOT" -type f -print0 | sort -z)
done

FILE_COUNT=0
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"
trap 'rm -rf "$STAGE_DIR"' EXIT HUP INT TERM

while IFS= read -r FILE; do
    [ -n "$FILE" ] || continue
    mkdir -p "$STAGE_DIR/$(dirname "$FILE")"
    if ! ln "$FILE" "$STAGE_DIR/$FILE" 2>/dev/null; then
        ln -s "$(pwd)/$FILE" "$STAGE_DIR/$FILE"
        touch -h -r "$FILE" "$STAGE_DIR/$FILE"
    fi
    FILE_COUNT=$((FILE_COUNT + 1))
done < "$DISC_FILE_LIST"

echo "Staged $FILE_COUNT file entries from $DISC_FILE_LIST in $STAGE_DIR"
mkdcdisc -e build/singe_dreamcast.elf -D "$STAGE_DIR" -S "$SORT_FILE" -N -n "$DISC_NAME" -o "$OUTPUT_FILE"
