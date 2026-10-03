#!/bin/bash
# phi-stream-boot.sh: at login, the Phi Stream dev service with the
# self-improvement loop, once the GPU is ready. Heavy (loads the 35B over the
# GPU and both Xeon Phi cards); PHI_STREAM_AUTOBOOT=0 to skip.
[ "${PHI_STREAM_AUTOBOOT:-1}" = 0 ] && exit 0
repo="$HOME/Intel Phi Stream"
[ -d "$repo" ] || exit 0
# Wait for the NVIDIA GPU to answer.
for _ in $(seq 1 60); do nvidia-smi -L >/dev/null 2>&1 && break; sleep 2; done
sleep 5
cd "$repo" || exit 1
# Restart the service if it wedges (diag.md stale while the process is up).
setsid "$HOME/tts079/phi-stream-watchdog.sh" > /dev/null 2>&1 < /dev/null &
exec scripts/phi-stream.sh dev -c 204800 --kv-q8 --frame agent --temp 0.5 \
    --top-p 0.95 --min-p 0.05 --guide --goal-probe --chain-against --improve
