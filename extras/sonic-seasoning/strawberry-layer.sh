#!/bin/bash
# strawberry-layer.sh: the "sweet / strawberry" soundtrack (Spence's
# crossmodal sweet profile: high, consonant, major, soft, slow) woven quietly
# under whatever music is playing, and ONLY while music is playing: never on
# its own. It watches the MPRIS players once a second; the moment none is
# playing, the layer stops. The music's own volume is never touched.
#   start: setsid strawberry-layer.sh &     stop: kill $(cat $XDG_RUNTIME_DIR/speak-079/strawberry.pid)
#   STRAWBERRY_VOL: the layer's level under the music (default 0.3)
set -u
rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/speak-079"; mkdir -p "$rt"
echo $$ > "$rt/strawberry.pid"
wav="$HOME/.cache/voice-079/strawberry.wav"
vol="${STRAWBERRY_VOL:-0.3}"
[ -s "$wav" ] || { echo "strawberry-layer: no $wav" >&2; exit 1; }

playing() {
    local s
    for s in $(qdbus6 2>/dev/null | grep -o 'org\.mpris\.MediaPlayer2\.[^ ]*'); do
        [ "$(qdbus6 "$s" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.PlaybackStatus 2>/dev/null)" = Playing ] && return 0
    done
    return 1
}
lp=""
stop() { if [ -n "$lp" ]; then kill -- -"$lp" 2>/dev/null; kill "$lp" 2>/dev/null; lp=""; fi; }
trap 'stop; rm -f "$rt/strawberry.pid"; exit 0' TERM INT

while :; do
    if playing; then
        if [ -z "$lp" ] || ! kill -0 "$lp" 2>/dev/null; then
            setsid bash -c "while :; do pw-play --volume $vol '$wav'; done" > /dev/null 2>&1 < /dev/null &
            lp=$!
        fi
    else
        stop
    fi
    sleep 1
done
