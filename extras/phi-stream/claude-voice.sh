#!/bin/bash
# claude-voice.sh: at login (KDE autostart, claude-voice.desktop), Claude Code
# in this Konsole window, with the voice. The window is named for the voice's
# typer first ($dir/konsole, which claude-focus reads; the prompt hook writes
# it too, but only once a prompt is sent), then Claude Code is started the
# way the person starts it: fish, a login shell, continuing the last
# conversation, the Discord channel on, Remote Control on (the Claude app or
# claude.ai/code can drive this session).
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079"
mkdir -p "$dir"
[ -n "${KONSOLE_DBUS_SERVICE:-}" ] && [ -n "${KONSOLE_DBUS_SESSION:-}" ] &&
    printf '%s %s\n' "$KONSOLE_DBUS_SERVICE" "$KONSOLE_DBUS_SESSION" > "$dir/konsole"
cd "$HOME" || exit 1
exec /usr/bin/fish -l -c 'claude -c --remote-control --dangerously-skip-permissions --channels plugin:discord@claude-plugins-official'
