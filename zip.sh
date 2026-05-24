#!/usr/bin/env bash

set -euo pipefail

FILTER_NATIVE_ASSETS=1
PACKAGE_DIR="DCSinge"
ELF_FILE="build/singe_dreamcast.elf"
OUTPUT_FILE=""

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
        --package-dir)
            PACKAGE_DIR="${2:?missing package directory}"
            shift 2
            ;;
        --elf)
            ELF_FILE="${2:?missing ELF path}"
            shift 2
            ;;
        -o|--output)
            OUTPUT_FILE="${2:?missing output ZIP path}"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--native-assets-only|--include-original-assets] [--package-dir DCSinge] [--elf build/singe_dreamcast.elf] [-o output.zip]"
            echo
            echo "  --native-assets-only        Build ZIP from a generated file list that omits"
            echo "                              .png files with sibling .dt files and .wav files"
            echo "                              with sibling .dca files. This is the default."
            echo "  --include-original-assets   Build ZIP including original .png and .wav files."
            echo "  --package-dir NAME          Top-level folder inside the ZIP. Default: DCSinge."
            echo "  --elf PATH                  ELF to include. Default: build/singe_dreamcast.elf."
            echo "  -o, --output PATH           Output ZIP path. Default is based on game_name."
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if ! command -v zip >/dev/null 2>&1; then
    echo "zip command not found" >&2
    exit 1
fi

if [ ! -f "$ELF_FILE" ]; then
    echo "Missing ELF: $ELF_FILE" >&2
    echo "Build first with: cmake --build build" >&2
    exit 1
fi

GAME_NAME=$(awk -F= '
    /^[[:space:]]*game_name[[:space:]]*=/ {
        sub(/^[[:space:]]*/, "", $2);
        sub(/[[:space:]]*$/, "", $2);
        print $2;
        exit
    }
' data/singe.cfg)

if [ -z "$OUTPUT_FILE" ]; then
    OUTPUT_FILE="dcsinge_loader.zip"
    if [ -n "$GAME_NAME" ]; then
        GAME_SLUG=$(printf '%s' "$GAME_NAME" | tr '[:upper:]' '[:lower:]' | tr ' /' '__' | tr -cd 'a-z0-9_-' )
        OUTPUT_FILE="${GAME_SLUG}_dcsinge_loader.zip"
    fi
fi

FILE_LIST="dcsinge-zip.files"
STAGE_DIR=".dcsinge-zip-stage"
PACKAGE_ROOT="$STAGE_DIR/$PACKAGE_DIR"

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

stage_file() {
    local src="$1"
    local dst="$2"

    mkdir -p "$(dirname "$dst")"
    if ! ln "$src" "$dst" 2>/dev/null; then
        cp -p "$src" "$dst"
    fi
}

echo "Writing ZIP file list to $FILE_LIST"
: > "$FILE_LIST"
for ROOT in data resources; do
    [ -d "$ROOT" ] || continue
    while IFS= read -r -d '' FILE; do
        if should_include_file "$FILE"; then
            printf '%s\n' "$FILE" >> "$FILE_LIST"
        fi
    done < <(find "$ROOT" -type f -print0 | sort -z)
done

FILE_COUNT=0
rm -rf "$STAGE_DIR"
mkdir -p "$PACKAGE_ROOT"
trap 'rm -rf "$STAGE_DIR"' EXIT HUP INT TERM

stage_file "$ELF_FILE" "$PACKAGE_ROOT/$(basename "$ELF_FILE")"
FILE_COUNT=$((FILE_COUNT + 1))

while IFS= read -r FILE; do
    [ -n "$FILE" ] || continue
    stage_file "$FILE" "$PACKAGE_ROOT/$FILE"
    FILE_COUNT=$((FILE_COUNT + 1))
done < "$FILE_LIST"

rm -f "$OUTPUT_FILE"
echo "Staged $FILE_COUNT file entries in $PACKAGE_ROOT"
(
    cd "$STAGE_DIR"
    zip -r "../$OUTPUT_FILE" "$PACKAGE_DIR"
)

echo "Wrote $OUTPUT_FILE"
