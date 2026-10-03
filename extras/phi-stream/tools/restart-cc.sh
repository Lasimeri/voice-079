#!/bin/bash
# Restart Claude Code with the Discord channel: wait for the old one's last
# message, end it, then open it again in a new Konsole tab, same conversation.
old=$1
sleep 8
kill -TERM "$old" 2>/dev/null
for i in $(seq 1 50); do kill -0 "$old" 2>/dev/null || break; sleep 0.2; done
konsole --new-tab --workdir "$HOME" -e fish -l -c 'claude -c --dangerously-skip-permissions --channels plugin:discord@claude-plugins-official; exec fish' >> "$HOME/.cache/voice-079/restart-cc.log" 2>&1 &
