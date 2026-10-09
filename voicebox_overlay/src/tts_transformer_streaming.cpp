#include "tts_transformer.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qwen3_tts {

static int32_t stream_argmax(const float * data, int32_t n) {
    int32_t max_idx = 0;
    float max_val = data[0];
    for (int32_t i = 1; i < n; ++i) {
        if (data[i] > max_val) {
            max_val = data[i];
            max_idx = i;
        }
    }
    return max_idx;
}

bool TTSTransformer::generate_streaming(const int32_t * text_tokens, int32_t n_tokens,
                                        const float * speaker_embd, int32_t max_len,
                                        std::vector<int32_t> & output,
                                        int32_t language_id,
                                        float repetition_penalty,
                                        float temperature,
                                        int32_t top_k,
                                        tts_frame_callback_t frame_callback,
                                        tts_generation_continue_callback_t should_continue) {
    if (!model_.ctx) {
        error_msg_ = "Model not loaded";
        return false;
    }
    if (!text_tokens) {
        error_msg_ = "text_tokens is null";
        return false;
    }
    if (n_tokens < 4) {
        error_msg_ = "Need at least 4 text tokens for generation";
        return false;
    }
    if (max_len <= 0) {
        output.clear();
        return true;
    }

    const auto & cfg = model_.config;
    std::vector<float> prefill_embd;
    std::vector<float> trailing_text_hidden;
    std::vector<float> tts_pad_embed;
    if (!build_prefill_graph(text_tokens, n_tokens, speaker_embd, language_id,
                             prefill_embd, trailing_text_hidden, tts_pad_embed)) {
        return false;
    }

    const int32_t prefill_len = (int32_t)(prefill_embd.size() / cfg.hidden_size);
    const int32_t trailing_len = (int32_t)(trailing_text_hidden.size() / cfg.hidden_size);
    const int32_t required_ctx = prefill_len + max_len + 8;
    if (state_.cache.n_ctx < required_ctx ||
        state_.cache.n_ctx > std::max<int32_t>(required_ctx * 2, 512)) {
        if (!init_kv_cache(required_ctx)) {
            return false;
        }
    }
    clear_kv_cache();

    std::vector<float> hidden_out;
    std::vector<float> logits;
    if (!forward_prefill(prefill_embd.data(), prefill_len, 0, hidden_out, &logits)) {
        return false;
    }

    output.clear();
    output.reserve((size_t)max_len * cfg.n_codebooks);

    int32_t n_past = prefill_len;
    std::vector<int32_t> frame_codes(cfg.n_codebooks);
    std::unordered_set<int32_t> generated_cb0_tokens;
    const int32_t suppress_start = cfg.codec_vocab_size - 1024;

    std::vector<float> probs(cfg.codec_vocab_size);
    std::vector<float> step_embd(cfg.hidden_size, 0.0f);
    std::vector<float> embd_row(cfg.hidden_size);

    for (int frame = 0; frame < max_len; ++frame) {
        if (should_continue && !should_continue()) {
            break;
        }

        for (int32_t i = suppress_start; i < cfg.codec_vocab_size; ++i) {
            if (i != cfg.codec_eos_id) {
                logits[i] = -INFINITY;
            }
        }

        if (repetition_penalty != 1.0f) {
            for (int32_t tok : generated_cb0_tokens) {
                if (tok >= 0 && tok < cfg.codec_vocab_size) {
                    if (logits[tok] > 0.0f) logits[tok] /= repetition_penalty;
                    else                    logits[tok] *= repetition_penalty;
                }
            }
        }

        int32_t next_token;
        if (temperature <= 0.0f) {
            next_token = stream_argmax(logits.data(), cfg.codec_vocab_size);
        } else {
            std::vector<float> sample_logits = logits;
            for (int32_t i = 0; i < cfg.codec_vocab_size; ++i) {
                sample_logits[i] /= temperature;
            }
            if (top_k > 0 && top_k < cfg.codec_vocab_size) {
                std::vector<std::pair<float, int32_t>> scored(cfg.codec_vocab_size);
                for (int32_t i = 0; i < cfg.codec_vocab_size; ++i) {
                    scored[i] = {sample_logits[i], i};
                }
                std::partial_sort(scored.begin(), scored.begin() + top_k, scored.end(),
                    [](const auto & a, const auto & b) { return a.first > b.first; });
                const float threshold = scored[top_k - 1].first;
                for (int32_t i = 0; i < cfg.codec_vocab_size; ++i) {
                    if (sample_logits[i] < threshold) sample_logits[i] = -INFINITY;
                }
            }

            const float max_logit = *std::max_element(sample_logits.begin(), sample_logits.end());
            double sum = 0.0;
            for (int32_t i = 0; i < cfg.codec_vocab_size; ++i) {
                probs[i] = expf(sample_logits[i] - max_logit);
                sum += probs[i];
            }
            if (!(sum > 0.0) || !std::isfinite(sum)) {
                next_token = stream_argmax(logits.data(), cfg.codec_vocab_size);
            } else {
                for (int32_t i = 0; i < cfg.codec_vocab_size; ++i) {
                    probs[i] = (float)(probs[i] / sum);
                }
                std::discrete_distribution<int32_t> dist(probs.begin(), probs.end());
                next_token = dist(rng_);
            }
        }

        if (next_token == cfg.codec_eos_id) {
            break;
        }

        frame_codes[0] = next_token;
        generated_cb0_tokens.insert(next_token);

        std::vector<int32_t> codes_1_15;
        if (!predict_codes_autoregressive(last_hidden_.data(), frame_codes[0],
                                          codes_1_15, temperature, top_k)) {
            return false;
        }
        for (int cb = 1; cb < cfg.n_codebooks; ++cb) {
            frame_codes[cb] = codes_1_15[cb - 1];
        }
        for (int cb = 0; cb < cfg.n_codebooks; ++cb) {
            output.push_back(frame_codes[cb]);
        }

        if (frame_callback && !frame_callback(frame_codes.data(), frame, cfg.n_codebooks)) {
            break;
        }

        if (frame + 1 >= max_len) {
            break;
        }
        if (should_continue && !should_continue()) {
            break;
        }

        std::fill(step_embd.begin(), step_embd.end(), 0.0f);
        if (!lookup_single_embedding_row(model_.codec_embd, frame_codes[0], embd_row.data())) {
            return false;
        }
        for (int32_t h = 0; h < cfg.hidden_size; ++h) {
            step_embd[h] = embd_row[h];
        }
        for (int cb = 1; cb < cfg.n_codebooks; ++cb) {
            const int32_t code_token = frame_codes[cb];
            if (!lookup_single_embedding_row(model_.code_pred_embd[cb - 1], code_token,
                                             embd_row.data())) {
                return false;
            }
            for (int32_t h = 0; h < cfg.hidden_size; ++h) {
                step_embd[h] += embd_row[h];
            }
        }

        const float * trailing_row = (frame < trailing_len)
            ? trailing_text_hidden.data() + (size_t)frame * cfg.hidden_size
            : tts_pad_embed.data();
        for (int32_t h = 0; h < cfg.hidden_size; ++h) {
            step_embd[h] += trailing_row[h];
        }

        if (!forward_step(step_embd.data(), n_past, logits)) {
            return false;
        }
        n_past++;
    }

    return true;
}

} // namespace qwen3_tts
