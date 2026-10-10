from __future__ import annotations

import argparse
import ctypes
import math
import os
import queue
import sys
import time
from dataclasses import dataclass
from typing import Iterable, Sequence

WM_SETTEXT = 0x000C
WM_COMMAND = 0x0111
VOICEBOX_CLASS = "FastTTSVoiceBox17"
ID_EDIT = 1001
ID_SPEAK = 1002
ID_STOP = 1003

if sys.platform == "win32":
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    user32.FindWindowW.restype = ctypes.c_void_p
    user32.GetDlgItem.argtypes = [ctypes.c_void_p, ctypes.c_int]
    user32.GetDlgItem.restype = ctypes.c_void_p
    user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    user32.SendMessageW.restype = ctypes.c_ssize_t
else:
    user32 = None


def _norm_word(word: str) -> str:
    return "".join(ch for ch in word.casefold() if ch.isalnum() or ch in ("'", "’"))


def _token_text(word: str) -> str:
    return word.strip()


def _join_words(words: Sequence[str]) -> str:
    text = " ".join(w.strip() for w in words if w.strip())
    for mark in (".", ",", "!", "?", ";", ":"):
        text = text.replace(" " + mark, mark)
    text = text.replace(" n't", "n't")
    return text.strip()


def _longest_common_prefix(a: Sequence[str], b: Sequence[str]) -> int:
    n = min(len(a), len(b))
    i = 0
    while i < n and _norm_word(a[i]) == _norm_word(b[i]) and _norm_word(a[i]):
        i += 1
    return i


@dataclass
class Word:
    text: str
    start: float
    end: float


class VoiceBoxClient:
    def __init__(self, dry_run: bool = False) -> None:
        self.dry_run = dry_run
        self.hwnd: int | None = None
        self.edit: int | None = None

    def connect(self, timeout: float) -> None:
        if self.dry_run:
            return
        if sys.platform != "win32" or user32 is None:
            raise RuntimeError("Live TTS injection currently requires Windows.")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            hwnd = int(user32.FindWindowW(VOICEBOX_CLASS, None) or 0)
            if hwnd:
                edit = int(user32.GetDlgItem(hwnd, ID_EDIT) or 0)
                if edit:
                    self.hwnd = hwnd
                    self.edit = edit
                    return
            time.sleep(0.25)
        raise RuntimeError(
            "Fast TTS VoiceBox window was not found. Start RUN_VOICEBOX.ps1 first "
            "or use RUN_LIVE_SPEECH.ps1 so it can launch VoiceBox automatically."
        )

    def _require(self) -> tuple[int, int]:
        if self.hwnd and self.edit:
            return self.hwnd, self.edit
        self.connect(2.0)
        assert self.hwnd and self.edit
        return self.hwnd, self.edit

    def speak(self, text: str) -> None:
        text = text.strip()
        if not text:
            return
        if self.dry_run:
            print(f"[tts] {text}", flush=True)
            return
        hwnd, edit = self._require()
        buf = ctypes.create_unicode_buffer(text)
        user32.SendMessageW(edit, WM_SETTEXT, 0, ctypes.cast(buf, ctypes.c_void_p).value)
        user32.SendMessageW(hwnd, WM_COMMAND, ID_SPEAK, edit)

    def cancel(self) -> None:
        if self.dry_run:
            return
        if not self.hwnd:
            return
        user32.SendMessageW(self.hwnd, WM_COMMAND, ID_STOP, 0)


