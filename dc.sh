#!/bin/sh

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

mkdcdisc -e build/singe_dreamcast.elf -d data -d resources -S "$SORT_FILE" -N -n "$DISC_NAME" -o "$OUTPUT_FILE"
