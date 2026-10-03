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

## Neural noise reduction (`nr079`)

Talk at a normal volume over loud music. After the echo canceller, the mic runs through **DeepFilterNet3**, an open neural noise remover (the nearest Linux equivalent to NVIDIA Broadcast). It strips music, fans and room noise and keeps your voice, so the listener's speech gate opens on quiet speech instead of needing you to shout over the background.

```bash
mkdir -p nr
# DeepFilterNet3 LADSPA plugin, prebuilt, model inside (Rikorose/DeepFilterNet)
curl -L -o nr/libdeep_filter_ladspa.so \
  https://github.com/Rikorose/DeepFilterNet/releases/download/v0.5.6/libdeep_filter_ladspa-0.5.6-x86_64-unknown-linux-gnu.so
./nr079 start      # or let `voice079 start` do it
```

- Chain: mic -> `voice079_mic_all` (WebRTC echo cancel) -> `voice079_mic_nr` (DeepFilterNet) -> listener. The listener picks `voice079_mic_nr` automatically when it exists.
- Cost: about 30% of one CPU core, real time. A few "underrun" warnings at startup are warm-up, not a fault.
- `NR079_ATTEN` sets the attenuation limit in dB (default 60). `VOICE079_NR=0` turns it off.
- Lighter alternative: RNNoise. Build `werman/noise-suppression-for-voice` with `-DBUILD_LADSPA_PLUGIN=ON` (VST/LV2 off), copy `librnnoise_ladspa.so` into `nr/`, then run `NR079_PLUGIN=rnnoise ./nr079 start`.

## Recognition tuning

- **Beam search** (5 beams) instead of greedy decoding: fewer misheard words for a few tens of MB more GPU memory.
- **Vocabulary prompt**: `VOCAB_PROMPT` in `listen079.c` lists the names and jargon you actually say, as a plain word list rather than sentences, so a hallucinated echo of it can never read as a command. A guard drops any transcript that is just a run of that list. Edit it for your own words.
- **Name variants**: whisper's common renderings of "Claude" (Kurt, Colonel, Clawed, Clothel, Clod, ...) still count as the wake word.

## Playback behaviour

- **No ducking by default**: your other audio is left alone while 079 speaks. `VOICE079_DUCK=1` brings back the old behaviour (other streams lowered to 79% while speaking).
- **Where 079 plays**: write a sink name into `$XDG_RUNTIME_DIR/speak-079/playsink` to send the voice somewhere else (for example a null sink streamed to another room). Delete the file to play through `voice079_out` (speakers plus the echo-cancel reference) again.

## Extras

- `extras/two-room/`: talk to the assistant from a second room. `bedroom-mic-in.sh` pulls a remote laptop's echo-cancelled mic into a desktop sink. `audio-to-yg6.sh` streams the 079 voice to that laptop. Both reconnect on their own. Each is a single loop of `pw-record | ssh | pw-cat`.
- `extras/phi-stream/`: `phi-stream-watchdog.sh` restarts the Intel Phi Stream service when its `diag.md` goes stale while the process is still up (a wedge). It allows a grace period after each start, at most 6 restarts an hour, and leaves the service alone during `improve-measure.sh`. `phi-stream-boot.sh` starts the service and the watchdog at login.

### Remote-machine credentials

Scripts that SSH into the laptops never carry a password. They read it from a private file, `$LAPTOP_SSH_PASS_FILE`, default `~/.config/voice-079/laptop-ssh-pass` (create it yourself with `chmod 600`), through a small askpass helper. SSH keys are better still: with key login set up, none of this is needed.

**Gain for the listener.** `nr079` adds gain after the noise remover (`NR079_GAIN_DB`, default 18 dB) and a limiter at full scale. It applies only to what the assistant hears: the mic's own level, which Discord and other programs use, is untouched. Measure what reaches the listener with `parec --device=voice079_mic_nr --format=float32le --rate=16000 --channels=1 --raw`. Speech should land around -30 to -20 dBFS.

## Your voice, learned passively

You don't need an enrollment session. Every line you address by name ("Claude, ...") is certainly you, because songs, videos and the TV don't say it, so it teaches your voiceprint (CAM++ speaker embeddings through sherpa-onnx). After 5 such lines the voiceprint exists, saved to `~/.local/share/speak-079/voiceprint.f32`, and the assistant says so. From then on, every line in another voice is dropped in every mode. That means you can turn the trigger word off while music or a video plays: their voices don't match yours. Each later addressed line nudges the voiceprint by 8%, but only if it already sounds like you (similarity >= 0.30), so someone else saying the name can't steer it.

- `LISTEN079_SPEAKER_MIN` is the similarity below which a line is dropped (default 0.40). `speaker.log` records every line's similarity, so you can tune it from real data.
- `LISTEN079_NO_LEARN=1` turns off passive learning. "Claude, learn my voice" still enrolls explicitly (6 lines); "Claude, forget my voice" deletes the voiceprint.
- Phone mode stays name-only, because on a call it's your own voice that should be ignored.
