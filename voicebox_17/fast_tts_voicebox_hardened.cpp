#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>

#include "audio-io.h"
#include "qwen.h"
#include "rvq-file.h"
#include "utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr UINT WM_FAST_STATUS = WM_APP + 1;
constexpr int ID_EDIT = 1001;
constexpr int ID_SPEAK = 1002;
constexpr int ID_STOP = 1003;
constexpr int ID_VOICE = 1004;
constexpr int RVQ_CODE_BITS = 11;
constexpr int OUTPUT_SAMPLE_RATE = 24000;
constexpr int OUTPUT_CHANNELS = 1;
static_assert(OUTPUT_SAMPLE_RATE == 24000, "Pinned qwentts.cpp ABI is 24 kHz mono");

HWND g_main = nullptr;
HWND g_edit = nullptr;
HWND g_status = nullptr;
HWND g_voice_combo = nullptr;

std::string wide_to_utf8(const std::wstring & s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t) n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int) s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const std::string & s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring out((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), out.data(), n);
    return out;
}

void post_status(HWND hwnd, const std::wstring & text) {
    auto * copy = new std::wstring(text);
    if (!PostMessageW(hwnd, WM_FAST_STATUS, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

bool has_nonspace(const std::wstring & s) {
    for (wchar_t c : s) if (!iswspace(c)) return true;
    return false;
}

std::string read_text_file_utf8(const std::string & path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() >= 3 && (unsigned char) s[0] == 0xEF && (unsigned char) s[1] == 0xBB && (unsigned char) s[2] == 0xBF) s.erase(0, 3);
    return s;
}

bool file_exists_utf8(const std::string & path) {
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(utf8_to_wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string join_path_utf8(const std::string & dir, const std::string & leaf) {
    if (dir.empty()) return leaf;
    char last = dir.back();
    return dir + ((last == '\\' || last == '/') ? "" : "\\") + leaf;
}

std::wstring get_env_w(const wchar_t * name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring out(n, L'\0');
    GetEnvironmentVariableW(name, out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::wstring settings_path() {
    std::wstring base = get_env_w(L"LOCALAPPDATA");
    if (base.empty()) return L"fast-tts.ini";
    std::wstring dir = base + L"\\Fast-TTS";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

std::wstring read_saved_voice_id() {
    wchar_t buf[512]{};
    std::wstring ini = settings_path();
    GetPrivateProfileStringW(L"Voice", L"SelectedId", L"", buf, (DWORD) (sizeof(buf) / sizeof(buf[0])), ini.c_str());
    return buf;
}

void save_voice_id(const std::string & id) {
    if (id == "__command_line") return;
    std::wstring ini = settings_path();
    std::wstring wid = utf8_to_wide(id);
    WritePrivateProfileStringW(L"Voice", L"SelectedId", wid.c_str(), ini.c_str());
}

bool read_spk_file_local(const std::string & path, std::vector<float> & emb) {
    FILE * f = utf8_fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (sz % (long) sizeof(float)) != 0) {
        fclose(f);
        return false;
    }
    emb.resize((size_t) sz / sizeof(float));
    bool ok = fread(emb.data(), sizeof(float), emb.size(), f) == emb.size();
    fclose(f);
    return ok;
}

struct VoiceProfile {
    std::string id;
    std::string name;
    std::string wav;
    std::string text;
    std::string spk;
    std::string rvq;
};

bool valid_voice_id(const std::string & id) {
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char c : id) {
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

void append_utf8(std::string & out, uint32_t cp) {
    if (cp <= 0x7F) out.push_back((char) cp);
    else if (cp <= 0x7FF) {
        out.push_back((char) (0xC0 | (cp >> 6)));
        out.push_back((char) (0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char) (0xE0 | (cp >> 12)));
        out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char) (0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char) (0xF0 | (cp >> 18)));
        out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char) (0x80 | (cp & 0x3F)));
    }
}

class JsonReader {
    const std::string & s_;
    size_t p_ = 0;
    std::string error_;

    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_;
    }

    bool fail(const char * msg) {
        if (error_.empty()) error_ = msg;
        return false;
    }

    bool consume(char c) {
        ws();
        if (p_ >= s_.size() || s_[p_] != c) return false;
        ++p_;
        return true;
    }

    int hex_value(char c) const {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    }

    bool parse_hex4(uint32_t & cp) {
        if (p_ + 4 > s_.size()) return fail("short unicode escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            int h = hex_value(s_[p_++]);
            if (h < 0) return fail("invalid unicode escape");
            cp = (cp << 4) | (uint32_t) h;
        }
        return true;
    }

    bool parse_string(std::string & out) {
        ws();
        if (p_ >= s_.size() || s_[p_] != '"') return fail("expected JSON string");
        ++p_;
        out.clear();
        while (p_ < s_.size()) {
            unsigned char c = (unsigned char) s_[p_++];
            if (c == '"') return true;
            if (c < 0x20) return fail("unescaped control character in JSON string");
            if (c != '\\') {
                out.push_back((char) c);
                continue;
            }
            if (p_ >= s_.size()) return fail("trailing JSON escape");
            char e = s_[p_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p_ + 2 > s_.size() || s_[p_] != '\\' || s_[p_ + 1] != 'u') return fail("missing low surrogate");
                        p_ += 2;
                        uint32_t low = 0;
                        if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) return fail("invalid low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unexpected low surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("invalid JSON escape");
            }
        }
        return fail("unterminated JSON string");
    }

    bool skip_number() {
        ws();
        size_t start = p_;
        if (p_ < s_.size() && s_[p_] == '-') ++p_;
        if (p_ >= s_.size()) return fail("invalid JSON number");
        if (s_[p_] == '0') ++p_;
        else {
            if (!std::isdigit((unsigned char) s_[p_])) return fail("invalid JSON number");
            while (p_ < s_.size() && std::isdigit((unsigned char) s_[p_])) ++p_;
        }
        if (p_ < s_.size() && s_[p_] == '.') {
            ++p_;
            if (p_ >= s_.size() || !std::isdigit((unsigned char) s_[p_])) return fail("invalid JSON fraction");
            while (p_ < s_.size() && std::isdigit((unsigned char) s_[p_])) ++p_;
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            ++p_;
            if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
            if (p_ >= s_.size() || !std::isdigit((unsigned char) s_[p_])) return fail("invalid JSON exponent");
            while (p_ < s_.size() && std::isdigit((unsigned char) s_[p_])) ++p_;
        }
        return p_ > start;
    }

    bool literal(const char * word) {
        ws();
        size_t n = std::char_traits<char>::length(word);
        if (p_ + n > s_.size() || s_.compare(p_, n, word) != 0) return false;
        p_ += n;
        return true;
    }

    bool skip_value(int depth = 0) {
        if (depth > 64) return fail("JSON nesting too deep");
        ws();
        if (p_ >= s_.size()) return fail("unexpected end of JSON");
        if (s_[p_] == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (s_[p_] == '{') {
            ++p_;
            ws();
            if (consume('}')) return true;
            for (;;) {
                std::string key;
                if (!parse_string(key) || !consume(':') || !skip_value(depth + 1)) return false;
                if (consume('}')) return true;
                if (!consume(',')) return fail("expected comma in JSON object");
            }
        }
        if (s_[p_] == '[') {
            ++p_;
            ws();
            if (consume(']')) return true;
            for (;;) {
                if (!skip_value(depth + 1)) return false;
                if (consume(']')) return true;
                if (!consume(',')) return fail("expected comma in JSON array");
            }
        }
        if (s_[p_] == '-' || std::isdigit((unsigned char) s_[p_])) return skip_number();
        if (literal("true") || literal("false") || literal("null")) return true;
        return fail("invalid JSON value");
    }

    bool parse_profile_object(std::string & id, std::string & name) {
        if (!consume('{')) return fail("expected object in voice manifest");
        ws();
        if (consume('}')) return true;
        for (;;) {
            std::string key;
            if (!parse_string(key) || !consume(':')) return false;
            if (key == "id" || key == "name") {
                std::string value;
                if (!parse_string(value)) return false;
                if (key == "id") id = std::move(value); else name = std::move(value);
            } else {
                if (!skip_value(1)) return false;
            }
            if (consume('}')) return true;
            if (!consume(',')) return fail("expected comma in voice object");
        }
    }

public:
    explicit JsonReader(const std::string & s) : s_(s) {}

    bool parse_manifest(std::vector<std::pair<std::string, std::string>> & items) {
        if (!consume('[')) return fail("voice manifest must be a JSON array");
        ws();
        if (consume(']')) return true;
        for (;;) {
            std::string id, name;
            if (!parse_profile_object(id, name)) return false;
            items.emplace_back(std::move(id), std::move(name));
            if (consume(']')) break;
            if (!consume(',')) return fail("expected comma in voice manifest");
        }
        ws();
        if (p_ != s_.size()) return fail("trailing data after voice manifest");
        return true;
    }

    const std::string & error() const { return error_; }
};

std::vector<VoiceProfile> discover_voices(const std::string & dir, std::string & warning) {
    std::vector<VoiceProfile> voices;
    voices.push_back(VoiceProfile{"", "Default / no clone", "", "", "", ""});
    if (dir.empty()) return voices;

    std::string manifest = read_text_file_utf8(join_path_utf8(dir, "migration-manifest.json"));
    if (manifest.empty()) return voices;

    std::vector<std::pair<std::string, std::string>> entries;
    JsonReader reader(manifest);
    if (!reader.parse_manifest(entries)) {
        warning = "Voice manifest is invalid JSON: " + reader.error();
        return voices;
    }

    int unsafe = 0;
    for (auto & entry : entries) {
        const std::string & id = entry.first;
        if (!valid_voice_id(id)) {
            ++unsafe;
            continue;
        }
        VoiceProfile v;
        v.id = id;
        v.name = entry.second.empty() ? id : entry.second;
        std::string base = join_path_utf8(dir, id);
        std::string p;
        p = base + ".wav"; if (file_exists_utf8(p)) v.wav = p;
        p = base + ".txt"; if (file_exists_utf8(p)) v.text = p;
        p = base + ".spk"; if (file_exists_utf8(p)) v.spk = p;
        p = base + ".rvq"; if (file_exists_utf8(p)) v.rvq = p;
        if (!v.spk.empty() || !v.wav.empty()) voices.push_back(std::move(v));
    }
    if (unsafe > 0) warning = "Skipped " + std::to_string(unsafe) + " unsafe voice manifest id(s).";
    return voices;
}

struct Config {
    std::string model;
    std::string codec;
    std::string lang = "English";
    std::string voice_dir;
    std::string ref_wav;
    std::string ref_text_file;
    std::string ref_spk;
    std::string ref_rvq;
    int max_tokens = 1024;
    int top_k = -1;
    float temperature = -1.0f;
    int64_t seed = -2;
    bool warmup = true;
    bool use_fa = true;
};

bool parse_args(Config & cfg) {
    int argc = 0;
    LPWSTR * argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    auto next = [&](int & i) -> std::string {
        if (i + 1 >= argc) return {};
        return wide_to_utf8(argv[++i]);
    };
    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--model") cfg.model = next(i);
        else if (a == L"--codec") cfg.codec = next(i);
        else if (a == L"--lang") cfg.lang = next(i);
        else if (a == L"--voice-dir") cfg.voice_dir = next(i);
        else if (a == L"--ref-wav") cfg.ref_wav = next(i);
        else if (a == L"--ref-text") cfg.ref_text_file = next(i);
        else if (a == L"--ref-spk") cfg.ref_spk = next(i);
        else if (a == L"--ref-rvq") cfg.ref_rvq = next(i);
        else if (a == L"--max-tokens") {
            std::string v = next(i); if (v.empty()) { ok = false; break; }
            cfg.max_tokens = std::max(1, std::atoi(v.c_str()));
        } else if (a == L"--top-k") {
            std::string v = next(i); if (v.empty()) { ok = false; break; }
            cfg.top_k = std::atoi(v.c_str());
        } else if (a == L"--temperature") {
            std::string v = next(i); if (v.empty()) { ok = false; break; }
            cfg.temperature = (float) std::atof(v.c_str());
        } else if (a == L"--seed") {
            std::string v = next(i); if (v.empty()) { ok = false; break; }
            cfg.seed = (int64_t) std::atoll(v.c_str());
        } else if (a == L"--no-warmup") cfg.warmup = false;
        else if (a == L"--no-fa") cfg.use_fa = false;
        else if (a == L"--help" || a == L"-h") ok = false;
        else ok = false;
    }
    LocalFree(argv);
    return ok && !cfg.model.empty() && !cfg.codec.empty();
}

VoiceProfile profile_from_config(const Config & cfg, const std::string & id, const std::string & name) {
    return VoiceProfile{id, name, cfg.ref_wav, cfg.ref_text_file, cfg.ref_spk, cfg.ref_rvq};
}

void apply_profile_to_config(const VoiceProfile & v, Config & cfg) {
    cfg.ref_wav = v.wav;
    cfg.ref_text_file = v.text;
    cfg.ref_spk = v.spk;
    cfg.ref_rvq = v.rvq;
}

class AudioPlayer {
    struct Buffer {
        WAVEHDR hdr{};
        std::vector<int16_t> pcm;
    };

    HWAVEOUT wave_ = nullptr;
    std::mutex mu_;
    std::deque<std::unique_ptr<Buffer>> active_;

    void cleanup_locked() {
        if (!wave_) return;
        for (auto it = active_.begin(); it != active_.end();) {
            MMRESULT rc = waveOutUnprepareHeader(wave_, &(*it)->hdr, sizeof(WAVEHDR));
            if (rc == MMSYSERR_NOERROR) it = active_.erase(it);
            else ++it;
        }
    }

public:
    bool open() {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = OUTPUT_CHANNELS;
        fmt.nSamplesPerSec = OUTPUT_SAMPLE_RATE;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (WORD) (fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        return waveOutOpen(&wave_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }

    bool queue(const float * samples, int n_samples) {
        if (!wave_ || !samples || n_samples <= 0) return false;
        std::lock_guard<std::mutex> lock(mu_);
        cleanup_locked();

        auto b = std::make_unique<Buffer>();
        b->pcm.resize((size_t) n_samples);
        for (int i = 0; i < n_samples; ++i) {
            float x = std::max(-1.0f, std::min(1.0f, samples[i]));
            b->pcm[(size_t) i] = (int16_t) std::lrintf(x * 32767.0f);
        }
        b->hdr.lpData = reinterpret_cast<LPSTR>(b->pcm.data());
        b->hdr.dwBufferLength = (DWORD) (b->pcm.size() * sizeof(int16_t));
        if (waveOutPrepareHeader(wave_, &b->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
        if (waveOutWrite(wave_, &b->hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(wave_, &b->hdr, sizeof(WAVEHDR));
            return false;
        }
        active_.push_back(std::move(b));
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!wave_) return;
        waveOutReset(wave_);
        for (int attempt = 0; attempt < 20 && !active_.empty(); ++attempt) {
            cleanup_locked();
            if (!active_.empty()) Sleep(1);
        }
    }

    void cleanup() {
        std::lock_guard<std::mutex> lock(mu_);
        cleanup_locked();
    }

    ~AudioPlayer() {
        stop();
        std::lock_guard<std::mutex> lock(mu_);
        if (!wave_) return;
        for (int attempt = 0; attempt < 100 && !active_.empty(); ++attempt) {
            cleanup_locked();
            if (!active_.empty()) Sleep(1);
        }
        MMRESULT rc = waveOutClose(wave_);
        if (rc != MMSYSERR_NOERROR) {
            // Never free a buffer Windows may still own. This path should only
            // be reachable on a broken/removed audio device; process exit will
            // reclaim the intentionally leaked emergency buffers.
            for (auto & b : active_) (void) b.release();
            active_.clear();
        }
        wave_ = nullptr;
    }
};

struct ReferenceData {
    qt_voice_ref extracted{};
    std::vector<float> spk;
    std::vector<int32_t> codes;
    int ref_T = 0;
    std::string text;

    ReferenceData() = default;
    ReferenceData(const ReferenceData &) = delete;
    ReferenceData & operator=(const ReferenceData &) = delete;

    ReferenceData(ReferenceData && other) noexcept { *this = std::move(other); }
    ReferenceData & operator=(ReferenceData && other) noexcept {
        if (this == &other) return *this;
        clear();
        extracted = other.extracted;
        other.extracted = {};
        spk = std::move(other.spk);
        codes = std::move(other.codes);
        ref_T = other.ref_T;
        other.ref_T = 0;
        text = std::move(other.text);
        return *this;
    }

    void clear() {
        if (extracted.ref_spk_emb || extracted.ref_codes) qt_voice_ref_free(&extracted);
        extracted = {};
        spk.clear();
        codes.clear();
        ref_T = 0;
        text.clear();
    }

    ~ReferenceData() { clear(); }
};

class Engine {
    struct Job {
        std::string text;
        uint64_t generation = 0;
    };
    struct CallbackCtx {
        Engine * self = nullptr;
        uint64_t generation = 0;
        std::chrono::steady_clock::time_point started;
        std::atomic<bool> first{true};
    };

    Config cfg_;
    HWND hwnd_ = nullptr;
    qt_context * q_ = nullptr;
    ReferenceData ref_;
    AudioPlayer audio_;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::atomic<uint64_t> generation_{1};
    bool quit_ = false;
    bool voice_change_ = false;
    VoiceProfile pending_voice_;

    static bool cancel_cb(void * p) {
        auto * c = static_cast<CallbackCtx *>(p);
        return !c || c->self->generation_.load(std::memory_order_acquire) != c->generation;
    }

    static bool chunk_cb(const float * samples, int n_samples, void * p) {
        auto * c = static_cast<CallbackCtx *>(p);
        if (!c || cancel_cb(c)) return false;
        if (c->first.exchange(false)) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - c->started).count();
            post_status(c->self->hwnd_, L"Speaking - first audio in " + std::to_wstring(ms) + L" ms");
        }
        return c->self->audio_.queue(samples, n_samples) && !cancel_cb(c);
    }

    bool load_reference_into(const VoiceProfile & voice, ReferenceData & dst) {
        if (!voice.text.empty()) {
            dst.text = read_text_file_utf8(voice.text);
            if (dst.text.empty()) post_status(hwnd_, L"Reference transcript could not be read; using speaker-only clone.");
        }
        if (!voice.spk.empty()) {
            if (!read_spk_file_local(voice.spk, dst.spk)) {
                post_status(hwnd_, L"Could not read cached speaker embedding for " + utf8_to_wide(voice.name) + L".");
                return false;
            }
            if (!voice.rvq.empty()) {
                int K = qt_num_codebooks(q_);
                if (!rvq_read_file(voice.rvq.c_str(), K, RVQ_CODE_BITS, dst.codes, &dst.ref_T)) {
                    post_status(hwnd_, L"Could not read cached RVQ data for " + utf8_to_wide(voice.name) + L".");
                    return false;
                }
            }
            return true;
        }
        if (!voice.wav.empty()) {
            int n = 0;
            float * raw = audio_read_mono(voice.wav.c_str(), OUTPUT_SAMPLE_RATE, &n);
            if (!raw || n <= 0) {
                if (raw) std::free(raw);
                post_status(hwnd_, L"Could not decode reference WAV for " + utf8_to_wide(voice.name) + L".");
                return false;
            }
            qt_status rc = qt_extract_voice_ref(q_, raw, n, &dst.extracted);
            std::free(raw);
            if (rc != QT_STATUS_OK) {
                post_status(hwnd_, L"Voice reference extraction failed: " + utf8_to_wide(qt_last_error()));
                return false;
            }
        }
        return true;
    }

    bool switch_reference(const VoiceProfile & voice) {
        ReferenceData candidate;
        if (!load_reference_into(voice, candidate)) return false;
        ref_ = std::move(candidate);
        return true;
    }

    void apply_common_params(qt_tts_params & p) {
        p.lang = cfg_.lang.empty() ? nullptr : cfg_.lang.c_str();
        p.max_new_tokens = cfg_.max_tokens;
        if (cfg_.temperature >= 0.0f) p.temperature = cfg_.temperature;
        if (cfg_.top_k >= 0) p.top_k = cfg_.top_k;
        if (cfg_.seed != -2) p.seed = cfg_.seed;

        const float * spk = nullptr;
        int spk_dim = 0;
        const int32_t * codes = nullptr;
        int ref_T = 0;
        if (ref_.extracted.ref_spk_emb) {
            spk = ref_.extracted.ref_spk_emb;
            spk_dim = ref_.extracted.ref_spk_dim;
            if (!ref_.text.empty()) {
                codes = ref_.extracted.ref_codes;
                ref_T = ref_.extracted.ref_T;
            }
        } else if (!ref_.spk.empty()) {
            spk = ref_.spk.data();
            spk_dim = (int) ref_.spk.size();
            if (!ref_.text.empty() && !ref_.codes.empty()) {
                codes = ref_.codes.data();
                ref_T = ref_.ref_T;
            }
        }
        p.ref_spk_emb = spk;
        p.ref_spk_dim = spk_dim;
        p.ref_codes = codes;
        p.ref_T = ref_T;
        p.ref_text = (codes && !ref_.text.empty()) ? ref_.text.c_str() : nullptr;
    }

    bool init() {
        post_status(hwnd_, L"Loading 1.7B model into memory...");
        qt_init_params ip{};
        qt_init_default_params(&ip);
        ip.talker_path = cfg_.model.c_str();
        ip.codec_path = cfg_.codec.c_str();
        ip.use_fa = cfg_.use_fa;
        ip.max_batch = 1;
        q_ = qt_init(&ip);
        if (!q_) {
            post_status(hwnd_, L"Model load failed: " + utf8_to_wide(qt_last_error()));
            return false;
        }
        if (!audio_.open()) {
            post_status(hwnd_, L"Windows audio output could not be opened.");
            return false;
        }
        if (!switch_reference(profile_from_config(cfg_, "__initial", "initial voice"))) return false;

        if (cfg_.warmup) {
            post_status(hwnd_, L"Warming model...");
            qt_tts_params p{};
            qt_tts_default_params(&p);
            p.text = "Ready.";
            apply_common_params(p);
            p.max_new_tokens = std::min(8, cfg_.max_tokens);
            qt_audio out{};
            qt_status rc = qt_synthesize(q_, &p, &out);
            if (rc == QT_STATUS_OK) {
                const bool format_ok = out.sample_rate == OUTPUT_SAMPLE_RATE && out.channels == OUTPUT_CHANNELS;
                const int actual_rate = out.sample_rate;
                const int actual_channels = out.channels;
                qt_audio_free(&out);
                if (!format_ok) {
                    post_status(hwnd_, L"Audio contract mismatch: backend returned " +
                        std::to_wstring(actual_rate) + L" Hz / " + std::to_wstring(actual_channels) +
                        L" channel(s), expected 24000 Hz mono.");
                    return false;
                }
            } else {
                std::wstring err = utf8_to_wide(qt_last_error());
                qt_audio_free(&out);
                post_status(hwnd_, L"WARNING - model loaded, but warmup synthesis failed: " + err +
                                   L". First Enter will retry the synthesis path.");
                return true;
            }
        }
        post_status(hwnd_, cfg_.warmup
            ? L"Ready - 24 kHz mono contract verified. Enter speaks, Shift+Enter adds a line, Esc stops."
            : L"Ready - warmup disabled (pinned backend contract: 24 kHz mono). Enter speaks, Esc stops.");
        return true;
    }

    void run() {
        if (!init()) return;
        for (;;) {
            Job job;
            VoiceProfile voice;
            bool do_voice_change = false;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return quit_ || voice_change_ || !queue_.empty(); });
                if (quit_) break;
                if (voice_change_) {
                    voice = std::move(pending_voice_);
                    voice_change_ = false;
                    do_voice_change = true;
                } else {
                    job = std::move(queue_.front());
                    queue_.pop_front();
                }
            }
            if (do_voice_change) {
                post_status(hwnd_, L"Loading voice: " + utf8_to_wide(voice.name) + L"...");
                if (switch_reference(voice)) post_status(hwnd_, L"Voice ready: " + utf8_to_wide(voice.name));
                else post_status(hwnd_, L"Voice switch failed; previous voice remains active.");
                continue;
            }
            if (job.generation != generation_.load(std::memory_order_acquire)) continue;

            qt_tts_params p{};
            qt_tts_default_params(&p);
            p.text = job.text.c_str();
            apply_common_params(p);

            CallbackCtx cb{};
            cb.self = this;
            cb.generation = job.generation;
            cb.started = std::chrono::steady_clock::now();
            // The pinned qwentts.cpp polls this callback at every Talker step
            // (~1 / 12.5 Hz, roughly 80 ms granularity).
            p.cancel = &Engine::cancel_cb;
            p.cancel_user_data = &cb;
            p.on_chunk = &Engine::chunk_cb;
            p.on_chunk_user_data = &cb;

            qt_audio out{};
            qt_status rc = qt_synthesize(q_, &p, &out);
            qt_audio_free(&out);
            audio_.cleanup();
            if (rc == QT_STATUS_CANCELLED) post_status(hwnd_, L"Stopped.");
            else if (rc != QT_STATUS_OK) post_status(hwnd_, L"Synthesis failed: " + utf8_to_wide(qt_last_error()));
            else post_status(hwnd_, L"Ready - type the next sentence and press Enter.");
        }
    }

public:
    Engine(Config cfg, HWND hwnd) : cfg_(std::move(cfg)), hwnd_(hwnd) {
        thread_ = std::thread([this] { run(); });
    }

    void enqueue(std::string text) {
        const uint64_t gen = generation_.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back(Job{std::move(text), gen});
        }
        cv_.notify_one();
        post_status(hwnd_, L"Queued.");
    }

    void cancel() {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.clear();
        }
        audio_.stop();
        post_status(hwnd_, L"Stopping - backend cancellation requested...");
    }

    void select_voice(VoiceProfile voice) {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.clear();
            pending_voice_ = std::move(voice);
            voice_change_ = true;
        }
        audio_.stop();
        cv_.notify_one();
    }

    ~Engine() {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(mu_);
            quit_ = true;
            queue_.clear();
        }
        cv_.notify_all();
        audio_.stop();
        if (thread_.joinable()) thread_.join();
        ref_.clear();
        if (q_) qt_free(q_);
    }
};

