# Live microphone -> cloned voice

Fast TTS can run as a near-real-time speech re-synthesis pipeline:

`microphone -> faster-whisper Turbo -> stable word commit -> Qwen3-TTS 1.7B clone -> streaming waveOut`

The native Qwen3-TTS VoiceBox remains the synthesis process. The live bridge captures 16 kHz microphone audio, repeatedly decodes only the still-uncommitted audio window, waits for Whisper to agree on words across passes, keeps the newest word unstable, and sends short committed phrases into the existing native VoiceBox queue.

## Setup

Build Fast TTS normally first:

```powershell
.\SETUP_AND_BUILD.ps1
```

Install the live speech Python environment once:

```powershell
.\SETUP_LIVE_SPEECH.ps1
```

The first live run may download the selected faster-whisper CTranslate2 model. GPU mode requires the CUDA/cuDNN runtime expected by the installed CTranslate2 build.

## Run

```powershell
.\RUN_LIVE_SPEECH.ps1
```

The default profile is tuned for the target RTX 5080 Laptop GPU / 16 GB VRAM system and assumes headphone use:

- Whisper model: `turbo`
- device: CUDA
- compute type: `float16`
- 20 ms microphone blocks with PortAudio low-latency mode
- 240 ms target between ASR decode starts
- first decode after about 360 ms of captured speech
- 320 ms speech endpoint
- 160 ms microphone pre-roll
- one unstable tail word
- TTS chunks of roughly 2-5 committed words
- 380 ms maximum chunk wait after the minimum chunk size is available
- 6 second hard cap on uncommitted ASR audio
- 700 ms Whisper warmup before the microphone loop begins

The ASR cadence is measured from the **start** of the previous decode rather than its completion. That prevents Whisper inference time from being added on top of the configured step interval. The console also prints the measured duration of each normal and endpoint decode so the profile can be tuned against real hardware behavior.

If `fast-tts-voicebox.exe` is not already running, the live runner starts it detached with the same model/voice resolution used by `RUN_VOICEBOX.ps1`. If VoiceBox is already running, its resident model and currently selected cloned voice are reused.

Because the intended setup always uses headphones, this profile does not spend latency or complexity on acoustic echo cancellation. If speaker playback is used later, echo cancellation should be added before relying on the same aggressive VAD/endpoint settings.

## Choose a microphone

```powershell
.\RUN_LIVE_SPEECH.ps1 -ListDevices
.\RUN_LIVE_SPEECH.ps1 -InputDevice 3
```

A device name may also be supplied instead of a numeric index.

## Conservative fallback

If the aggressive profile causes transcript revisions, short/choppy TTS phrases, or GPU contention, use:

```powershell
.\RUN_LIVE_SPEECH.ps1 `
  -ComputeType int8_float16 `
  -StepMs 300 `
  -MinDecodeMs 440 `
  -EndpointMs 420 `
  -TailWords 2 `
  -MinChunkWords 3 `
  -MaxChunkWords 7 `
  -MaxChunkDelayMs 600
```

That trades some latency for more transcription stability and lower Whisper VRAM/compute pressure.

## More aggressive experiment

Once the default profile is proven stable on the target machine, this is the next sensible latency experiment:

```powershell
.\RUN_LIVE_SPEECH.ps1 `
  -StepMs 200 `
  -MinDecodeMs 300 `
  -EndpointMs 280 `
  -PreRollMs 140 `
  -TailWords 1 `
  -MinChunkWords 2 `
  -MaxChunkWords 4 `
  -MaxChunkDelayMs 300
```

Do not drop `TailWords` to zero by default. The unstable tail exists to keep Whisper from causing the TTS engine to speak a word that a later ASR pass would revise.

If the room is noisy, raise `-SpeechThresholdDb` toward `-38`. If quiet speech is being missed, lower it toward `-48`.

## Voice cloning

The live bridge does not load a second Qwen model. Voice cloning stays in the existing native VoiceBox process, including cached `.spk` / `.rvq` voices. Select a saved voice in the VoiceBox dropdown before or during live mode, or pass the same `-RefWav`, `-RefText`, `-RefSpk`, and `-RefRvq` options accepted by `RUN_VOICEBOX.ps1` to `RUN_LIVE_SPEECH.ps1` when it launches VoiceBox.

## Recognition-only test

To exercise Whisper and microphone capture without sending phrases to Qwen:

```powershell
.\RUN_LIVE_SPEECH.ps1 -DryRun
```

## How the streaming commit works

Whisper is not treated as an append-only transcript. Each decode is compared with the previous hypothesis. Only the common stable prefix is committed, while the newest tail word is held back because it is the most likely to be revised. After stable words are committed, their audio is trimmed from the ASR buffer so inference cost does not grow with conversation length. The committed words are accumulated into short phrase-sized chunks and submitted to Qwen3-TTS while microphone capture and ASR continue.

The microphone path keeps 20 ms blocks as separate arrays and only flattens them when Whisper actually needs an inference buffer. This avoids copying an ever-growing NumPy array on every audio callback.

The cloned output deliberately trails the speaker slightly rather than waiting for a full sentence or risking speaking every unstable partial hypothesis.
