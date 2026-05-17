#!/bin/sh

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
            echo "  --native-assets-only        Build CDI from a temporary staged tree that omits"
            echo "                              .png files with sibling .dt files and .wav files"
            echo "                              with sibling .dca files. This is the default."
            echo "  --include-original-assets   Build CDI directly from data/resources, including"
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

SORT_FILE="dcsinge.sort"

cat > "$SORT_FILE" <<'EOF'
/singe.cfg 10000
/*.cfg 9900
/*.singe 9800
/*.txt 9700
/crimepatrol-hd/singe/crimepatrol-hd/*.cfg 9600
/crimepatrol-hd/singe/crimepatrol-hd/*.singe 9500
/crimepatrol-hd/singe/crimepatrol-hd/*.txt 9400
/crimepatrol-hd/singe/crimepatrol-hd/*.ttf 9300
/crimepatrol-hd/singe/crimepatrol-hd/*.png 8000
/crimepatrol-hd/singe/crimepatrol-hd/*.wav 7000
/*.dcmv -10000
EOF

DISC_NAME="DCSinge"
OUTPUT_FILE="dcsinge.cdi"
if [ -n "$GAME_NAME" ]; then
    DISC_NAME="$GAME_NAME - DCSinge"
    GAME_SLUG=$(printf '%s' "$GAME_NAME" | tr '[:upper:]' '[:lower:]' | tr ' /' '__' | tr -cd 'a-z0-9_-' )
    OUTPUT_FILE="${GAME_SLUG}_dcsinge.cdi"
fi

if [ "$FILTER_NATIVE_ASSETS" -eq 1 ]; then
    STAGE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dcsinge-cdi.XXXXXX") || exit 1
    trap 'rm -rf "$STAGE_DIR"' EXIT HUP INT TERM

    echo "Staging CDI tree in $STAGE_DIR"
    for ROOT in data resources; do
        [ -d "$ROOT" ] || continue
        find "$ROOT" -type d | while IFS= read -r DIR; do
            mkdir -p "$STAGE_DIR/$DIR"
        done
        find "$ROOT" -type f | while IFS= read -r FILE; do
            case "$FILE" in
                *.png|*.PNG)
                    NATIVE="${FILE%.*}.dt"
                    if [ -f "$NATIVE" ]; then
                        echo "SKIP original PNG: $FILE"
                        continue
                    fi
                    ;;
                *.wav|*.WAV)
                    NATIVE="${FILE%.*}.dca"
                    if [ -f "$NATIVE" ]; then
                        echo "SKIP original WAV: $FILE"
                        continue
                    fi
                    ;;
            esac
            cp -p "$FILE" "$STAGE_DIR/$FILE"
        done
    done

    mkdcdisc -e build/singe_dreamcast.elf -D "$STAGE_DIR" -S "$SORT_FILE" -N -n "$DISC_NAME" -o "$OUTPUT_FILE"
else
    mkdcdisc -e build/singe_dreamcast.elf -d data -d resources -S "$SORT_FILE" -N -n "$DISC_NAME" -o "$OUTPUT_FILE"
fi
