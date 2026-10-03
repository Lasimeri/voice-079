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
- `extras/two-room/netlisten.sh`: hear the person through a laptop's microphone, recognised on the desktop's GPU. The laptop streams f32le 16 kHz mono to a TCP port here, and each line goes to the same typer FIFO.
- `extras/phi-stream/`: `phi-stream-watchdog.sh` restarts the Intel Phi Stream service when its `diag.md` goes stale while the process is still up (a wedge). It allows a grace period to any service younger than 7 minutes, whoever started it, at most 6 restarts an hour, and leaves the service alone during `improve-measure.sh`. `phi-stream-boot.sh` starts the service and the watchdog at login. Every start goes through the repository's own `scripts/phi-stream.sh` with one options file, `phi-stream.opts`. `ptracer.c` (`gcc -shared -fPIC -o libptracer.so ptracer.c`) is preloaded into the service (`PHI_STREAM_PRELOAD`) so the watchdog's gdb can take a stuck service's stacks under Yama `ptrace_scope=1`.
- `extras/phi-stream/tools/`: Claude's side of developing the stream.
  - `await-review.sh`: the review gate. It returns when a candidate passes its sandbox or the stream writes to Claude, so a Claude Code session reviews and re-arms it.
  - `stall-probe.sh` + `stall.gdb`: at 45 s of a stale `diag.md`, count with gdb breakpoints whether the engine still calls `write_status_file`. This is how the 2026-10-03 "wedges" were shown to be the model resting.
  - `*-windows.sh`, `ab-check.awk`, `loopiness.c`, `lens-lines.c`, `window-count.c`: interleaved A/B measurement of the live stream (loopiness, guide KL, rates).
  - `guide-rate-ab.sh`: throughput with the guide lane on and off.
  - `mcp-call.sh`: one MCP call to the stream's management server.
  - `restart-cc.sh`: reopen Claude Code in the same conversation.
- `extras/sonic-seasoning/` (paused): `straw.sh` makes a "sweet" soundtrack after Spence's crossmodal findings (high register, consonant major harmony, soft bell attacks). `strawberry-layer.sh` loops it under whatever music an MPRIS player is playing, and only then: never alone. Unfinished: the level is far too low (about -49 LUFS).
- `docs/voice079.mmd` (+ `.html`, `.png`): the pipeline diagram.

### Remote-machine credentials

Scripts that SSH into the laptops never carry a password. They read it from a private file, `$LAPTOP_SSH_PASS_FILE`, default `~/.config/voice-079/laptop-ssh-pass` (create it yourself with `chmod 600`), through a small askpass helper. SSH keys are better still: with key login set up, none of this is needed.

**Gain for the listener.** `nr079` adds gain after the noise remover (`NR079_GAIN_DB`, default 18 dB) and a limiter at full scale. It applies only to what the assistant hears: the mic's own level, which Discord and other programs use, is untouched. Measure what reaches the listener with `parec --device=voice079_mic_nr --format=float32le --rate=16000 --channels=1 --raw`. Speech should land around -30 to -20 dBFS.

## Your voice, learned passively

You don't need an enrollment session. Every line you address by name ("Claude, ...") is certainly you, because songs, videos and the TV don't say it, so it teaches your voiceprint (CAM++ speaker embeddings through sherpa-onnx). After 5 such lines the voiceprint exists, saved to `~/.local/share/speak-079/voiceprint.f32`, and the assistant says so. From then on, every line in another voice is dropped in every mode. That means you can turn the trigger word off while music or a video plays: their voices don't match yours. Each later addressed line nudges the voiceprint by 8%, but only if it already sounds like you (similarity >= 0.30), so someone else saying the name can't steer it.

- `LISTEN079_SPEAKER_MIN` is the similarity below which a line is dropped (default 0.40). `speaker.log` records every line's similarity, so you can tune it from real data.
- `LISTEN079_NO_LEARN=1` turns off passive learning. "Claude, learn my voice" still enrolls explicitly (6 lines); "Claude, forget my voice" deletes the voiceprint.
- Phone mode stays name-only, because on a call it's your own voice that should be ignored.

## Knowing it heard you

- **The status line** (`speak079d now`) always shows the listening state, whether the trigger word is required, the voiceprint's progress ("learning 1/3", then "yours"), and what became of the last thing heard in the past 90 seconds: `sent`, or `dropped: say Claude first` / `not your voice` / `my own voice` / `just okay/yeah` / `noise`. The listener writes that record to `$XDG_RUNTIME_DIR/speak-079/lastheard`.
- **A soft click** plays every time a line is typed into Claude (`VOICE079_CLICK=0` turns it off). It goes out through the echo canceller, so the mic never takes it for speech.
- **Easier to trigger:** a line starts when sound stands `LISTEN079_GATE_DB` (default 8 dB) above the background, 6 dB more while 079 is talking. The name can come anywhere in the first four words ("Okay so, Claude, ...").
- **Learning finishes the job:** after three lines addressed by name, the voiceprint exists, and the listener turns the trigger word off by itself and says so. "Claude, I'm on the phone" brings it back for a call.
- **Defaults now:** DeepFilterNet attenuation 100 dB (its maximum) and listener gain +26 dB.

## Everything as it was, after a reboot (`state079`)

