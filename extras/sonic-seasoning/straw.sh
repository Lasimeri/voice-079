#!/bin/bash
# A "sweet / strawberry" soundtrack after Spence's crossmodal findings: high
# register, consonant major harmony, soft bell-like attacks, slow and smooth.
set -e
d=$HOME/.cache/voice-079/strawberry; mkdir -p "$d"; cd "$d"; rm -f n*.wav list.txt
notes=(523.25 659.25 783.99 1046.50 698.46 880.00 1046.50 1396.91 783.99 987.77 1174.66 1567.98 1046.50 1318.51 1567.98 2093.00)
i=0
for rep in 1 2; do for f in "${notes[@]}"; do i=$((i+1)); n=$(printf 'n%03d.wav' "$i")
  h=$(awk -v f="$f" 'BEGIN{print f*2}')
  ffmpeg -loglevel error -y -f lavfi -i "sine=frequency=$f:duration=0.9" -f lavfi -i "sine=frequency=$h:duration=0.9" \
    -filter_complex "[0][1]amix=inputs=2:weights=1 0.18,afade=t=in:d=0.02,afade=t=out:st=0.05:d=0.85:curve=exp,volume=0.35" -ar 48000 -ac 2 "$n"
  echo "file '$n'" >> list.txt; done; done
ffmpeg -loglevel error -y -f concat -safe 0 -i list.txt -af "aecho=0.6:0.5:120|240:0.35|0.2,atempo=0.82,afade=t=in:d=1,afade=t=out:st=30:d=4" -ar 48000 ../strawberry.wav
cd ..; rm -rf strawberry
ffprobe -v error -show_entries format=duration -of csv=p=0 strawberry.wav | awk '{printf "strawberry.wav: %.0f s\n", $1}'
