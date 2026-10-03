#!/bin/bash
# Build nr-st: the safetensors DeepFilterNet3 LADSPA plugin, the offline
# LADSPA host and the comparison tool. Low priority (the user's CPU is busy).
# No -ffast-math: the network follows IEEE float like the reference.
set -euo pipefail
cd "$(dirname "$0")"
cc="${CC:-gcc}"
nice -n 19 "$cc" -O2 -Wall -Wextra -shared -fPIC -fvisibility=hidden \
    -o libdeep_filter_st_ladspa.so deep_filter_st_ladspa.c dfn3.c -lm
nice -n 19 "$cc" -O2 -Wall -o ladspa-run ladspa-run.c -ldl -lm
nice -n 19 "$cc" -O2 -Wall -o cmp-audio cmp-audio.c -lm
echo "built libdeep_filter_st_ladspa.so ladspa-run cmp-audio"
