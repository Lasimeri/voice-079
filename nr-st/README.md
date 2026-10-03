# nr-st: DeepFilterNet3 LADSPA plugin in C, weights from safetensors

A drop-in for `~/tts079/nr/libdeep_filter_ladspa.so`, written in C. It keeps the
label `deep_filter_mono` and the same 8 ports in the same order, with the same
names, bounds and default hints. It runs DeepFilterNet3 streaming inference with
weights read by hand from `model.safetensors`. Not deployed: nr079 is unchanged.

## Files

| file | what |
|---|---|
| `dfn3.c`, `dfn3.h` | safetensors loader, STFT/ERB/complex features, network, streaming loop |
| `deep_filter_st_ladspa.c` | LADSPA wrapper -> `libdeep_filter_st_ladspa.so` |
| `ladspa-abi.h` | LADSPA 1.1 ABI declarations (no system ladspa.h here) |
| `ladspa-run.c` | offline LADSPA host: raw f32 in, raw f32 out, timing, passes, pacing |
| `cmp-audio.c` | agreement: offset search, SNR, max diff, segments, offset tracking |
| `build.sh` | `nice -n 19 gcc -O2 ...` for all three; no -ffast-math |
| `model.safetensors`, `config.json` | the weights and their config (copies, see below) |

Build: `~/tts079/nr-st/build.sh`

Plugin environment:
- `NRST_MODEL`: weights path, default `~/tts079/nr-st/model.safetensors`.
- `NRST_CONFIG`: default `config.json` next to the model. If present, it is
  checked against the built-in DFN3 dimensions; if absent, only the tensor shapes
  are checked.

## Weights provenance

| | |
|---|---|
| file | `v3/model.safetensors` (8,682,709 bytes, 118 tensors, F32, 2.17M params) |
| sha256 | `fca0af2f25cad49d74fc9ac5f9155813416e4b350ec344bb433b4a48a9a76d38` |
| source | https://huggingface.co/mlx-community/DeepFilterNet-mlx, revision `220d5dfb7266352272d74c2a7d2025c59e07b391` (lastModified 2026-03-11) |
| same bytes | https://huggingface.co/iky1e/DeepFilterNet3-MLX `model.safetensors`, revision `2439328cfebafb28099d9feff6890e29dbae2241` (lastModified 2026-03-09) |
| config.json | sha256 `cc96fe1bf688e99ed9d31c26ad881d8e0799394862eb35e0be2513afcb3cf7ca` (identical in both repos) |
| license | MIT (repo cards), original DeepFilterNet MIT/Apache-2.0 |
| fetched | 2026-10-03 with curl from `resolve/<revision>/`; the `hf` CLI was not used (it is Python) |

Layout: PyTorch, not MLX channel-last. The converter (`convert_deepfilternet.py`,
read as text, not run) saves the `torch.load` state_dict as-is. The header shapes
agree: Conv2d `[out, in/g, kt, kf]`, ConvTranspose2d `[in, out/g, kt, kf]`,
GroupedLinear `[g, in/g, out/g]`, GRU `[3H, in]` with gates r,z,n.

Checked against the original checkpoint, `models/DeepFilterNet3.zip`
(`model_120.ckpt.best`) in Rikorose/DeepFilterNet v0.5.6, by sha256 of the raw
storages:
- 95 tensors are byte-identical to a whole storage blob.
- 20 (all GRU parameters) are byte-identical slices of the flattened GRU storages.
- 3 (`df_dec.df_convp.1.weight`, `erb_dec.conv0_out.0.weight`, `mask.erb_inv_fb`)
  hold the same values in a different storage order, because those tensors are
  non-contiguous in the checkpoint.

The GroupedLinear and GRU bytes also appear verbatim in
`DeepFilterNet3_onnx.tar.gz`, and in none of the `DeepFilterNet3_ll_onnx.tar.gz`
files.

