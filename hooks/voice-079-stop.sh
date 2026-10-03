#!/bin/bash
# Claude Code Stop hook (async): the reply that just ended, said in 079's
# voice (speak079d say-stdin; a new reply supersedes what is still queued).
# The transcript feed this replaces spoke old sentences over new ones.
# Off: touch ~/.claude/hooks/speak-079.off
set -u
[ -e "$HOME/.claude/hooks/speak-079.off" ] && exit 0
msg=$(jq -r '.last_assistant_message // empty' 2>/dev/null)
[ -n "$msg" ] || exit 0
# Only what the person needs to hear: the reply's first paragraph (the
# rest is for reading). The person: "you don't need to talk every output,
# only to say what I need to hear."
msg=$(printf '%s\n' "$msg" | sed '/^<•••>$/d' | awk 'NF {p = 1} p && !NF {exit} p')
[ -n "$msg" ] || exit 0
printf '%s' "$msg" | "$HOME/tts079/speak079d" say-stdin
exit 0
