#!/bin/bash
# room-link.sh: the desktop's audio links to one laptop's room, for follow079.
# Three streams over SSH, each reconnecting on its own:
#   o79_ROOM (a silent desktop sink speak079d plays 079 into when the person
#     is in that room) -> the laptop's echo-cancelled speaker sink TARGET;
#   music_ROOM (the music player's sink then) -> the same;
#   the laptop's clean mic MICSRC -> the desktop's mix_mic sink, as client
#     ROOM-mic, which follow079 mutes unless the person is there.
# Raw PCM both ways with parec/pacat and pw-cat --raw (PipeWire 1.6's pw-cat
# read stdin as a sound file without it).
#   room-link.sh ROOM IP TARGET MICSRC      stop: room-link.sh stop ROOM
set -u
[ "${1:-}" = stop ] && {
    p="$HOME/.cache/voice-079/link-${2:?room}.pid"
    [ -f "$p" ] && kill -- -"$(cat "$p")" 2>/dev/null
    rm -f "$p"
    exit 0
}
room=${1:?room} ip=${2:?ip} target=${3:?target} micsrc=${4:?micsrc}
echo $$ > "$HOME/.cache/voice-079/link-$room.pid"
for s in "o79_$room" "music_$room"; do
    pactl list short sinks | grep -q "	$s	" ||
        pactl load-module module-null-sink sink_name="$s" sink_properties=device.description="$s" > /dev/null
done
pactl list short sinks | grep -q '	mix_mic	' ||
    pactl load-module module-null-sink sink_name=mix_mic sink_properties=device.description=Mixed-mic > /dev/null
pf="${LAPTOP_SSH_PASS_FILE:-$HOME/.config/voice-079/laptop-ssh-pass}"
A="$HOME/.cache/voice-079/laptop-askpass.sh"
umask 077; printf '#!/bin/sh\nexec cat "%s"\n' "$pf" > "$A"; chmod 700 "$A"
lssh() {
    env SSH_ASKPASS="$A" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no \
            -o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
            -o ServerAliveInterval=15 -o StrictHostKeyChecking=no "lasimeri@$ip" "$@"
}
out() {   # SINK: its monitor to the laptop's speakers
    while :; do
        parec --device="$1.monitor" --format=s16le --rate=48000 --channels=2 --raw 2> /dev/null |
            lssh "export XDG_RUNTIME_DIR=/run/user/1000; exec pw-cat --playback --raw --target $target --format s16 --rate 48000 --channels 2 -" 2> /dev/null
        sleep 2
    done
}
out "o79_$room" &
out "music_$room" &
while :; do
    lssh "export XDG_RUNTIME_DIR=/run/user/1000; exec parec --device=$micsrc --format=s16le --rate=48000 --channels=2 --raw" 2> /dev/null |
        pacat --playback --device=mix_mic --format=s16le --rate=48000 --channels=2 --raw \
            --volume="${ROOM_MIC_VOLUME:-163840}" --client-name="$room-mic" 2> /dev/null
    sleep 2
done