class Chunker:
    def __init__(
        self,
        client: VoiceBoxClient,
        min_words: int,
        max_words: int,
        max_delay_ms: int,
    ) -> None:
        self.client = client
        self.min_words = max(1, min_words)
        self.max_words = max(self.min_words, max_words)
        self.max_delay = max(0.05, max_delay_ms / 1000.0)
        self.pending: list[str] = []
        self.last_emit = time.monotonic()
        self.history: list[str] = []

    def add(self, words: Iterable[str]) -> None:
        for word in words:
            word = word.strip()
            if word:
                self.pending.append(word)
                self.history.append(word)
        if len(self.history) > 40:
            self.history = self.history[-40:]

    def prompt(self) -> str | None:
        if not self.history:
            return None
        return _join_words(self.history[-24:])

    @staticmethod
    def _ends_phrase(word: str) -> bool:
        return word.endswith((".", "!", "?", ";", ":"))

    def maybe_emit(self, force: bool = False) -> None:
        while self.pending:
            now = time.monotonic()
            elapsed = now - self.last_emit
            if not force and len(self.pending) < self.min_words:
                return

            limit = min(self.max_words, len(self.pending))
            cut = 0

            for i in range(self.min_words, limit + 1):
                if self._ends_phrase(self.pending[i - 1]):
                    cut = i

            if cut == 0:
                if len(self.pending) >= self.max_words:
                    cut = self.max_words
                elif force:
                    cut = limit
                elif elapsed >= self.max_delay and len(self.pending) >= self.min_words:
                    cut = limit
                else:
                    return

            phrase = _join_words(self.pending[:cut])
            del self.pending[:cut]
            if phrase:
                self.client.speak(phrase)
                self.last_emit = time.monotonic()

    def flush(self) -> None:
        self.maybe_emit(force=True)


class StableWordCommitter:
    def __init__(self, tail_words: int, chunker: Chunker) -> None:
        self.tail_words = max(0, tail_words)
        self.chunker = chunker
        self.previous: list[Word] = []

    def observe(self, current: Sequence[Word]) -> float:
        prev_text = [w.text for w in self.previous]
        cur_text = [w.text for w in current]
        lcp = _longest_common_prefix(prev_text, cur_text)
        stable_count = max(0, lcp - self.tail_words)
        if stable_count <= 0:
            self.previous = list(current)
            return 0.0

        stable = list(current[:stable_count])
        self.chunker.add(w.text for w in stable)
        trim_to = max(0.0, stable[-1].end)
        self.previous = [
            Word(w.text, max(0.0, w.start - trim_to), max(0.0, w.end - trim_to))
            for w in current[stable_count:]
        ]
        return trim_to

    def finalize(self, current: Sequence[Word]) -> None:
        chosen = list(current) if current else list(self.previous)
        if chosen:
            self.chunker.add(w.text for w in chosen)
        self.previous = []
        self.chunker.flush()

    def reset(self) -> None:
        self.previous = []


def _transcribe_words(model, audio, language: str | None, prompt: str | None) -> list[Word]:
    kwargs = dict(
        beam_size=1,
        condition_on_previous_text=False,
        vad_filter=False,
        word_timestamps=True,
        temperature=0.0,
    )
    if language:
        kwargs["language"] = language
    if prompt:
        kwargs["initial_prompt"] = prompt
    segments, _ = model.transcribe(audio, **kwargs)
    words: list[Word] = []
    for segment in segments:
        seg_words = getattr(segment, "words", None)
        if seg_words:
            for item in seg_words:
                text = _token_text(getattr(item, "word", ""))
                if not text:
                    continue
                start = float(getattr(item, "start", 0.0) or 0.0)
                end = float(getattr(item, "end", start) or start)
                words.append(Word(text, start, end))
        else:
            text = str(getattr(segment, "text", "") or "").strip()
            parts = text.split()
            if not parts:
                continue
            start = float(getattr(segment, "start", 0.0) or 0.0)
            end = float(getattr(segment, "end", start) or start)
            span = max(0.001, end - start)
            for i, part in enumerate(parts):
                a = start + span * i / len(parts)
                b = start + span * (i + 1) / len(parts)
                words.append(Word(part, a, b))
    return words


def _dbfs(block) -> float:
    import numpy as np

    rms = float(np.sqrt(np.mean(np.square(block), dtype=np.float64) + 1e-12))
    return 20.0 * math.log10(max(rms, 1e-8))


