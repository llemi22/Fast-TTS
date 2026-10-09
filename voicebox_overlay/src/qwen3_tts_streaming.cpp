#include "qwen3_tts.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace qwen3_tts {

static int64_t stream_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

tts_result Qwen3TTS::synthesize_streaming(const std::string & text,
                                           const tts_params & params,
                                           const tts_stream_params & stream_params,
                                           tts_audio_chunk_callback_t audio_callback,
                                           tts_generation_continue_callback_t should_continue) {
    tts_result result;
    if (!models_loaded_) {
        result.error_msg = "Models not loaded";
        return result;
    }

    std::vector<float> zero_embedding(transformer_.get_config().hidden_size, 0.0f);
    return synthesize_streaming_internal(text, zero_embedding.data(), params, stream_params,
                                         std::move(audio_callback),
                                         std::move(should_continue), result);
}

tts_result Qwen3TTS::synthesize_streaming_with_embedding(
        const std::string & text,
        const float * embedding, int32_t embedding_size,
        const tts_params & params,
        const tts_stream_params & stream_params,
        tts_audio_chunk_callback_t audio_callback,
        tts_generation_continue_callback_t should_continue) {
    tts_result result;
    if (!models_loaded_) {
        result.error_msg = "Models not loaded";
        return result;
    }
    if (!embedding || embedding_size != transformer_.get_config().hidden_size) {
        result.error_msg = "Invalid speaker embedding";
        return result;
    }
    return synthesize_streaming_internal(text, embedding, params, stream_params,
                                         std::move(audio_callback),
                                         std::move(should_continue), result);
}

tts_result Qwen3TTS::synthesize_streaming_internal(
        const std::string & text,
        const float * speaker_embedding,
        const tts_params & params,
        const tts_stream_params & stream_params,
        tts_audio_chunk_callback_t audio_callback,
        tts_generation_continue_callback_t should_continue,
        tts_result & result) {
    const int64_t total_start = stream_now_ms();

    const int first_chunk_frames = std::max(1, stream_params.first_chunk_frames);
    const int chunk_frames = std::max(1, stream_params.chunk_frames);
    const int context_frames = std::max(0, stream_params.context_frames);

    const int64_t tokenize_start = stream_now_ms();
    std::vector<int32_t> text_tokens = tokenizer_.encode_for_tts(text);
    result.t_tokenize_ms = stream_now_ms() - tokenize_start;
    if (text_tokens.empty()) {
        result.error_msg = "Failed to tokenize text";
        return result;
    }

    if (!transformer_loaded_) {
        if (tts_model_path_.empty() || !transformer_.load_model(tts_model_path_)) {
            result.error_msg = "Failed to load TTS transformer: " + transformer_.get_error();
            return result;
        }
        transformer_loaded_ = true;
    }
    if (!decoder_loaded_) {
        if (decoder_model_path_.empty() || !audio_decoder_.load_model(decoder_model_path_)) {
            result.error_msg = "Failed to load vocoder: " + audio_decoder_.get_error();
            return result;
        }
        decoder_loaded_ = true;
    }

    transformer_.clear_kv_cache();

    std::vector<int32_t> speech_codes;
    const int n_codebooks = transformer_.get_config().n_codebooks;
    int emitted_frames = 0;
    int next_emit_frame = first_chunk_frames;
    bool cancelled = false;
    bool stream_failed = false;
    std::string stream_error;
    int64_t decode_ms = 0;

    auto continue_ok = [&]() -> bool {
        if (should_continue && !should_continue()) {
            cancelled = true;
            return false;
        }
        return true;
    };

    auto emit_through = [&](int total_frames) -> bool {
        if (total_frames <= emitted_frames) return true;
        if (!continue_ok()) return false;

        const int ctx_start = std::max(0, emitted_frames - context_frames);
        const int warmup_frames = emitted_frames - ctx_start;
        const int segment_frames = total_frames - ctx_start;
        if (segment_frames <= 0) return true;

        std::vector<float> segment;
        const int64_t t0 = stream_now_ms();
        if (!audio_decoder_.decode_window(
                speech_codes.data() + (size_t)ctx_start * n_codebooks,
                segment_frames, ctx_start, segment)) {
            decode_ms += stream_now_ms() - t0;
            stream_failed = true;
            stream_error = "Streaming vocoder decode failed: " + audio_decoder_.get_error();
            return false;
        }
        decode_ms += stream_now_ms() - t0;

        const int64_t drop64 = audio_decoder_.samples_for_frames(warmup_frames);
        const size_t drop = (size_t)std::min<int64_t>(drop64, (int64_t)segment.size());
        if (drop < segment.size()) {
            std::vector<float> chunk(segment.begin() + (std::vector<float>::difference_type)drop,
                                     segment.end());
            if (audio_callback && !audio_callback(std::move(chunk))) {
                cancelled = true;
                return false;
            }
        }

        emitted_frames = total_frames;
        return continue_ok();
    };

    const int64_t generate_start = stream_now_ms();
    const bool generated = transformer_.generate_streaming(
        text_tokens.data(), (int32_t)text_tokens.size(),
        speaker_embedding, params.max_audio_tokens, speech_codes,
        params.language_id, params.repetition_penalty,
        params.temperature, params.top_k,
        [&](const int32_t *, int32_t frame_index, int32_t) -> bool {
            if (!continue_ok()) return false;
            const int total_frames = frame_index + 1;
            if (total_frames >= next_emit_frame) {
                if (!emit_through(total_frames)) return false;
                next_emit_frame = total_frames + chunk_frames;
            }
            return true;
        },
        continue_ok);
    result.t_generate_ms = stream_now_ms() - generate_start - decode_ms;
    result.t_decode_ms = decode_ms;

    if (!generated) {
        result.error_msg = "Failed to generate speech codes: " + transformer_.get_error();
        return result;
    }
    if (stream_failed) {
        result.error_msg = stream_error;
        return result;
    }

    const int total_frames = (int)(speech_codes.size() / (size_t)n_codebooks);
    if (!cancelled && total_frames > emitted_frames) {
        if (!emit_through(total_frames) && stream_failed) {
            result.error_msg = stream_error;
            return result;
        }
    }

    result.sample_rate = audio_decoder_.get_config().sample_rate;
    result.t_total_ms = stream_now_ms() - total_start;
    result.success = !stream_failed;
    return result;
}

} // namespace qwen3_tts
