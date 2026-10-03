#!/bin/bash
# voice079-boot.sh: at login (KDE autostart, voice079.desktop), the whole
# desktop assistant, once the sound server and the MG-XU are up (at most 90 s):
# the voice (echo cancellers, neural noise reduction, listening, speaker, typer
# with the mute key), the cameras (face tracking, the laptops' feeds, the
# self-arranging layout). Each comes back as it was at shutdown: state079
# keeps what the person last chose (listen mode, cameras on or off, laptop
# feeds, layout, screen, 079's volume); with nothing kept, the defaults below.
st="$HOME/tts079/state079"
for _ in $(seq 1 90); do
    pactl info > /dev/null 2>&1 &&
        pactl list sources short 2>/dev/null | grep -q "Yamaha_Corporation_MG-XU" && break
    sleep 1
done
# Listening mode: as last chosen (wake: "Claude, ..." first, with a short
# follow-up window after 079 speaks; ptt; open; off), the trigger word when
# nothing is kept. VOICE079_BOOT_MODE overrides (empty for open mic, --ptt).
case "$("$st" get listen wake)" in
    off)  mode=off ;;
    open) mode="" ;;
    ptt)  mode=--ptt ;;
    *)    mode=--wake ;;
esac
mode="${VOICE079_BOOT_MODE-$mode}"
if [ "$mode" != off ]; then
    "$HOME/tts079/voice079" start $mode
    # 079's own voice at the level last set (taken at logout), else
    # VOICE079_VOLUME, else 40%; the music and the master are never touched.
    for _ in $(seq 1 20); do pactl list short sinks 2>/dev/null | grep -q voice079_out && break; sleep 0.5; done
    pactl set-sink-volume voice079_out "${VOICE079_VOLUME:-$("$st" get volume 40%)}" 2>/dev/null
fi
# The cameras, as last left: split (the inside camera through the face
# tracker, the outside one beside it; either may be missing and joins when
# plugged in), view, start (no window) or off. VOICE079_BOOT_CAM=0: none.
# Needs the Wayland session, so give it a moment.
cams=$("$st" get cams split)
if [ "${VOICE079_BOOT_CAM:-1}" != 0 ] && [ "$cams" != off ]; then
    sleep 3
    case "$cams" in
        view|start) "$HOME/tts079/cam079" "$cams" > /dev/null 2>&1 ;;
        *) "$HOME/tts079/cam079" split > /dev/null 2>&1 || "$HOME/tts079/cam079" view > /dev/null 2>&1 ;;
    esac
fi
# The two laptops' face-tracked cameras beside these (lapcams.sh; it keeps
# retrying until each laptop is up), unless last stopped.
# VOICE079_BOOT_LAPCAMS=0: none.
if [ "${VOICE079_BOOT_LAPCAMS:-1}" != 0 ] && [ "$("$st" get lapcams on)" != off ] && [ -x "$HOME/tts079/lapcams.sh" ]; then
    setsid "$HOME/tts079/lapcams.sh" > "$HOME/.cache/voice-079/lapcams.log" 2>&1 < /dev/null &
fi
# Camera windows lay themselves out as cameras connect and drop (resident KWin
# script; the layout and screen last chosen, featured on DP-2 by default).
[ -x "$HOME/tts079/cam-grid-place" ] && "$HOME/tts079/cam-grid-place" --watch > /dev/null 2>&1
# The audio following the person between the desk and the bedroom (follow079),
# if it was on at shutdown; it needs the cameras' feeds, so it starts last.
if [ "$("$st" get follow off)" = on ] && [ -x "$HOME/tts079/follow079" ]; then
    sleep 20
    "$HOME/tts079/follow079" start > /dev/null 2>&1
fi
exit 0
