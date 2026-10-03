#!/bin/bash
# Stream the SCP-079 voice to the bedroom laptop (Yg6, 192.168.0.125) in
# addition to the desktop. It taps the monitor of the voice079_out sink (the
# sink speak079d plays 079 into), so only 079's voice is sent and nothing on
# the desktop's own audio routing or volume changes. Reconnects if the link
# drops. Stop: pkill -f audio-to-yg6
set -u
echo $$ > "$HOME/.cache/voice-079/yg6-audio.pid"
ip="${YG6_IP:-192.168.0.125}"
# SSH password from a private file, never in the code (see lapcams.sh).
pf="${LAPTOP_SSH_PASS_FILE:-$HOME/.config/voice-079/laptop-ssh-pass}"
A="$HOME/.cache/voice-079/laptop-askpass.sh"
umask 077; printf '#!/bin/sh\nexec cat "%s"\n' "$pf" > "$A"; chmod 700 "$A"
src="${VOICE079_OUT_MONITOR:-voice079_out.monitor}"
while :; do
    # parec, not pw-record: pw-record --target does not resolve a monitor's
    # name and silently recorded the default source (the desk microphone).
    parec --device="$src" --format=s16le --rate=48000 --channels=2 --raw 2>/dev/null |
        env SSH_ASKPASS="$A" SSH_ASKPASS_REQUIRE=force DISPLAY=:0 \
            ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no \
                -o NumberOfPasswordPrompts=1 -o ConnectTimeout=10 \
                -o ServerAliveInterval=15 -o StrictHostKeyChecking=no "lasimeri@$ip" \
            "export XDG_RUNTIME_DIR=/run/user/1000; exec pw-cat --playback --target yg6_aec_ref --format s16 --rate 48000 --channels 2 -" 2>/dev/null
    sleep 2
done
