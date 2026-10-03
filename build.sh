#!/bin/bash
# Build the voice-079 tools:
#   tts079    - the SCP-079 text-to-speech front end (tcc)
#   ptt079    - the push/open-mic typer that feeds Claude Code (tcc)
#   listen079 - the whisper.cpp listener (gcc), with optional sherpa-onnx voice-ID
#
# WHISPER_DIR:     a whisper.cpp checkout built with CUDA or HIP (default ~/whisper.cpp)
# SHERPA_ONNX_DIR: optional sherpa-onnx shared release (default ~/sherpa-onnx)
# TTS079_ADDON:    the SBTalker 079 voice addon folder (for tts079_data.h)
set -euo pipefail
cd "$(dirname "$0")"
w="${WHISPER_DIR:-$HOME/whisper.cpp}"

[ -f tts079_data.h ] || ./gen-data.sh "${TTS079_ADDON:-$HOME/.cache/voice-079/godot-tts-079/addons/tts_079}" > tts079_data.h
tcc -O2 -o tts079 tts079.c
tcc -O2 -o ptt079 ptt079.c || gcc -O2 -o ptt079 ptt079.c

# Voice identification through sherpa-onnx's C API, when present.
s="${SHERPA_ONNX_DIR:-$HOME/sherpa-onnx}"
sl="$s/sherpa-onnx-v1.13.8-linux-x64-shared-no-tts-lib/lib"
spk=()
if [ -f "$s/include/sherpa-onnx/c-api/c-api.h" ] && [ -f "$sl/libsherpa-onnx-c-api.so" ]; then
    spk=(-DHAVE_SPEAKER -I"$s/include" -L"$sl" -lsherpa-onnx-c-api -Wl,-rpath,"$sl")
fi
gcc -O2 -o listen079 listen079.c -I"$w/include" -I"$w/ggml/include" "${spk[@]}" \
    -L"$w/build/bin" -lwhisper -lggml -lggml-base -lm \
    -Wl,-rpath,"$w/build/bin"

echo "built tts079, ptt079 and listen079"
