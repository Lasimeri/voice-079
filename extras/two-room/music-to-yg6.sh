#!/bin/bash
# music-to-yg6.sh: the desktop's music player (VLC) heard on the bedroom
# laptop's speakers (Yg6) instead of the desktop's. A null sink,
# bedroom_music, takes VLC's stream; its monitor is sent over SSH as 48 kHz
# stereo PCM to the laptop's echo-cancelled speaker sink (yg6_aec_ref, so
# the laptop's microphone does not hear it back). Reconnects on its own.
#   start: setsid music-to-yg6.sh &      stop: music-to-yg6.sh stop
# Stop moves VLC back to the default sink and removes bedroom_music.
set -u
ip="${YG6_IP:-192.168.0.125}"
pidf="$HOME/.cache/voice-079/yg6-music.pid"
vlc_inputs() { pactl list sink-inputs 2>/dev/null | awk '/^Sink Input #/{id=substr($3,2)} /application.name = "VLC/{print id}'; }
if [ "${1:-}" = stop ]; then
    [ -f "$pidf" ] && kill -- -"$(cat "$pidf")" 2>/dev/null
    rm -f "$pidf"
    for i in $(vlc_inputs); do pactl move-sink-input "$i" "$(pactl get-default-sink)"; done
    m=$(pactl list short modules | awk '/module-null-sink/ && /sink_name=bedroom_music/ {print $1}')
    [ -n "$m" ] && pactl unload-module "$m"
    echo "music-to-yg6: stopped; VLC back on $(pactl get-default-sink)"
    exit 0
fi
echo $$ > "$pidf"
pactl list short sinks | grep -q bedroom_music ||
    pactl load-module module-null-sink sink_name=bedroom_music sink_properties=device.description=Bedroom-laptop-music > /dev/null
# VLC's stream (and the next, as each track opens a new one) onto it, or
# back to the default sink while follow079 has the person at the desk
# ($XDG_RUNTIME_DIR/speak-079/music_room: bedroom, the default, or desk).
roomf="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079/music_room"
( while :; do
    if [ "$(cat "$roomf" 2>/dev/null || echo bedroom)" = desk ]; then to=$(pactl get-default-sink); else to=bedroom_music; fi
    for i in $(vlc_inputs); do pactl move-sink-input "$i" "$to" 2>/dev/null; done
    sleep 2
done ) &
pf="${LAPTOP_SSH_PASS_FILE:-$HOME/.config/voice-079/laptop-ssh-pass}"
A="$HOME/.cache/voice-079/laptop-askpass.sh"
umask 077; printf '#!/bin/sh\nexec cat "%s"\n' "$pf" > "$A"; chmod 700 "$A"
while :; do
    parec --device=bedroom_music.monitor --format=s16le --rate=48000 --channels=2 --raw 2>/dev/null |
        env SSH_ASKPASS="$A" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
            ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no \
                -o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
                -o ServerAliveInterval=15 -o StrictHostKeyChecking=no "lasimeri@$ip" \
            "export XDG_RUNTIME_DIR=/run/user/1000; exec pw-cat --playback --raw --target yg6_aec_ref --format s16 --rate 48000 --channels 2 -" 2>/dev/null
    sleep 2
done
