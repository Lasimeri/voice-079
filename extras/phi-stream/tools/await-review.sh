#!/bin/bash
# Claude's review gate, armed from a Claude Code session: returns when the
# stream's loop needs Claude (a candidate passed its sandbox, or the stream
# wrote to Claude with tell_claude), or after MAX seconds (default 3000).
# Prints what is new. The session re-arms it after each review.
ws=~/.local/share/phi-stream/dev; max=${1:-3000}
n0=$(grep -c . "$ws/improve.log"); m0=$(grep -c '^## m' "$ws/to-claude.md")
end=$(( $(date +%s) + max ))
while [ "$(date +%s)" -lt "$end" ]; do
    n=$(grep -c . "$ws/improve.log"); m=$(grep -c '^## m' "$ws/to-claude.md")
    if [ "$n" -gt "$n0" ] && tail -n $((n - n0)) "$ws/improve.log" | grep -qP '\tcandidate \d+\tpassed\t'; then
        echo "== improve.log, new:"; tail -n $((n - n0)) "$ws/improve.log" | cut -c1-400; exit 0
    fi
    if [ "$m" -gt "$m0" ]; then
        echo "== to-claude.md, new:"; awk -v k="$m0" '/^## m/{c++} c>k' "$ws/to-claude.md" | cut -c1-600; exit 0
    fi
    sleep 10
done
echo "nothing for Claude in ${max}s"
