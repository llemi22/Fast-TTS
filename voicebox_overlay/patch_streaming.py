from __future__ import annotations
import shutil
import sys
from pathlib import Path

MARK = "VOICEBOX_STREAMING_API"


def die(msg: str) -> None:
    raise SystemExit(f"[VoiceBox streaming patch] {msg}")


def backup(path: Path) -> None:
    bak = path.with_suffix(path.suffix + ".voicebox_streaming.bak")
    if not bak.exists():
        shutil.copy2(path, bak)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        die(f"Expected exactly one {label} anchor, found {count}. Upstream source may have changed.")
    return text.replace(old, new, 1)


def patch_transformer_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if f"{MARK}_TRANSFORMER" in text:
        return
    backup(path)
    if "#include <functional>" not in text:
        text = replace_once(text, "#include <random>\n", "#include <random>\n#include <functional>\n", "tts_transformer include")

    class_anchor = "// TTS Transformer class\nclass TTSTransformer {"
    api = """// VOICEBOX_STREAMING_API_TRANSFORMER
using tts_frame_callback_t = std::function<bool(const int32_t * frame_codes, int32_t frame_index, int32_t n_codebooks)>;
using tts_generation_continue_callback_t = std::function<bool()>;

"""
    text = replace_once(text, class_anchor, api + class_anchor, "TTSTransformer class")

    gen_anchor = """    bool generate(const int32_t * text_tokens, int32_t n_tokens,
                  const float * speaker_embd, int32_t max_len,
                  std::vector<int32_t> & output,
                  int32_t language_id = 2050,
                  float repetition_penalty = 1.05f,
                  float temperature = 0.9f,
                  int32_t top_k = 50);
"""
    gen_new = gen_anchor + """
    // Low-TTFA variant: emits each completed codec frame and can stop between frames.
    bool generate_streaming(const int32_t * text_tokens, int32_t n_tokens,
                            const float * speaker_embd, int32_t max_len,
                            std::vector<int32_t> & output,
                            int32_t language_id = 2050,
                            float repetition_penalty = 1.05f,
                            float temperature = 0.9f,
                            int32_t top_k = 50,
                            tts_frame_callback_t frame_callback = nullptr,
                            tts_generation_continue_callback_t should_continue = nullptr);
"""
    text = replace_once(text, gen_anchor, gen_new, "TTSTransformer::generate declaration")
    path.write_text(text, encoding="utf-8")


def patch_decoder_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if f"{MARK}_DECODER" in text:
        return
    backup(path)
    anchor = """    bool decode(const int32_t * codes, int32_t n_frames,
                std::vector<float> & samples);
"""
    new = anchor + """
    // VOICEBOX_STREAMING_API_DECODER
    // Decode a bounded window at its absolute codec-frame offset. Used for overlapped live chunks.
    bool decode_window(const int32_t * codes, int32_t n_frames, int32_t position_offset,
                       std::vector<float> & samples);

    // Exact output sample count used by the upstream chunked decoder when dropping left context.
    int64_t samples_for_frames(int32_t n_frames) const;
"""
    text = replace_once(text, anchor, new, "AudioTokenizerDecoder::decode declaration")
    path.write_text(text, encoding="utf-8")