Values not in config.json, taken from the DFN3 `config.ini` (v0.5.6 tarball):
`norm_tau = 1` (so alpha = 0.99 after libDF's rounding) and
`min_nb_erb_freqs = 2`. The ERB band widths computed from them equal the widths
implied by the `erb_fb` tensor; dfn3_new checks this. BatchNorm eps is 1e-5
(PyTorch default). `df_pathway_kernel_size_t = 5` comes from the
`df_convp` shape.

## The plugin in use today is a different model

`~/tts079/nr/libdeep_filter_ladspa.so` is byte-identical to the v0.5.6 release
asset `libdeep_filter_ladspa-0.5.6-x86_64-unknown-linux-gnu.so` (sha256
`2ca3205c...2236`). Its crate's default feature is `default-model-ll`. It embeds
`DeepFilterNet3_ll_onnx.tar.gz` (36,359,660 bytes, byte-identical at file
offset 14619893), not `DeepFilterNet3_onnx.tar.gz`.

DFN3_ll differs from DFN3 in architecture and in its trained weights:

| | DFN3_ll | DFN3 |
|---|---|---|
| emb_hidden_dim | 512 | 256 |
| df_hidden_dim | 512 | 256 |
| df_num_layers | 3 | 2 |
| conv_kernel | 2,3 | 1,3 |
| enc_linear_groups | 16 | 32 |
| convt_depthwise | true | false |
| df/conv lookahead | 0/0 | 2/2 |

No safetensors of DFN3_ll is published. Same-weights parity against the live
plugin is therefore impossible by construction. Verification is split into three legs.

## Verification (2026-10-03, nice 19, load average 8 to 25)

Test input: 69.2 s, raw f32 mono 48 kHz, file
`~/.cache/voice-079/nr-st/test/input.f32`.

| span | content |
|---|---|
| 0-2 s | exact zeros |
| 2-32 s | podcast speech (Off Topic ep. 102) + 0.3 x music (Odd Chap) |
| 32-37 s | music |
| 37-38 s | zeros |
| 38-46 s | speech |
| 46-69.2 s | a read-only `parec` capture of `voice079_mic_all` |

Peak -2.8 dBFS, rms -21.5 dBFS.

References: the v0.5.6 release CLI `deep-filter-0.5.6-x86_64-unknown-linux-musl`
(sha256 `70775e251eee44c0f2451a1e833326cf8bcbbe304d3e7cd12851e6fce72ef7da`; tract,
same `DfTract::process`) on `input.wav` (float WAV, `ffmpeg -f f32le -ar 48000
-ac 1 -i input.f32 -c:a pcm_f32le input.wav`). Without `-m` it runs its built-in
DFN3 (`DeepFilterNet3_onnx.tar.gz`, byte-identical inside the binary); with
`-m DeepFilterNet3_ll_onnx.tar.gz` it runs the live model. Its output is int16
via `(s*32767) as i16`; cmp-audio `-q` applies the same truncation to the C output.
```
cd ~/.cache/voice-079/nr-st/test
nice -n 19 ../ref/deep-filter-0.5.6-x86_64-unknown-linux-musl -a 100 -o cli-dfn3-a100 input.wav
nice -n 19 ../ref/deep-filter-0.5.6-x86_64-unknown-linux-musl -m ../DeepFilterNet/models/DeepFilterNet3_ll_onnx.tar.gz -a 100 -o cli-dfn3ll-a100 input.wav
ffmpeg -i cli-dfn3-a100/input.wav -f s16le -c:a pcm_s16le cli-dfn3-a100.s16    # same for -a 20, -a 0, ll
```

1. **Same weights: this plugin vs tract DFN3** (atten 100, thresholds -15/35/35)

   | atten | offset | int16 identical | 1 LSB | >=2 LSB | SNR (int16) |
   |---|---|---|---|---|---|
   | 100 dB | 480 (hop buffer, found by search) | 99.961% | 0.039% | 0 | 94.41 dB |
   | 20 dB | 480 | 99.964% | 0.036% | 0 | 95.32 dB |
   | 0 dB | 480 | 100% | 0 | 0 | exact passthrough |

   - At 100 dB the 5 s segments run 90.7-96.9 dB. The quiet mic segments
     (-72 dBFS, a few LSB rms) score 63-67 dB, from the same 1-LSB flips.
   - Bound: every sample lands within 1 LSB after the same truncation, so the
     float difference is under 2 LSB (6.1e-5, -84 dBFS) at every sample; the
     zero bin of a truncating quantizer is 2 LSB wide.
   - Estimate, not a measurement: assuming the float difference is independent
     of where a sample sits inside its quantization bin, a 0.039% flip rate
     implies mean |diff| about 0.00039 / 32767 = 1.2e-8.

2. **Host check: Rust plugin through ladspa-run vs tract DFN3_ll**

   The Rust plugin processes on a worker thread and adds 480 samples of latency
   on every run() that measures RTF >= 1. Run unpaced offline, it climbed to
   1 s latency and panicked by design (`lib.rs:444`). With `-R` (real-time
   pacing, as PipeWire) at block 480:
   - its latency stepped 480 -> 960 -> 1440 -> 1920 -> 2400 -> 3360 (6 underruns);
   - it then fell 1 sample every 10.0 s (4 "decreases"). The decrease path pops
     `take(frame_size)` channels, i.e. 1 sample, while bookkeeping 480.

   Agreement with tract DFN3_ll:
   - jump-free stretch 2-9 s: 68.1 dB, 82.5% int16-identical, max 19 LSB;
   - 0.25 s windows not adjacent to an offset change: 62.8 dB (block 480),
     56.7 dB (256), 56.9 dB (1024).

   Two runs of the same Rust binary are bit-identical in the median window. The
   residual vs the CLI is between two Rust/tract builds; it is not attributed
   further.

   From the code, and so true live as well: each underrun adds 480 samples of
   latency, and each "decrease" removes only 1 sample (it pops
   `take(frame_size)` channels, not samples).

   Measured offline only, at nice 19, under load 8 to 25, paced with `-R`:
   latency reached 7671 samples (160 ms) at block 256 and 5758 (120 ms) at
   block 1024 within 69 s. The live filter-chain loads module-rt, so these
   figures are not a statement about live behaviour.

3. **Required comparison: old plugin (ref) vs this plugin, atten 100**

   Offset tracked per 0.25 s window; the start offset is +960, DFN3's 2-frame
   lookahead.

   | block | SNR all windows | SNR excluding offset changes | per-window median | max abs diff |
   |---|---|---|---|---|
   | 256 | 6.64 dB | 6.67 dB | 2.37 dB | 0.533 |
   | 480 | 6.74 dB | 6.82 dB | 2.69 dB | 0.533 |
   | 1024 | 6.12 dB | 6.62 dB | 2.33 dB | 0.533 |

   Model-only, with no wrapper (this plugin vs tract DFN3_ll, fixed offset
   1440 = 480 + 960): 6.77 dB, correlation 0.899. The disagreement is the
   network (DFN3 vs DFN3_ll), not alignment or the wrapper.

   Per 5 s segment, block 480. The local offset is searched within +-3000 of
   +960, so it follows the old plugin's latency path:

   | start s | ref dBFS | local offset | old vs new SNR dB | model-only SNR dB |
   |---|---|---|---|---|
   | 0 | -32.4 | 960 | 11.48 | 11.48 |
   | 5 | -26.5 | 960 | 6.77 (jump at 9.25 s) | 11.31 |
   | 10 | -24.1 | 480 | 8.15 | 8.29 |
   | 15 | -24.4 | 0 | 4.39 (jump at 15.0 s) | 4.30 |
   | 20 | -24.9 | -960 | 3.23 (jumps at 21.0-21.5 s) | 4.05 |
   | 25 | -23.4 | -1920 | 4.41 (jumps at 25.25-25.75 s) | 7.11 |
   | 30 | -29.9 | -1920 | 8.71 | 8.71 |
   | 35 | -25.8 | -1919 | 8.07 | 8.25 |
   | 40 | -27.4 | -1919 | 9.32 | 9.11 |
   | 45 | -34.6 | -1918 | 17.53 | 21.95 |
   | 50 | -64.2 | -1918 | 2.99 | 3.03 |
   | 55 | -64.0 | -1917 | 1.85 | 1.88 |
   | 60 | -60.5 | -1917 | 2.21 | 2.26 |
   | 65 | -60.0 | -1916 | 2.22 | 2.22 |

   Segments with no jump inside agree with the model-only figure to within
   0.2 dB.

   By content (model-only, fixed offset), with each output's RMS level
   (delay-corrected):

   | span | SNR dB | input dBFS | DFN3_ll out dBFS | DFN3 (this) out dBFS |
   |---|---|---|---|---|
   | speech+music 2-32 s | 6.39 | -18.6 | -24.9 | -27.5 |
   | music 32-37 s | -0.98 | -19.6 | -49.6 | -51.9 |
   | speech 38-46 s | 8.86 | -24.6 | -25.3 | -26.6 |
   | mic capture 46-69.2 s | 4.22 | -54.7 | -62.8 | -73.2 |

   DFN3 suppresses more than DFN3_ll on every span: about 10 dB more on the
   room capture, 2.3 dB more on music. It also leaves clean speech 1.3 dB
   lower.

Determinism and lifecycle:
- Output is byte-identical for blocks 1, 7, 256, 333, 480, 1024, random
  1..2048 and random 1..4096, and in-place (in == out).
- 3 passes with deactivate+activate, and 2 passes with a new instance, are each
  byte-identical to the first pass.
- 44.1 kHz, a missing model, a truncated model, and a config that contradicts
  the weights each make instantiate() return NULL, with the reason on stderr.

Timing (thread CPU per 10 ms hop, block 480, single thread, -O2, two runs):

| | mean | p50 | p99 | max |
|---|---|---|---|---|
| all hops | 1418-1425 us | 1234-1259 us | 3113-3270 us | 4913-6011 us |
| non-silent hops | 1483 us | 1292 us | 3125 us | 4913 us |

- 0 of 13,844 hops exceeded 10 ms.
- tract DFN3 (CLI), for scale: user CPU 7.32 s for 6922 hops, about 1060 us
  per hop, including model setup. Wall RTF 0.125, about 1250 us per hop.
- The Rust plugin (DFN3_ll), process CPU: 5612-5858 us per hop.

Commands:
```
cd ~/.cache/voice-079/nr-st/test
S=~/tts079/nr-st
nice -n 19 $S/ladspa-run -b 480 -c 'Attenuation Limit (dB)=100' -t new.times $S/libdeep_filter_st_ladspa.so deep_filter_mono input.f32 new.f32
nice -n 19 $S/cmp-audio -r s16 -q -L 2000 cli-dfn3-a100.s16 new.f32
RUST_LOG=info nice -n 19 $S/ladspa-run -R -b 480 -c 'Attenuation Limit (dB)=100' ~/tts079/nr/libdeep_filter_ladspa.so deep_filter_mono input.f32 old.f32
nice -n 19 $S/cmp-audio -o 960 -w 0.25 -S 600 old.f32 new.f32
nice -n 19 $S/cmp-audio -o 960 -s 5 -S 3000 old.f32 new.f32
nice -n 19 $S/cmp-audio -r s16 -o 1440 -s 5 cli-dfn3ll-a100.s16 new.f32
```

## Differences from the Rust plugin

- Model: DFN3 (from safetensors), not DFN3_ll. It has 2 frames (20 ms) more
  algorithmic lookahead. Total input-to-output delay is 1920 samples (40 ms):
  480 hop buffer + 480 STFT + 960 lookahead. The Rust plugin starts at 960
  (20 ms: 480 + 480) and grows by 480 per underrun.
- Synchronous: run() fills 480-sample hops and processes them in place. Latency
  is a fixed 480 samples of buffering and never grows on underruns. No worker
  thread, no allocation in run(). `Min Processing Buffer (frames)` is accepted
  and ignored.
- activate() resets all state; the Rust plugin keeps state across
  deactivate/activate.
- Mono only, UniqueID 7843797 (the Rust plugin's is 7843795). PipeWire selects
  by label, so this does not matter to nr079.
- Controls apply at the next run() call. The Rust plugin applies one queued
  change per worker-loop pass.

## Switching nr079 (not done)

One edit, line 24 of `~/tts079/nr079`, label and ports unchanged. The line becomes:
```
        node="{ type = ladspa name = nr plugin = \"$here/nr-st/libdeep_filter_st_ladspa.so\" label = deep_filter_mono
```
Cost of the switch: 20 ms more delay (40 ms vs 20 ms at start), and different
suppression (see the by-content table). Gain: about 4x less CPU (1.42 ms vs
5.6-5.9 ms per hop) and a fixed latency.
Side note: the nr079 header comment says the `NR079_ATTEN` default is 60; the
code uses 100.
