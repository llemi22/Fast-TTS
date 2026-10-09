#include "audio_tokenizer_decoder.h"

namespace qwen3_tts {

bool AudioTokenizerDecoder::decode_window(const int32_t * codes, int32_t n_frames,
                                          int32_t position_offset,
                                          std::vector<float> & samples) {
    return decode_single(codes, n_frames, position_offset, samples);
}

int64_t AudioTokenizerDecoder::samples_for_frames(int32_t n_frames) const {
    return output_samples_for_frames(n_frames);
}

} // namespace qwen3_tts