KDE's own session restore saves nothing for these programs on Wayland (a test save recorded zero applications), so the assistant keeps its own state on disk, in `~/.local/state/voice-079/state` (one `KEY=VALUE` per line; the runtime directory is tmpfs and is lost at shutdown).

- What is kept: the listening mode (`listen`: wake, ptt, open, off), the cameras (`cams`: split, view, start, off), the laptop feeds (`lapcams`), the camera layout and screen (`layout`, `screen`), whether the Phi Stream starts (`phi`), and 079's volume (`volume`).
- Only the person's own commands write it: `voice079 start/stop`, `cam079 start/view/split/stop`, `lapcams.sh` and `lapcams.sh stop`, `cam-grid-place --layout/--screen`, `state079 set KEY VALUE`. A shutdown kills everything, and nothing on that path writes, so a shutdown never saves "off". `cam079`'s own restarts use an internal `_stop` for the same reason.
- `voice079-boot.sh` and `extras/phi-stream/phi-stream-boot.sh` read it at login. With nothing kept they use the defaults: trigger word, both cameras, laptop feeds on, featured layout on DP-2, Phi Stream on, 40%.
- 079's volume changes without a command, so a logout script records it. Copy `state079-save.sh` to `~/.config/plasma-workspace/shutdown/` (Plasma's `plasma-shutdown` runs that directory).

```bash
state079 show                 # what the next login will bring back
state079 set phi off          # keep the Phi Stream from starting at login
cam-grid-place --layout grid  # kept, and applied now
```

## The audio follows you (`follow079`)

`follow079 start` moves 079's voice, the music player and the microphone the listener hears to the room you are in: desk (the desktop), bedroom (the Yg6 laptop) or laptop (the E16, its HDMI output). A room claims you when its camera's face tracker has the ADMIN reticle locked on you (3 of the last 5 s; a face that has not moved for 30 s, a picture on the wall, is no one) or its microphone hears speech (`micwatch.c`, an energy voice detector, 2 of the last 3 s). Voice moves you between the living room and the bedroom (the E16 shares the living room, and its mic hears the desk speakers, so only the desk mic speaks for that room); faces move you between the desk and the E16. A move by face needs the current room to have lost you for 20 s. Each laptop runs `laptop-mic-boot.sh NAME ROOM` at login (an echo-cancelled speaker and mic pair, `NAME_aec_ref` and `NAME_mic_clean`, and micwatch on the mic); `room-link.sh ROOM IP TARGET MICSRC` carries 079 (`o79_ROOM`), the music (`music_ROOM`) and the mic (into `mix_mic`, which the listener reads with only your room's mic open). `follow079 status` shows each room's face and voice seconds; `follow079 stop` puts everything back on the desk. It comes back at login if it was on.

Since 2026-10-03, faces alone move you (`FOLLOW_VOICE=1` brings voice back): each camera's score is how much of its frame your locked face fills, averaged over 5 s, and a room takes over when its score leads the current room's by half again on two checks running (4 s), so the desk and the E16, which both see you, no longer hold on to you. Every 2 s the loop also puts any room mic stream it finds off `mix_mic` back on it, muted: unloading `mix_mic` under them had moved them to the default sink, and the laptop's mic then played out of the desktop speakers.

While a laptop in your room plays sound no echo canceller hears (a browser video straight to the E16's HDMI), follow079 marks it (`$XDG_RUNTIME_DIR/speak-079/media`, from the laptops' media status that `lapcams.sh` in optical-079 pulls each second) and the listener sends only lines that name Claude: the video's narrator had scored 0.45 to 0.80 against the voiceprint, as high as your own voice. `FOLLOW_MEDIA_GATE=0` or `LISTEN079_MEDIAGATE=0` turns it off.

## Two trigger words: Claude and Discord

`voice079 start --wake` (the mode kept in `state079`): a line starting with "Claude" goes to Claude Code, typed into its Konsole session over D-Bus whatever window has the focus (`EnableSecuritySensitiveDBusAPI=true` in `konsolerc`; the virtual keyboard, which takes the focus, is only the fallback). A line starting with "Discord" and punctuation ("Discord, on my way.") goes to `discord079` instead: your words, sent as you in the conversation open in the Discord app (the window raised for the line, `desk` types and presses Enter, the focus goes back to where it was), and 079 says where it went from the window title ("Sent to general, LocalLLM."). Only the conversation already open is used; picking one by name is left out because Discord's quick switcher matches loosely. The voiceprint still applies to Discord lines. `DISCORD079_DRY=1 discord079 "Discord, test"` types and clears without sending. Every send is logged in `~/.local/share/speak-079/discord.log`.

A restart of the listener by `follow079` keeps the mode you chose: it had read `voice079 stop`'s saved "off" as wake. A line let in by the 15 s follow-up window no longer renews the window (only a named line or 079 finishing does), so room talk cannot chain through it.

## Quieting the music while you talk (`duck079`, opt-in)

`LISTEN079_DUCK=1`: when the listener's gate opens on speech, every playing desktop stream (not the mic streams into `mix_mic`, not 079's own voice) goes to `DUCK079_PCT` percent (50, about -18 dB) and comes back 1.5 s after you stop, unless you changed its volume meanwhile. It restores by application, because VLC opens a new stream per track at the volume it last had. Off by default: on the energy gate the echo canceller's leftover music kept the gate open and the music ducked; it waits for a speech-model gate.
