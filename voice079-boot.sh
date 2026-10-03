#!/bin/bash
# voice079-boot.sh: at login (KDE autostart, voice079.desktop), the whole
# desktop assistant, once the sound server and the MG-XU are up (at most 90 s):
# the voice (listening, echo cancellers, speaker, typer with the mute key) and
# the cameras. Wake mode by default so room and video audio do not trigger it;
# VOICE079_BOOT_MODE= (empty) for open mic, --ptt for push to talk.
for _ in $(seq 1 90); do
    pactl info > /dev/null 2>&1 &&
        pactl list sources short 2>/dev/null | grep -q "Yamaha_Corporation_MG-XU" && break
    sleep 1
done
"$HOME/tts079/voice079" start ${VOICE079_BOOT_MODE---wake}
# The cameras (split when two are present, else a single view); off with
# VOICE079_BOOT_CAM=0. Needs the Wayland session, so give it a moment.
if [ "${VOICE079_BOOT_CAM:-1}" != 0 ]; then
    sleep 3
    n=$(for v in /sys/class/video4linux/video*; do [ -e "$v" ] && cat "$v/name"; done 2>/dev/null | sort -u | wc -l)
    if [ "$n" -ge 2 ]; then
        "$HOME/tts079/cam079" split > /dev/null 2>&1 || "$HOME/tts079/cam079" view > /dev/null 2>&1
    else
        "$HOME/tts079/cam079" view > /dev/null 2>&1
    fi
fi
