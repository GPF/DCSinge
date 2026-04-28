#!/bin/sh

GAME_NAME=$(awk -F= '
    /^[[:space:]]*game_name[[:space:]]*=/ {
        sub(/^[[:space:]]*/, "", $2);
        sub(/[[:space:]]*$/, "", $2);
        print $2;
        exit
    }
' data/singe.cfg)

DISC_NAME="DCSinge"
OUTPUT_FILE="dcsinge.cdi"
if [ -n "$GAME_NAME" ]; then
    DISC_NAME="$GAME_NAME - DCSinge"
    GAME_SLUG=$(printf '%s' "$GAME_NAME" | tr '[:upper:]' '[:lower:]' | tr ' /' '__' | tr -cd 'a-z0-9_-' )
    OUTPUT_FILE="${GAME_SLUG}_dcsinge.cdi"
fi

mkdcdisc -e build/singe_dreamcast.elf -d data -d resources -N -n "$DISC_NAME" -o "$OUTPUT_FILE"
