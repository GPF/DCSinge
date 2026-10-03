#!/usr/bin/env bash
# Encode a video + audio pair into a Dreamcast MPEG-1 program stream (MPEG-1 video +
# MP2 audio) and build its .pidx seek index.
#
#   tools/encode_mpeg.sh lair.m2v lair.ogg lair.mpg [options]
#
# Options (defaults suit Dragon's Lair: 4:3, 23.976 fps, mono):
#   --size WxH       output size            (320x240)
#   --fps N/D        frame rate of the source and output (24000/1001). Every source frame
#                    is kept (nothing is dropped or duplicated): a raw .m2v carries no
#                    timestamps, so this sets its real rate. Use 24 if audio ends early.
#   --vbit RATE      video bitrate          (1500k)
#   --gop N          frames per closed GOP  (12) -> seeks decode at most N-1 frames
#   --rate HZ        audio sample rate      (22050)
#   --channels N     1 or 2                 (1)
#   --abit RATE      audio bitrate          (64k; use 96k for stereo)
#   --expect N       fail unless the video has exactly N pictures
#   --limit SECS     encode only the first SECS seconds (to try settings quickly)
#   --vf FILTER      ffmpeg video filter(s) applied before the scale, e.g. yadif=0 to
#                    deinterlace, or crop=704:464:8:8
#
# Needs ffmpeg and python3 (tools/build_pidx.py). Closed GOPs plus a short GOP keep
# avmpeg_seek_frame() cheap and make every I-frame a clean landing point.
set -euo pipefail

if [ $# -lt 3 ]; then
    sed -n "2,22p" "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
VIDEO=$1; AUDIO=$2; OUT=$3; shift 3

SIZE=320x240; FPS=24000/1001; VBIT=1500k; GOP=12; RATE=22050; CH=1; ABIT=64k; EXPECT=; LIMIT=; VF=
while [ $# -gt 0 ]; do
    case "$1" in
        --size) SIZE=$2 ;;
        --fps) FPS=$2 ;;
        --vbit) VBIT=$2 ;;
        --gop) GOP=$2 ;;
        --rate) RATE=$2 ;;
        --channels) CH=$2 ;;
        --abit) ABIT=$2 ;;
        --expect) EXPECT=$2 ;;
        --limit) LIMIT=$2 ;;
        --vf) VF=$2 ;;
        *) echo "unknown option $1" >&2; exit 1 ;;
    esac
    shift 2
done

HERE=$(cd "$(dirname "$0")" && pwd)
W=${SIZE%x*}; H=${SIZE#*x}

echo "== source video"; ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,width,height,r_frame_rate,avg_frame_rate,field_order,nb_frames \
    -of default=nw=1 "$VIDEO" || true
echo "== source audio"; ffprobe -v error -select_streams a:0 \
    -show_entries stream=codec_name,sample_rate,channels,duration -of default=nw=1 "$AUDIO" || true

ffmpeg -hide_banner -y -framerate "$FPS" -i "$VIDEO" -i "$AUDIO" -map 0:v:0 -map 1:a:0 \
    -vf "${VF:+$VF,}scale=${W}:${H}:flags=lanczos,setsar=1" -r "$FPS" ${LIMIT:+-t "$LIMIT"} \
    -c:v mpeg1video -b:v "$VBIT" \
    -g "$GOP" -bf 2 -flags +cgop -sc_threshold 1000000000 \
    -c:a mp2 -ar "$RATE" -ac "$CH" -b:a "$ABIT" \
    -f mpeg "$OUT"

PIDX=${OUT%.*}.pidx
python3 "$HERE/build_pidx.py" "$OUT" "$PIDX" | head -3

PICS=$(python3 - "$PIDX" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<I', d, 0)[0]
t = d[4 + 12 * n:]
print(struct.unpack_from('<I', t, 4)[0] if t[:4] == b'PCNT' else -1)
PY
)
echo "== $OUT: $PICS pictures, $(du -h "$OUT" | cut -f1), index $PIDX"
if [ -n "$EXPECT" ] && [ "$PICS" != "$EXPECT" ]; then
    echo "ERROR: expected $EXPECT pictures, got $PICS (check the frame rate / telecine)" >&2
    exit 2
fi