std::unique_ptr<Engine> g_engine;
std::vector<VoiceProfile> g_voices;
int g_selected_voice = 0;

void submit_text() {
    int len = GetWindowTextLengthW(g_edit);
    if (len <= 0) return;
    std::wstring text((size_t) len + 1, L'\0');
    GetWindowTextW(g_edit, text.data(), len + 1);
    text.resize((size_t) len);
    if (!has_nonspace(text)) return;
    SetWindowTextW(g_edit, L"");
    SetFocus(g_edit);
    if (g_engine) g_engine->enqueue(wide_to_utf8(text));
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HFONT font = (HFONT) GetStockObject(DEFAULT_GUI_FONT);
            HWND voice_label = CreateWindowW(L"STATIC", L"Voice:", WS_CHILD | WS_VISIBLE,
                                              16, 18, 44, 24, hwnd, nullptr, nullptr, nullptr);
            g_voice_combo = CreateWindowW(L"COMBOBOX", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          64, 14, 360, 300, hwnd, (HMENU)(INT_PTR) ID_VOICE, nullptr, nullptr);
            for (const VoiceProfile & v : g_voices) {
                std::wstring name = utf8_to_wide(v.name);
                SendMessageW(g_voice_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            }
            SendMessageW(g_voice_combo, CB_SETCURSEL, (WPARAM) g_selected_voice, 0);
            g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                     16, 52, 650, 250, hwnd, (HMENU)(INT_PTR) ID_EDIT, nullptr, nullptr);
            HWND speak = CreateWindowW(L"BUTTON", L"Speak", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       16, 316, 100, 34, hwnd, (HMENU)(INT_PTR) ID_SPEAK, nullptr, nullptr);
            HWND stop = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      126, 316, 100, 34, hwnd, (HMENU)(INT_PTR) ID_STOP, nullptr, nullptr);
            g_status = CreateWindowW(L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE,
                                     16, 362, 650, 34, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(voice_label, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(g_voice_combo, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(g_edit, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(speak, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(stop, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(g_status, WM_SETFONT, (WPARAM) font, TRUE);
            SetFocus(g_edit);
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(g_voice_combo, 64, 14, std::max(160, std::min(420, w - 80)), 300, TRUE);
            MoveWindow(g_edit, 16, 52, std::max(120, w - 32), std::max(80, h - 192), TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_SPEAK), 16, h - 124, 100, 34, TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_STOP), 126, h - 124, 100, 34, TRUE);
            MoveWindow(g_status, 16, h - 62, std::max(120, w - 32), 36, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            if (id == ID_SPEAK) submit_text();
            else if (id == ID_STOP && g_engine) g_engine->cancel();
            else if (id == ID_VOICE && code == CBN_SELCHANGE && g_voice_combo) {
                LRESULT idx = SendMessageW(g_voice_combo, CB_GETCURSEL, 0, 0);
                if (idx >= 0 && (size_t) idx < g_voices.size()) {
                    g_selected_voice = (int) idx;
                    const VoiceProfile & v = g_voices[(size_t) idx];
                    save_voice_id(v.id);
                    if (g_engine) g_engine->select_voice(v);
                    SetFocus(g_edit);
                }
            }
            return 0;
        }
        case WM_FAST_STATUS: {
            std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring *>(lp));
            if (s && g_status) SetWindowTextW(g_status, s->c_str());
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    Config cfg;
    if (!parse_args(cfg)) {
        MessageBoxW(nullptr,
                    L"Usage:\nfast-tts-voicebox.exe --model <qwen-talker-1.7b-base-Q8_0.gguf> --codec <qwen-tokenizer-12hz-Q8_0.gguf> [--voice-dir folder] [--lang English] [--ref-wav file.wav] [--ref-text transcript.txt] [--ref-spk voice.spk --ref-rvq voice.rvq]",
                    L"Fast TTS", MB_OK | MB_ICONINFORMATION);
        return 2;
    }

    if (cfg.voice_dir.empty()) {
        std::wstring user = get_env_w(L"USERPROFILE");
        if (!user.empty()) {
            std::string candidate = wide_to_utf8(user + L"\\Qwen3-TTS\\maintained-server-data\\voices-q8");
            if (file_exists_utf8(join_path_utf8(candidate, "migration-manifest.json"))) cfg.voice_dir = candidate;
        }
    }

    std::string manifest_warning;
    g_voices = discover_voices(cfg.voice_dir, manifest_warning);
    const bool command_line_voice = !cfg.ref_wav.empty() || !cfg.ref_spk.empty() || !cfg.ref_rvq.empty() || !cfg.ref_text_file.empty();
    if (command_line_voice) {
        VoiceProfile cli = profile_from_config(cfg, "__command_line", "Command-line voice");
        g_voices.insert(g_voices.begin() + std::min<size_t>(1, g_voices.size()), cli);
        g_selected_voice = 1;
    } else {
        std::string saved_id = wide_to_utf8(read_saved_voice_id());
        for (size_t i = 0; i < g_voices.size(); ++i) {
            if (g_voices[i].id == saved_id) {
                g_selected_voice = (int) i;
                break;
            }
        }
        if ((size_t) g_selected_voice < g_voices.size()) apply_profile_to_config(g_voices[(size_t) g_selected_voice], cfg);
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"FastTTSVoiceBox17";
    wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 1;

    g_main = CreateWindowW(wc.lpszClassName, L"Fast TTS - Qwen3-TTS 1.7B",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 720, 470,
                           nullptr, nullptr, instance, nullptr);
    if (!g_main) return 1;

    if (!manifest_warning.empty()) {
        MessageBoxW(g_main, utf8_to_wide(manifest_warning).c_str(), L"Fast TTS voice library", MB_OK | MB_ICONWARNING);
    }

    g_engine = std::make_unique<Engine>(cfg, g_main);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_edit && msg.hwnd == g_edit && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                if (g_engine) g_engine->cancel();
                continue;
            }
            if (msg.wParam == VK_RETURN && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
                submit_text();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_engine.reset();
    return (int) msg.wParam;
}
