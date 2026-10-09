# Fast TTS

Fast TTS is a native Windows low-latency front end for **Qwen3-TTS 1.7B Base** using the MIT-licensed [`ServeurpersoCom/qwentts.cpp`](https://github.com/ServeurpersoCom/qwentts.cpp) runtime.

The current build is designed around these existing GGUF files:

- `qwen-talker-1.7b-base-Q8_0.gguf`
- `qwen-tokenizer-12hz-Q8_0.gguf`

For the original machine this pair already lives at:

`C:\Users\lukel\Qwen3-TTS\models\Qwen3-TTS-GGUF-Q8_0`

`RUN_VOICEBOX.ps1` auto-detects that location, so the models are **not copied or downloaded again**.

## Why this backend

`qwentts.cpp` natively supports the 1.7B model and stateful frame-by-frame streaming. Its streaming path emits the first audio callback after the first generated codec frame, then ramps chunk size while preserving persistent decoder state. Fast TTS sends those chunks directly to a persistent Windows `waveOut` device.

## User experience

- model remains loaded for the entire app session
- warmup runs once at startup
- large native Windows text box
- **Enter** speaks
- **Shift+Enter** inserts a newline
- **Esc** cancels current speech and clears queued speech
- submitted text clears immediately so the next sentence can be typed while audio is playing
- synthesis jobs queue FIFO
- first-audio latency is shown in the status bar
- no temporary WAV files in the normal streaming path
- optional cached voice cloning from a reference WAV or precomputed `.spk` / `.rvq` files

## Build on the RTX 5080 Windows machine

Requirements:

- Git
- CMake
- Visual Studio with the Desktop C++ workload
- NVIDIA CUDA Toolkit **12.8 or newer recommended for RTX 50-series / Blackwell**

From PowerShell in the Fast-TTS repository:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\SETUP_AND_BUILD.ps1
```

The setup script:

1. clones `ServeurpersoCom/qwentts.cpp`
2. pins it to commit `51512f129a7419567f4b8abfb06801451789b8f1`
3. checks out its pinned GGML submodule
4. adds the native Fast TTS VoiceBox target
5. builds the CUDA executable

The backend source is placed under `app\`; model files remain where they already are.

## Verify the existing model pair

```powershell
.\PREPARE_MODELS.ps1
```

This does **not** download anything. It verifies the existing 1.7B model and tokenizer.

## Run

```powershell
.\RUN_VOICEBOX.ps1
```

The runner automatically checks:

1. `-ModelDir` if supplied
2. `$env:FAST_TTS_MODEL_DIR`
3. `%USERPROFILE%\Qwen3-TTS\models\Qwen3-TTS-GGUF-Q8_0`
4. local `models\` folders

You can override paths explicitly:

```powershell
.\RUN_VOICEBOX.ps1 `
  -Model "D:\models\qwen-talker-1.7b-base-Q8_0.gguf" `
  -Codec "D:\models\qwen-tokenizer-12hz-Q8_0.gguf"
```

## Voice cloning

Reference WAV, extracted once when the app starts:

```powershell
.\RUN_VOICEBOX.ps1 -RefWav "C:\voices\reference.wav"
```

ICL cloning with a matching transcript:

```powershell
.\RUN_VOICEBOX.ps1 `
  -RefWav "C:\voices\reference.wav" `
  -RefText "C:\voices\reference.txt"
```

Precomputed `.spk` / `.rvq` latents are also accepted:

```powershell
.\RUN_VOICEBOX.ps1 `
  -RefSpk "C:\voices\voice.spk" `
  -RefRvq "C:\voices\voice.rvq" `
  -RefText "C:\voices\voice.txt"
```

Using precomputed latents avoids re-running the speaker encoder / reference codec at each app startup.

## Backend and licenses

- Fast TTS code: MIT
- `ServeurpersoCom/qwentts.cpp`: MIT
- Qwen3-TTS model and tokenizer: Apache-2.0

The 1.7B GGUF files are intentionally not committed to Git because they already exist locally and the pair is larger than a normal single GitHub source artifact.
