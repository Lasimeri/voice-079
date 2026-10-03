#!/bin/bash
# restart-claude.sh OLDPID [DELAY]: end this Claude Code once its last reply
# has been spoken (DELAY seconds, default 25), then open it again in a NEW
# Konsole process (claude-voice.sh: the same conversation, Remote Control and
# the Discord channel on). A new process is the point: Konsole reads
# EnableSecuritySensitiveDBusAPI when it starts, and the voice types into
# Claude Code over that D-Bus API without bringing its window forward.
old=${1:?pid of the running claude}
sleep "${2:-25}"
kill -TERM "$old" 2>/dev/null
for _ in $(seq 1 50); do kill -0 "$old" 2>/dev/null || break; sleep 0.2; done
kill -0 "$old" 2>/dev/null && kill -9 "$old"
setsid konsole --separate -e "$HOME/tts079/claude-voice.sh" > /dev/null 2>&1 < /dev/null &
