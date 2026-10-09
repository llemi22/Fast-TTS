# Fast TTS

Fast TTS is a low-latency native Windows Qwen3-TTS VoiceBox focused on minimizing **Enter-to-first-audio** latency.

## What the full package contains

The GitHub Release produced by this repository contains one file:

`Fast_TTS_FULL_WITH_MODELS.zip`

That package includes:

- the pinned `predict-woo/qwen3-tts.cpp` source tree
- the pinned GGML source tree
- Fast TTS streaming / cancellation / Windows playback modifications
- Windows setup, build, and run scripts
- `qwen3-tts-0.6b-q8_0.gguf`
- `qwen3-tts-tokenizer-f16.gguf`

## Interaction

- **Enter** — speak
- **Shift+Enter** — newline
- **Esc** — cancel generation and playback
- model remains loaded between utterances
- codec frames are streamed to the vocoder before the whole utterance finishes
- overlapping vocoder windows reduce chunk-boundary artifacts
- multiple Windows audio buffers are queued ahead for smoother playback

## Source bundle

`Fast_TTS_SOURCE_PACKAGE.zip` is the small editable Fast TTS source/overlay package. GitHub Actions expands it, integrates the exact pinned upstream runtime and GGML dependency, downloads the compatible Qwen3-TTS model files, and publishes the all-in-one Release ZIP.

## Licensing

- `predict-woo/qwen3-tts.cpp`: MIT
- Fast TTS modifications: MIT
- Qwen3-TTS compatible model weights: Apache-2.0

The upstream runtime is pinned to commit `b3ba14077cf1b3e11b86e5f84aa9184605c89b28` for reproducibility.