def _parse_device(value: str | None):
    if value is None:
        return None
    stripped = value.strip()
    if stripped.isdigit():
        return int(stripped)
    return stripped


def run_live(args: argparse.Namespace) -> int:
    try:
        import numpy as np
        import sounddevice as sd
        from faster_whisper import WhisperModel
    except Exception as exc:
        raise RuntimeError(
            "Live speech dependencies are missing. Run SETUP_LIVE_SPEECH.ps1 first."
        ) from exc

    if args.list_devices:
        print(sd.query_devices())
        return 0

    client = VoiceBoxClient(dry_run=args.dry_run)
    if not args.dry_run:
        print("Waiting for Fast TTS VoiceBox...", flush=True)
        client.connect(args.voicebox_wait)
        print("Connected to Fast TTS VoiceBox.", flush=True)

    print(
        f"Loading Whisper '{args.model}' on {args.device} ({args.compute_type})...",
        flush=True,
    )
    try:
        model = WhisperModel(args.model, device=args.device, compute_type=args.compute_type)
    except Exception as exc:
        raise RuntimeError(
            "Whisper failed to load. For CUDA, faster-whisper currently needs the "
            "CTranslate2 CUDA/cuDNN runtime expected by its installed version."
        ) from exc

    chunker = Chunker(
        client=client,
        min_words=args.min_chunk_words,
        max_words=args.max_chunk_words,
        max_delay_ms=args.max_chunk_delay_ms,
    )
    committer = StableWordCommitter(args.tail_words, chunker)

    sample_rate = args.sample_rate
    block_ms = args.block_ms
    blocksize = max(1, int(sample_rate * block_ms / 1000))
    pre_roll_blocks = max(1, int(math.ceil(args.pre_roll_ms / block_ms)))
    input_device = _parse_device(args.input_device)

    audio_q: queue.Queue = queue.Queue(maxsize=max(64, int(5000 / block_ms)))
    pre_roll: list = []
    active = False
    silence_ms = 0
    audio = np.zeros((0,), dtype=np.float32)
    last_decode = 0.0

    def callback(indata, frames, time_info, status) -> None:
        del frames, time_info
        if status:
            print(f"[audio] {status}", file=sys.stderr, flush=True)
        block = np.asarray(indata[:, 0], dtype=np.float32).copy()
        try:
            audio_q.put_nowait(block)
        except queue.Full:
            try:
                audio_q.get_nowait()
            except queue.Empty:
                pass
            try:
                audio_q.put_nowait(block)
            except queue.Full:
                pass

    print(
        "Live mode ready. Speak into the microphone. "
        "Use headphones to keep the cloned output out of the mic. Ctrl+C stops.",
        flush=True,
    )

    with sd.InputStream(
        samplerate=sample_rate,
        blocksize=blocksize,
        channels=1,
        dtype="float32",
        device=input_device,
        callback=callback,
    ):
        try:
            while True:
                block = audio_q.get(timeout=1.0)
                speech = _dbfs(block) >= args.speech_threshold_db
                now = time.monotonic()

                if not active:
                    pre_roll.append(block)
                    if len(pre_roll) > pre_roll_blocks:
                        pre_roll = pre_roll[-pre_roll_blocks:]
                    if not speech:
                        chunker.maybe_emit()
                        continue

                    active = True
                    silence_ms = 0
                    audio = np.concatenate(pre_roll).astype(np.float32, copy=False)
                    pre_roll = []
                    last_decode = 0.0
                    committer.reset()
                    print("[mic] speech started", flush=True)
                else:
                    audio = np.concatenate((audio, block))

                if speech:
                    silence_ms = 0
                else:
                    silence_ms += block_ms

                should_decode = (
                    audio.size >= int(sample_rate * args.min_decode_ms / 1000)
                    and (now - last_decode) * 1000.0 >= args.step_ms
                )

                current_words: list[Word] = []
                if should_decode:
                    current_words = _transcribe_words(
                        model, audio, args.language, chunker.prompt()
                    )
                    last_decode = time.monotonic()
                    if current_words:
                        print(
                            "[stt] " + _join_words([w.text for w in current_words]),
                            flush=True,
                        )
                        trim_to = committer.observe(current_words)
                        if trim_to > 0:
                            trim_samples = min(
                                audio.size,
                                max(1, int(trim_to * sample_rate)),
                            )
                            audio = audio[trim_samples:].copy()
                            last_decode = 0.0
                    chunker.maybe_emit()

                endpoint = silence_ms >= args.endpoint_ms
                safety_flush = audio.size >= int(sample_rate * args.max_uncommitted_seconds)

                if endpoint or safety_flush:
                    final_words = _transcribe_words(
                        model, audio, args.language, chunker.prompt()
                    ) if audio.size else []
                    if final_words:
                        print(
                            "[final] " + _join_words([w.text for w in final_words]),
                            flush=True,
                        )
                    committer.finalize(final_words)
                    audio = np.zeros((0,), dtype=np.float32)
                    silence_ms = 0
                    last_decode = 0.0
                    if endpoint:
                        active = False
                        pre_roll = []
                        print("[mic] endpoint", flush=True)
                    else:
                        # Continue listening immediately after a forced safety flush.
                        committer.reset()
        except KeyboardInterrupt:
            print("\nStopping live speech.", flush=True)
            return 0


