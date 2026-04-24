# Moonshine tuning for telephony audio

## The acoustic bottleneck

Moonshine — like Whisper, Parakeet, and every other modern ASR model — was
trained on 16 kHz audio with full frequency content up to 8 kHz. Telephony
G.711 delivers audio at 8 kHz sample rate with a 300–3400 Hz passband. That's
**two compounding mismatches**:

1. Sample rate — closable by upsampling (libswresample does this well).
2. Spectral content — *not* closable by upsampling. Everything above 3.4 kHz
   was never transmitted. No resampler can restore information that isn't
   in the signal.

Published research on this effect (clinical telephony, call-center audio,
ASR benchmarks) documents a "soft floor" around **34 % WER** for
acoustic-only adaptation of 16 kHz-trained models on 8 kHz inputs. That
floor is independent of model size — bigger models do not close it.

Expected v1 WER on G.711 µ-law English telephony with stock Moonshine-base:
~15–25 % depending on source audio quality, background noise, codec chain.
Usable for fraud-signal detection and compliance. Not sufficient for
live-readable captioning of general calls without further work.

## What closes the gap

Three tools, in impact order:

1. **Fine-tuning on G.711-degraded domain data.** Augment a clean telephony
   corpus by simulating µ-law/A-law roundtrip (codec squeezing), add
   domain-specific utterances (call-center greetings, product names, digit
   strings), fine-tune Moonshine for a few epochs. This is the only thing
   that moves WER 5–10 points absolute on real calls. Separate project;
   deferred to v2.
2. **Preprocessing chain before ASR.**
   - Noise suppression — RNNoise or DeepFilterNet before resampling
     (careful: some denoisers hurt ASR accuracy more than the noise does;
     validate on held-out data)
   - 8 → 16 kHz learned bandwidth extension (HiFi-GAN-based super-resolution
     models). Partial recovery of 3.4–8 kHz band. v2+.
   - AGC / loudness normalization. Cheap and safe; add in v1 if time permits.
3. **Bigger Moonshine variant.** `small-streaming` (123 MB) and
   `medium-streaming` (245 MB) give ~2–3 points WER on clean audio, less on
   telephony. Useful but not a substitute for fine-tuning.

## When to use Parakeet or Whisper instead of Moonshine

- **Parakeet-TDT 0.6B:** higher-accuracy English, ~2–3 points better than
  Moonshine-base on clean audio, comparable streaming latency. Larger memory
  footprint (~1.2 GB). Good choice when the deployment has GPU headroom or
  accuracy matters more than concurrent-session count.
- **Canary / Whisper:** multilingual. If you need anything beyond English,
  Moonshine is not the right model. Ship Whisper-large via Sherpa-ONNX as
  the multilingual fallback behind a per-stream language-detect stage.
