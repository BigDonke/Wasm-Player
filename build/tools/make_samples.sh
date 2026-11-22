#!/bin/bash
# Generates the two test files in ../../samples. Needs ffmpeg with libx265, and python for the PGS track.
# Every audio track is a sine at its own pitch, so you can hear which track is playing.
set -euo pipefail
OUT=$(realpath -m "${1:-$(dirname "$0")/../../samples}")
cd "$(dirname "$0")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
PY=python3
"$PY" -c '' 2>/dev/null || PY=python

# bitexact and info=0 keep encoder versions and build details out of the files
FF="ffmpeg -hide_banner -loglevel error -y -fflags +bitexact -flags +bitexact"
HDR10="hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"
HDR10="$HDR10:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400"

# 0.2 amplitude sines, one pitch per track, on every channel of the layout
tone() { echo "aevalsrc=0.2*sin($1*2*PI*t):s=48000:c=$2:d=$3"; }
# TrueHD is lossless, so a sine on every channel costs 18 MB a minute. Front left and right only, 16 bit,
# keeps it near 4 MB. FFmpeg's TrueHD encoder stops at 5.1.
S="0.2*sin(220*2*PI*t)"
TRUEHD="aevalsrc=$S|$S|0|0|0|0:s=48000:c=5.1(side):d=60,aformat=sample_fmts=s16"

cat > "$TMP/en.srt" <<'EOF'
1
00:00:01,000 --> 00:00:04,000
English SRT line one
with <i>italic</i> text

2
00:00:10,000 --> 00:00:14,000
SRT cue at ten seconds

3
00:00:40,000 --> 00:00:44,000
SRT cue at forty seconds
EOF

cat > "$TMP/styled.ass" <<'EOF'
[Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,56,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,40,40,60,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:02.00,0:00:06.00,Default,,0,0,0,,ASS line one\Nwith {\i1}italic{\i0} and {\c&H00FFFF&}colored{\c} text
Dialogue: 0,0:00:20.00,0:00:24.00,Default,,0,0,0,,{\pos(960,200)}ASS line at the top
Dialogue: 0,0:00:45.00,0:00:49.00,Default,,0,0,0,,ASS cue at forty-five seconds
EOF

# 1. 60 s, 1080p HEVC 10-bit HDR10, closed GOP every 2 s.
#    TrueHD 5.1, DTS 5.1, E-AC-3 5.1, AAC stereo, SRT and ASS.
D=60
$FF -f lavfi -i "testsrc=s=1920x1080:r=24000/1001:d=$D" \
  -f lavfi -i "$TRUEHD" -f lavfi -i "$(tone 330 5.1 $D)" \
  -f lavfi -i "$(tone 440 5.1 $D)" -f lavfi -i "$(tone 550 stereo $D)" \
  -i "$TMP/en.srt" -i "$TMP/styled.ass" \
  -map 0 -map 1 -map 2 -map 3 -map 4 -map 5 -map 6 \
  -vf "format=yuv420p10le,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=tv" \
  -c:v libx265 -preset fast -crf 30 \
  -x265-params "$HDR10:keyint=48:min-keyint=48:open-gop=0:info=0:log-level=error" \
  -c:a:0 truehd -strict -2 -c:a:1 dca -b:a:1 768k -c:a:2 eac3 -b:a:2 224k -c:a:3 aac -b:a:3 96k \
  -c:s:0 srt -c:s:1 ass \
  -metadata:s:a:0 language=eng \
  -metadata:s:a:1 language=eng \
  -metadata:s:a:2 language=eng \
  -metadata:s:a:3 language=eng \
  -metadata:s:s:0 language=eng -metadata:s:s:1 language=eng \
  "$OUT/hevc-hdr10-truehd-dts-eac3-subs.mkv"

# 2. 3 min, 720p HEVC 10-bit SDR, keyframe every 4 s. DTS 5.1, AAC stereo, SRT and PGS.
D=180
"$PY" make_sup.py "$TMP/en.sup"
$FF -f lavfi -i "testsrc=s=1280x720:r=24000/1001:d=$D" \
  -f lavfi -i "$(tone 330 5.1 $D)" -f lavfi -i "$(tone 550 stereo $D)" \
  -i "$TMP/en.srt" -i "$TMP/en.sup" \
  -map 0 -map 1 -map 2 -map 3 -map 4 \
  -c:v libx265 -preset fast -crf 32 -pix_fmt yuv420p10le -x265-params "keyint=96:min-keyint=96:info=0:log-level=error" \
  -c:a:0 dca -b:a:0 768k -strict -2 -c:a:1 aac -b:a:1 96k -c:s:0 srt -c:s:1 copy \
  -metadata:s:a:0 language=eng \
  -metadata:s:a:1 language=eng \
  -metadata:s:s:0 language=eng -metadata:s:s:1 language=eng \
  "$OUT/hevc-dts-pgs-3min.mkv"

ls -la "$OUT"
