#!/bin/bash
# Bring the bedroom laptop's microphone into the one whisper listener. Yg6's
# echo-cancelled mic (yg6_mic_clean: 079's voice and the music sent there
# already removed) comes over SSH into the desktop's mix_mic sink, where the
# desktop's own noise-reduced mic (voice079_mic_nr) joins it through a
# loopback; the listener reads mix_mic.monitor (VOICE079_SRC). Both ends use
# parec/pacat with --raw: PipeWire 1.6's pw-cat read stdin as a sound file,
# and pw-record --target did not resolve a source by name. The bedroom side
# gets +24 dB (BEDROOM_MIC_VOLUME, pulse volume 250%), as the desk mic gets
# +26 dB in nr079. Reconnects if the link drops.
#   start: setsid bedroom-mic-in.sh &      stop: bedroom-mic-in.sh stop
set -u
ip="${YG6_IP:-192.168.0.125}"
pidf="$HOME/.cache/voice-079/yg6-mic.pid"
if [ "${1:-}" = stop ]; then
    [ -f "$pidf" ] && kill -- -"$(cat "$pidf")" 2>/dev/null
    rm -f "$pidf"
    for m in $(pactl list short modules | awk '/module-loopback/ && /sink=mix_mic/ {print $1}
                                             /module-null-sink/ && /sink_name=mix_mic/ {print $1}'); do
        pactl unload-module "$m"
    done
    echo "bedroom-mic-in: stopped"
    exit 0
fi
echo $$ > "$pidf"
pactl list short sinks | grep -q 'mix_mic' ||
    pactl load-module module-null-sink sink_name=mix_mic sink_properties=device.description=Two-room-mic > /dev/null
pactl list short modules | grep -q 'module-loopback.*sink=mix_mic' ||
    pactl load-module module-loopback source=voice079_mic_nr sink=mix_mic latency_msec=30 source_dont_move=true sink_dont_move=true > /dev/null
pf="${LAPTOP_SSH_PASS_FILE:-$HOME/.config/voice-079/laptop-ssh-pass}"
A="$HOME/.cache/voice-079/laptop-askpass.sh"
umask 077; printf '#!/bin/sh\nexec cat "%s"\n' "$pf" > "$A"; chmod 700 "$A"
while :; do
    env SSH_ASKPASS="$A" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no \
            -o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
            -o ServerAliveInterval=15 -o StrictHostKeyChecking=no "lasimeri@$ip" \
        "export XDG_RUNTIME_DIR=/run/user/1000; exec parec --device=yg6_mic_clean --format=s16le --rate=48000 --channels=2 --raw" 2>/dev/null |
        pacat --playback --device=mix_mic --format=s16le --rate=48000 --channels=2 --raw \
            --volume="${BEDROOM_MIC_VOLUME:-163840}" --client-name=bedroom-mic 2>/dev/null
    sleep 2
done