def patch_qwen_h(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    if f"{MARK}_QWEN" in text:
        return
    backup(path)
    cb_anchor = "using tts_progress_callback_t = std::function<void(int tokens_generated, int max_tokens)>;\n"
    cb_new = cb_anchor + """
// VOICEBOX_STREAMING_API_QWEN
struct tts_stream_params {
    int32_t first_chunk_frames = 4;
    int32_t chunk_frames = 8;
    int32_t context_frames = 12;
};

using tts_audio_chunk_callback_t = std::function<bool(std::vector<float> && audio)>;
"""
    text = replace_once(text, cb_anchor, cb_new, "Qwen progress callback")

    public_anchor = """    tts_result synthesize_with_embedding(const std::string & text,
                                          const float * embedding, int32_t embedding_size,
                                          const tts_params & params = tts_params());
"""
    public_new = public_anchor + """
    // Incremental low-TTFA synthesis. Audio chunks are delivered during transformer generation.
    tts_result synthesize_streaming(
        const std::string & text,
        const tts_params & params,
        const tts_stream_params & stream_params,
        tts_audio_chunk_callback_t audio_callback,
        tts_generation_continue_callback_t should_continue = nullptr);

    tts_result synthesize_streaming_with_embedding(
        const std::string & text,
        const float * embedding, int32_t embedding_size,
        const tts_params & params,
        const tts_stream_params & stream_params,
        tts_audio_chunk_callback_t audio_callback,
        tts_generation_continue_callback_t should_continue = nullptr);
"""
    text = replace_once(text, public_anchor, public_new, "Qwen synthesize_with_embedding declaration")

    private_anchor = """    tts_result synthesize_internal(const std::string & text,
                                   const float * speaker_embedding,
                                   const tts_params & params,
                                   tts_result & result);
"""
    private_new = private_anchor + """
    tts_result synthesize_streaming_internal(
        const std::string & text,
        const float * speaker_embedding,
        const tts_params & params,
        const tts_stream_params & stream_params,
        tts_audio_chunk_callback_t audio_callback,
        tts_generation_continue_callback_t should_continue,
        tts_result & result);
"""
    text = replace_once(text, private_anchor, private_new, "Qwen private synthesize_internal declaration")
    path.write_text(text, encoding="utf-8")


def patch_cmake(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    backup(path)

    if "src/tts_transformer_streaming.cpp" not in text:
        anchor = """add_library(tts_transformer STATIC
    ${TTS_TRANSFORMER_SOURCES}
)"""
        repl = """add_library(tts_transformer STATIC
    ${TTS_TRANSFORMER_SOURCES}
    src/tts_transformer_streaming.cpp
)"""
        text = replace_once(text, anchor, repl, "tts_transformer CMake block")

    if "src/audio_tokenizer_decoder_streaming.cpp" not in text:
        anchor = """add_library(audio_tokenizer_decoder STATIC
    src/audio_tokenizer_decoder.cpp
    src/gguf_loader.cpp
)"""
        repl = """add_library(audio_tokenizer_decoder STATIC
    src/audio_tokenizer_decoder.cpp
    src/audio_tokenizer_decoder_streaming.cpp
    src/gguf_loader.cpp
)"""
        text = replace_once(text, anchor, repl, "audio decoder CMake block")

    if "src/qwen3_tts_streaming.cpp" not in text:
        anchor = """add_library(qwen3_tts STATIC
    src/qwen3_tts.cpp
)"""
        repl = """add_library(qwen3_tts STATIC
    src/qwen3_tts.cpp
    src/qwen3_tts_streaming.cpp
)"""
        text = replace_once(text, anchor, repl, "qwen3_tts CMake block")

    voicebox_marker = "add_executable(qwen3-tts-voicebox WIN32"
    if voicebox_marker not in text:
        text += """

# Qwen3-TTS VoiceBox — native Windows low-TTFA Enter-to-speak GUI
if(WIN32)
    add_executable(qwen3-tts-voicebox WIN32
        src/voicebox_win.cpp
    )
    target_link_libraries(qwen3-tts-voicebox PRIVATE
        qwen3_tts
        winmm
        comctl32
        ole32
        shell32
        uuid
    )
endif()
"""

    path.write_text(text, encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 2:
        die("usage: patch_streaming.py <qwen3-tts.cpp repo root>")
    root = Path(sys.argv[1]).resolve()
    required = [
        root / "CMakeLists.txt",
        root / "src" / "tts_transformer.h",
        root / "src" / "audio_tokenizer_decoder.h",
        root / "src" / "qwen3_tts.h",
    ]
    missing = [str(p) for p in required if not p.exists()]
    if missing:
        die("Missing expected upstream files: " + ", ".join(missing))

    patch_transformer_h(root / "src" / "tts_transformer.h")
    patch_decoder_h(root / "src" / "audio_tokenizer_decoder.h")
    patch_qwen_h(root / "src" / "qwen3_tts.h")
    patch_cmake(root / "CMakeLists.txt")
    print("Streaming engine API + CMake patch applied successfully.")


if __name__ == "__main__":
    main()
