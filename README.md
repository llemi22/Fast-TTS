# Fast TTS

Fast TTS is a native Windows low-latency front end for **Qwen3-TTS 1.7B Base** using the MIT-licensed [`ServeurpersoCom/qwentts.cpp`](https://github.com/ServeurpersoCom/qwentts.cpp) runtime.

The current build is designed around these GGUF files:

- `qwen-talker-1.7b-base-Q8_0.gguf`
- `qwen-tokenizer-12hz-Q8_0.gguf`

The models stay outside Git. `RUN_VOICEBOX.ps1` resolves them from `-ModelDir`, `FAST_TTS_MODEL_DIR`, `%USERPROFILE%\Qwen3-TTS\models\Qwen3-TTS-GGUF-Q8_0`, then local `models\` folders. No machine-specific username is required.

## Why this backend

`qwentts.cpp` natively supports the 1.7B model and stateful frame-by-frame streaming. Its streaming path emits the first audio callback after the first generated codec frame, then ramps chunk size while preserving persistent decoder state. Fast TTS sends those chunks directly to a persistent Windows `waveOut` device.

The backend is pinned to commit `51512f129a7419567f4b8abfb06801451789b8f1`. At that pin, cooperative cancellation is polled at every Talker step (roughly one 12.5 Hz frame), and the public ABI defines decoded audio as **24 kHz mono**. The app checks that audio contract during startup warmup.

## User experience

- model remains loaded for the entire app session
- warmup runs once at startup unless disabled
- large native Windows text box
- **Enter** speaks
- **Shift+Enter** inserts a newline
- **Esc** cancels current speech and clears queued speech
- submitted text clears immediately so the next sentence can be typed while audio is playing
- synthesis jobs queue FIFO
- first-audio latency is shown in the status bar
- no temporary WAV files in the normal streaming path
- voice dropdown discovers saved voice profiles
- last selected voice is remembered in `%LOCALAPPDATA%\Fast-TTS\settings.ini`
- cached `.spk` / `.rvq` voices are preferred when available

## Hardened native path

The active native source is:

`voicebox_17\fast_tts_voicebox_hardened.cpp`

`SETUP_AND_BUILD.ps1` copies that file into the pinned backend as `app\tools\fast-tts-voicebox.cpp` before compiling.

Hardening includes:

- callback-free `waveOut` ownership so cancel/voice switching cannot free a buffer while a `WOM_DONE` callback still touches it
- structural JSON parsing for `migration-manifest.json`, including nested values and escaped Unicode
- strict voice-ID validation so manifest entries cannot escape the configured voice directory
- transactional voice switching: a failed new voice leaves the previous voice active
- explicit 24 kHz mono verification during warmup
- warmup synthesis failure is reported as a warning rather than a false fully-healthy `Ready`
- backend cancellation is wired to the pinned runtime's per-Talker-step cancel polling

The older `predict-woo/qwen3-tts.cpp` overlay is not part of the 1.7B build path and is being removed from the repository to avoid two conflicting implementations.

## Build on Windows / NVIDIA CUDA

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
4. copies the hardened native VoiceBox source
5. adds the native Fast TTS target
6. builds the CUDA executable

The backend source is placed under `app\`; model files remain outside the repository.

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

## Voice library

By default the runner checks:

`%USERPROFILE%\Qwen3-TTS\maintained-server-data\voices-q8`

when that folder contains `migration-manifest.json`. Override it with `-VoiceDir`.

The dropdown reads friendly names from the manifest. Voice IDs are treated as file stems only and may contain letters, digits, `_`, and `-`; path separators and traversal characters are rejected.

Reference WAV:

```powershell
.\RUN_VOICEBOX.ps1 -RefWav "C:\voices\reference.wav"
```

ICL cloning with a matching transcript:

```powershell
.\RUN_VOICEBOX.ps1 `
  -RefWav "C:\voices\reference.wav" `
  -RefText "C:\voices\reference.txt"
```

Precomputed `.spk` / `.rvq` latents:

```powershell
.\RUN_VOICEBOX.ps1 `
  -RefSpk "C:\voices\voice.spk" `
  -RefRvq "C:\voices\voice.rvq" `
  -RefText "C:\voices\voice.txt"
```

Using precomputed latents avoids re-running the speaker encoder / reference codec at each app startup.

## Validation

`.github/workflows/validate-windows.yml` performs a Windows CPU compile against the exact pinned backend and verifies that `fast-tts-voicebox.exe` is produced. This proves the native source/CMake integration compiles on Windows; actual CUDA runtime behavior and GPU cancellation latency still need to be exercised on the target NVIDIA machine.

## Backend and licenses

- Fast TTS code: MIT
- `ServeurpersoCom/qwentts.cpp`: MIT
- Qwen3-TTS model and tokenizer: Apache-2.0

The 1.7B GGUF files are intentionally not committed to Git.
