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
# Started by the watchdog's own launcher: the one command line for both a
# login and a restart after a wedge (same flags, libptracer.so preloaded so a
# wedge's thread stacks can be taken), never two copies drifting apart.
"$HOME/tts079/phi-stream-watchdog.sh" restart
# Its terminal window on the desktop once the model is running (what
# `phi-stream.sh start` did; the window reconnects across restarts and
# reloads onto new builds). PHI_STREAM_WINDOW=0: none. At most 20 minutes.
[ "${PHI_STREAM_WINDOW:-1}" = 0 ] && exit 0
for _ in $(seq 1 400); do
    [ -n "$("$repo/target/release/phi-stream" status 2>/dev/null)" ] && exec "$repo/scripts/phi-stream.sh" window
    sleep 3
done
exit 0
