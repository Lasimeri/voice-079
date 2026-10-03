#!/bin/bash
# voice079-boot.sh: at login (KDE autostart, voice079.desktop), the whole
# desktop assistant, once the sound server and the MG-XU are up (at most 90 s):
# the voice (echo cancellers, neural noise reduction, listening, speaker, typer
# with the mute key), the cameras (face tracking, the laptops' feeds, the
# self-arranging layout).
for _ in $(seq 1 90); do
    pactl info > /dev/null 2>&1 &&
        pactl list sources short 2>/dev/null | grep -q "Yamaha_Corporation_MG-XU" && break
    sleep 1
done
# Listening mode: the trigger word is required ("Claude, ..."), with a short
# follow-up window after 079 speaks; the person's choice. VOICE079_BOOT_MODE
# overrides (empty for open mic, --ptt for push to talk).
mode="${VOICE079_BOOT_MODE---wake}"
"$HOME/tts079/voice079" start $mode
# 079's own voice at the level the person chose (VOICE079_VOLUME, default 40%;
# their music and the master volume are never touched).
for _ in $(seq 1 20); do pactl list short sinks 2>/dev/null | grep -q voice079_out && break; sleep 0.5; done
pactl set-sink-volume voice079_out "${VOICE079_VOLUME:-40%}" 2>/dev/null
# The cameras: split runs the inside camera through the face tracker and the
# outside one beside it, and copes with either missing (it joins when plugged
# in). VOICE079_BOOT_CAM=0: none. Needs the Wayland session, so give it a moment.
if [ "${VOICE079_BOOT_CAM:-1}" != 0 ]; then
    sleep 3
    "$HOME/tts079/cam079" split > /dev/null 2>&1 || "$HOME/tts079/cam079" view > /dev/null 2>&1
fi
# The two laptops' face-tracked cameras beside these (lapcams.sh; it keeps
# retrying until each laptop is up). VOICE079_BOOT_LAPCAMS=0: none.
if [ "${VOICE079_BOOT_LAPCAMS:-1}" != 0 ] && [ -x "$HOME/tts079/lapcams.sh" ]; then
    setsid "$HOME/tts079/lapcams.sh" > "$HOME/.cache/voice-079/lapcams.log" 2>&1 < /dev/null &
fi
# Camera windows lay themselves out as cameras connect and drop (resident KWin
# script; featured layout: the person large, the rest stacked).
[ -x "$HOME/tts079/cam-grid-place" ] && "$HOME/tts079/cam-grid-place" --watch > /dev/null 2>&1
exit 0
