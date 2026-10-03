# voice-079

Talk to [Claude Code](https://claude.com/claude-code) out loud, fully local, and hear it answer in the SCP-079 voice. No cloud speech services: the microphone is transcribed on your own GPU, the reply is spoken by a local synthesizer, and the words are typed straight into the Claude Code terminal.

This is the voice half of the setup. The cameras and face tracking live in a companion repo, **optical-079**.

## What it does

- **Listens** on an open microphone (whisper.cpp, CUDA or ROCm), with WebRTC echo cancellation so the mic never hears the synthesizer or your music.
- **Decides** whether a sentence was meant for the assistant: an optional wake word ("Claude"), filler/acknowledgement filtering, and optional speaker identification (sherpa-onnx) so it answers you and not the TV.
- **Types** the recognized text into the Claude Code window by taking over a keyboard through `uinput`. A numpad-period key toggles mute.
- **Speaks** the reply in the SCP-079 voice (a low, slow synthesizer chain), speaking only the first paragraph so you hear what matters, not a wall of text.
- **Feeds context** into Claude through two hooks: what you actually heard since your last message, and (with optical-079) the camera frame at the moment you spoke.

## Components

| File | What it is |
| --- | --- |
| `tts079.c` | the SCP-079 text-to-speech front end (built with tcc) |
| `speak079d` | the speaking daemon and status line (ffmpeg voice chain: low pitch, slow tempo) |
| `listen079.c` | the listener: whisper.cpp transcription + wake/filler logic + optional speaker ID |
| `ptt079.c` | takes a keyboard (EVIOCGRAB), types recognized text via uinput, numpad-period = mute |
| `voice079` | the launcher: `voice079 start [--wake|--ptt]`, `voice079 stop` |
| `voice079-boot.sh` | login autostart: waits for the sound card, then starts everything |
| `aec-all.conf` | PipeWire echo-canceller (monitor mode): cancels everything the speakers play |
| `claude-focus`, `claude-voice.sh` | focus the Claude window; open Claude Code at login |
| `gen-data.sh` | generates `tts079_data.h` from the SBTalker voice addon |
| `hooks/voice-079-prompt.sh` | Claude Code UserPromptSubmit hook: tells Claude what you heard + attaches a camera frame |
| `hooks/voice-079-stop.sh` | Claude Code Stop hook: speaks the reply's first paragraph |
| `70-ptt079.rules` | udev rule so the typer can read the keyboard and write `/dev/uinput` without root |

## Requirements

- Linux with **PipeWire** (echo cancellation), **KDE Plasma** recommended (window focusing uses KWin/Konsole D-Bus).
- **tcc** and **gcc**.
- **whisper.cpp** built with GPU support, plus the `large-v3-turbo` model. See below.
- Optional: **sherpa-onnx** (speaker identification). Without it, voice-ID is simply off.
- The SBTalker "079" voice addon (the TTS voice data), placed where `gen-data.sh` can read it.

## Build

```bash
# 1. whisper.cpp with GPU support + the model
git clone https://github.com/ggml-org/whisper.cpp ~/whisper.cpp
cd ~/whisper.cpp && cmake -B build -DGGML_CUDA=ON && cmake --build build -j --config Release
./models/download-ggml-model.sh large-v3-turbo
mkdir -p ~/models/whisper && cp models/ggml-large-v3-turbo.bin ~/models/whisper/

# 2. (optional) sherpa-onnx shared release unzipped into ~/sherpa-onnx/

# 3. build this
cd voice-079
WHISPER_DIR=~/whisper.cpp ./build.sh
```

`build.sh` produces `tts079`, `ptt079`, and `listen079`. On an AMD GPU, build whisper.cpp with `-DGGML_HIPBLAS=ON` instead of CUDA.

## Run

```bash
# install the udev rule once so the typer needs no root
sudo cp 70-ptt079.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger

./voice079 start          # open mic
./voice079 start --wake   # only answer sentences that start with "Claude"
./voice079 stop
```

Then hook it into Claude Code by pointing `~/.claude/settings.json` at the two scripts in `hooks/` (UserPromptSubmit -> `voice-079-prompt.sh`, Stop -> `voice-079-stop.sh`) and setting the status line to `speak079d now`.

### The "only answer when I say Claude" toggle

```bash
touch   "$XDG_RUNTIME_DIR/speak-079/phone"   # ON: ignore the room, require "Claude ..."
rm -f   "$XDG_RUNTIME_DIR/speak-079/phone"   # OFF: answer everything heard
```

Useful when you are watching a video or on a call.

## Notes

- Paths use `$HOME`; device names in `aec-all.conf` are examples (a Yamaha MG-XU mic and a Schiit DAC). Change the two `target.object` lines to your own, found with `pactl list sources short` / `pactl list sinks short`.
- The echo-canceller only listens; it changes no routing or volumes.
- This repo expects a working Claude Code install. It is not affiliated with Anthropic.
