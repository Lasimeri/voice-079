#!/bin/bash
# On a laptop (copied to ~/.cache/lapcam/mic-boot.sh): the echo-cancelled
# pair the desktop's audio links use (NAME_aec_ref, a sink on the laptop's
# current output; NAME_mic_clean, its microphone with that sink's sound
# removed), and micwatch on the clean mic (/dev/shm/mic.status, one line a
# second: whether someone is speaking), restarted if it stops.
#   mic-boot.sh NAME ROOM      (e.g. yg6 bedroom, e16 laptop)
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
name=${1:?name} room=${2:?room}
c=$(dirname "$(readlink -f "$0")")
for _ in $(seq 1 60); do pactl info > /dev/null 2>&1 && break; sleep 1; done
if ! pactl list short sinks | grep -q "${name}_aec_ref"; then
    pactl load-module module-echo-cancel aec_method=webrtc \
        source_master="$(pactl get-default-source)" sink_master="$(pactl get-default-sink)" \
        source_name="${name}_mic_clean" sink_name="${name}_aec_ref" > /dev/null
fi
while :; do
    parec --device="${name}_mic_clean" --format=s16le --rate=16000 --channels=1 --raw 2> /dev/null |
        "$c/micwatch" "$room" /dev/shm/mic
    sleep 2
done
