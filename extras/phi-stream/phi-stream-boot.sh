#!/bin/bash
# phi-stream-boot.sh: at login, the Phi Stream dev service with the
# self-improvement loop, once the GPU is ready. Heavy (loads the 35B over the
# GPU and both Xeon Phi cards); PHI_STREAM_AUTOBOOT=0 to skip, and it stays
# off at login after `state079 set phi off` (the person's stop is kept).
[ "${PHI_STREAM_AUTOBOOT:-1}" = 0 ] && exit 0
[ "$("$HOME/tts079/state079" get phi on)" = off ] && exit 0
repo="$HOME/Intel Phi Stream"
[ -d "$repo" ] || exit 0
# Wait for the NVIDIA GPU to answer.
for _ in $(seq 1 60); do nvidia-smi -L >/dev/null 2>&1 && break; sleep 2; done
sleep 5
# Restart the service if it wedges (diag.md stale while the process is up).
setsid "$HOME/tts079/phi-stream-watchdog.sh" > /dev/null 2>&1 < /dev/null &
# The repository's own launcher with the one command line every start uses
# (phi-stream.opts; the watchdog's restarts and the measurement use the same
# launcher): in tmux, its window opened once the model runs. libptracer.so
# (PHI_STREAM_PRELOAD) lets the watchdog take a wedge's thread stacks.
cd "$repo" || exit 1
[ -f "$HOME/tts079/libptracer.so" ] && export PHI_STREAM_PRELOAD="$HOME/tts079/libptracer.so"
# shellcheck disable=SC2046
exec scripts/phi-stream.sh dev $(cat "$HOME/tts079/phi-stream.opts")
