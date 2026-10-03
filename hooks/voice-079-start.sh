#!/bin/bash
# Claude Code SessionStart hook for the voice (~/tts079/voice079): from the
# moment Claude Code starts, what the person says is typed into this session.
# The session's Konsole is named for the typer at once ($dir/konsole): the
# prompt hook names it only when a message is sent, so a session started by
# hand heard nothing until something was typed into it. The listener is
# started if it is not running, in the mode last chosen (state079 listen:
# wake, ptt or open; off falls back to wake, the person's last choice) and
# on the follow-me mix (mix_mic.monitor) when follow079 is on.
# Off: touch ~/.claude/hooks/speak-079.off
set -u
[ -e "$HOME/.claude/hooks/speak-079.off" ] && exit 0
here="$HOME/tts079"
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079"
mkdir -p "$dir"
cat > /dev/null
[ -n "${KONSOLE_DBUS_SERVICE:-}" ] && [ -n "${KONSOLE_DBUS_SESSION:-}" ] &&
    printf '%s %s\n' "$KONSOLE_DBUS_SERVICE" "$KONSOLE_DBUS_SESSION" > "$dir/konsole"
mode=$("$here/state079" get listen wake)
case "$mode" in open|wake|ptt) ;; *) mode=wake ;; esac
case "$mode" in
    wake) said="say Claude first" ;;
    ptt) said="hold \` to talk" ;;
    *) said="open mic" ;;
esac
if [ -f "$dir/listen.pid" ] && kill -0 "$(cat "$dir/listen.pid")" 2>/dev/null; then
    "$here/speak079d" start > /dev/null 2>&1
    echo "[voice: listening ($said), typed into this session]"
    exit 0
fi
# At login voice079-boot.sh starts the listener itself: left to it.
if pgrep -f 'voice079-boot[.]sh' > /dev/null; then
    echo "[voice: starting at login ($said), typed into this session]"
    exit 0
fi
flag=""; [ "$mode" != open ] && flag="--$mode"
src=""
if [ "$("$here/state079" get follow off)" = on ] &&
    pactl list sources short 2>/dev/null | grep -q 'mix_mic[.]monitor'; then
    src=mix_mic.monitor
fi
# Detached: the echo cancellers and noise reduction take seconds to load,
# and Claude Code does not wait for them.
VOICE079_SRC="$src" setsid flock -n -o "$dir/start.lock" "$here/voice079" start $flag \
    >> "$HOME/.local/share/speak-079/listen.log" 2>&1 < /dev/null &
echo "[voice: listener started ($said), typed into this session]"
exit 0
