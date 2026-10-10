# Live microphone -> cloned voice

Fast TTS can now run as a near-real-time speech re-synthesis pipeline:

`microphone -> faster-whisper Turbo -> stable word commit -> Qwen3-TTS 1.7B clone -> streaming waveOut`

The native Qwen3-TTS VoiceBox remains the synthesis process. The live bridge captures 16 kHz microphone audio, repeatedly decodes only the still-uncommitted audio window, waits for Whisper to agree on words across passes, keeps the last few words unstable, and sends short committed phrases into the existing native VoiceBox queue.

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

By default this uses:

- Whisper model: `turbo`
- device: CUDA
- compute type: `int8_float16`
- 320 ms ASR update target
- 450 ms speech endpoint
- two unstable tail words
- TTS chunks of roughly 3-7 committed words

If `fast-tts-voicebox.exe` is not already running, the live runner starts it detached with the same model/voice resolution used by `RUN_VOICEBOX.ps1`. If VoiceBox is already running, its resident model and currently selected cloned voice are reused.

Use headphones for the first version. Speaker output can otherwise feed the cloned voice back into the microphone because acoustic echo cancellation is not yet part of this path.

## Choose a microphone

```powershell
.\RUN_LIVE_SPEECH.ps1 -ListDevices
.\RUN_LIVE_SPEECH.ps1 -InputDevice 3
```

A device name may also be supplied instead of a numeric index.

## Latency tuning

Lower values react faster but can increase correction risk or produce shorter TTS phrases:

```powershell
.\RUN_LIVE_SPEECH.ps1 `
  -StepMs 260 `
  -EndpointMs 380 `
  -TailWords 1 `
  -MinChunkWords 2 `
  -MaxChunkWords 6 `
  -MaxChunkDelayMs 500
```

For more stable wording, increase `TailWords` to 2-3 and `StepMs` to 350-450.

If the room is noisy, raise `-SpeechThresholdDb` toward `-38`. If quiet speech is being missed, lower it toward `-48`.

## Voice cloning

The live bridge does not load a second Qwen model. Voice cloning stays in the existing native VoiceBox process, including cached `.spk` / `.rvq` voices. Select a saved voice in the VoiceBox dropdown before or during live mode, or pass the same `-RefWav`, `-RefText`, `-RefSpk`, and `-RefRvq` options accepted by `RUN_VOICEBOX.ps1` to `RUN_LIVE_SPEECH.ps1` when it launches VoiceBox.

## Recognition-only test

To exercise Whisper and microphone capture without sending phrases to Qwen:

```powershell
.\RUN_LIVE_SPEECH.ps1 -DryRun
```

## How the streaming commit works

Whisper is not treated as an append-only transcript. Each decode is compared with the previous hypothesis. Only the common stable prefix is committed, while the newest tail words are held back because they are the most likely to be revised. After stable words are committed, their audio is trimmed from the ASR buffer so inference cost does not grow with conversation length. The committed words are accumulated into short phrase-sized chunks and submitted to Qwen3-TTS while microphone capture and ASR continue.

This means the cloned output deliberately trails the speaker slightly rather than waiting for a full sentence or risking speaking every unstable partial hypothesis.
