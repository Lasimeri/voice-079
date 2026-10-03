#!/bin/bash
# netlisten: hear the person through the LAPTOP microphone, processed on the
# desktop GPU. The laptop streams f32le 16 kHz mono to this desktop's TCP port;
# whisper runs here; each line is typed into Claude Code through the same
# typer FIFO ptt079 reads. Stop: netlisten stop.
set -u
dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079"
log="$HOME/.local/share/speak-079/spoken.log"
model="$HOME/models/whisper/ggml-large-v3-turbo.bin"
port="${NETLISTEN_PORT:-7079}"
here="$(cd "$(dirname "$0")" && pwd)"
case "${1:-start}" in
  stop)
    [ -f "$dir/netlisten.pid" ] && kill -- -"$(cat "$dir/netlisten.pid")" 2>/dev/null
    rm -f "$dir/netlisten.pid"; echo "netlisten stopped"; exit 0 ;;
esac
setsid bash -c '
  dir="'"$dir"'"; log="'"$log"'"; here="'"$here"'"; model="'"$model"'"; port="'"$port"'"
  echo $$ > "$dir/netlisten.pid"
  while :; do
    socat -u TCP-LISTEN:$port,reuseaddr - | "$here/listen079" "$model" 2>>"$HOME/.local/share/speak-079/netlisten.log" |
      while IFS= read -r said; do
        [ -n "$said" ] || continue
        printf "%s\theard-laptop\t%s\n" "$(date +%Y-%m-%dT%H:%M:%S.%3N)" "$said" >> "$log"
        [ -p "$dir/type.fifo" ] && printf "[voice] %s\n" "$said" > "$dir/type.fifo"
      done
    sleep 1
  done
' >/dev/null 2>&1 < /dev/null &
echo "netlisten on :$port (laptop mic -> desktop GPU)"