def _self_test() -> int:
    class Sink:
        def __init__(self):
            self.items: list[str] = []
        def speak(self, text: str) -> None:
            self.items.append(text)

    sink = Sink()
    chunker = Chunker(sink, min_words=2, max_words=4, max_delay_ms=1)
    c = StableWordCommitter(tail_words=1, chunker=chunker)

    w1 = [Word("hello", 0.0, 0.2), Word("there", 0.2, 0.4), Word("friend", 0.4, 0.6)]
    assert c.observe(w1) == 0.0
    w2 = [Word("hello", 0.0, 0.2), Word("there", 0.2, 0.4), Word("friend", 0.4, 0.6), Word("today", 0.6, 0.8)]
    trim = c.observe(w2)
    assert 0.39 <= trim <= 0.41, trim
    chunker.flush()
    assert sink.items == ["hello there"], sink.items

    remaining = [Word("friend", 0.0, 0.2), Word("today", 0.2, 0.4)]
    c.finalize(remaining)
    assert sink.items[-1] == "friend today", sink.items
    print("live_stt_tts self-test passed")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Near-real-time microphone -> faster-whisper -> Fast TTS cloned-voice bridge."
    )
    p.add_argument("--model", default="turbo", help="faster-whisper model name/path")
    p.add_argument("--device", default="cuda", choices=("cuda", "cpu"))
    p.add_argument("--compute-type", default="int8_float16")
    p.add_argument("--language", default="en")
    p.add_argument("--input-device", default=None, help="sounddevice input index or device name")
    p.add_argument("--sample-rate", type=int, default=16000)
    p.add_argument("--block-ms", type=int, default=20)
    p.add_argument("--step-ms", type=int, default=320)
    p.add_argument("--min-decode-ms", type=int, default=480)
    p.add_argument("--endpoint-ms", type=int, default=450)
    p.add_argument("--pre-roll-ms", type=int, default=240)
    p.add_argument("--speech-threshold-db", type=float, default=-43.0)
    p.add_argument("--tail-words", type=int, default=2)
    p.add_argument("--min-chunk-words", type=int, default=3)
    p.add_argument("--max-chunk-words", type=int, default=7)
    p.add_argument("--max-chunk-delay-ms", type=int, default=650)
    p.add_argument("--max-uncommitted-seconds", type=float, default=10.0)
    p.add_argument("--voicebox-wait", type=float, default=90.0)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--list-devices", action="store_true")
    p.add_argument("--self-test", action="store_true")
    return p


def main() -> int:
    args = build_parser().parse_args()
    if args.self_test:
        return _self_test()
    return run_live(args)


if __name__ == "__main__":
    raise SystemExit(main())
