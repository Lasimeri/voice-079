#!/bin/bash
# Bring the bedroom laptop's microphone into the one whisper listener. It pulls
# Yg6's echo-cancelled mic (yg6_mic_clean, 079's playback already removed) over
# SSH and plays it into the desktop's mix_mic sink, where it is mixed with the
# desktop mic. The listener reads mix_mic.monitor (VOICE079_SRC). Reconnects if
# the link drops. Stop: pkill -f bedroom-mic-in.sh
set -u
ip="${YG6_IP:-192.168.0.125}"
# SSH password from a private file, never in the code (see lapcams.sh).
pf="${LAPTOP_SSH_PASS_FILE:-$HOME/.config/voice-079/laptop-ssh-pass}"
A="$HOME/.cache/voice-079/laptop-askpass.sh"
umask 077; printf '#!/bin/sh\nexec cat "%s"\n' "$pf" > "$A"; chmod 700 "$A"
while :; do
    env SSH_ASKPASS="$A" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
        ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no \
            -o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
            -o ServerAliveInterval=15 -o StrictHostKeyChecking=no "lasimeri@$ip" \
        "export XDG_RUNTIME_DIR=/run/user/1000; exec pw-record --target yg6_mic_clean --format s16 --rate 48000 --channels 2 -" 2>/dev/null |
        pw-cat --playback --target mix_mic --format s16 --rate 48000 --channels 2 - 2>/dev/null
    sleep 2
done
