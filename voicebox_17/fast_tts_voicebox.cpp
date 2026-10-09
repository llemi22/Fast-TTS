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
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <cstdlib>
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

std::string json_unescape(const std::string & s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        char c = s[++i];
        switch (c) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string json_string_field(const std::string & obj, const char * key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return {};
    p = obj.find(':', p + needle.size());
    if (p == std::string::npos) return {};
    p = obj.find('"', p + 1);
    if (p == std::string::npos) return {};
    ++p;
    std::string raw;
    bool escaped = false;
    for (; p < obj.size(); ++p) {
        char c = obj[p];
        if (!escaped && c == '"') break;
        if (!escaped && c == '\\') {
            escaped = true;
            raw.push_back(c);
            continue;
        }
        escaped = false;
        raw.push_back(c);
    }
    return json_unescape(raw);
}

std::vector<VoiceProfile> discover_voices(const std::string & dir) {
    std::vector<VoiceProfile> voices;
    voices.push_back(VoiceProfile{"", "Default / no clone", "", "", "", ""});
    if (dir.empty()) return voices;

    std::string manifest = read_text_file_utf8(join_path_utf8(dir, "migration-manifest.json"));
    if (manifest.empty()) return voices;

    size_t pos = 0;
    while ((pos = manifest.find('{', pos)) != std::string::npos) {
        size_t end = manifest.find('}', pos + 1);
        if (end == std::string::npos) break;
        std::string obj = manifest.substr(pos, end - pos + 1);
        pos = end + 1;

        VoiceProfile v;
        v.id = json_string_field(obj, "id");
        v.name = json_string_field(obj, "name");
        if (v.id.empty()) continue;
        if (v.name.empty()) v.name = v.id;

        std::string base = join_path_utf8(dir, v.id);
        std::string p;
        p = base + ".wav"; if (file_exists_utf8(p)) v.wav = p;
        p = base + ".txt"; if (file_exists_utf8(p)) v.text = p;
        p = base + ".spk"; if (file_exists_utf8(p)) v.spk = p;
        p = base + ".rvq"; if (file_exists_utf8(p)) v.rvq = p;

        if (!v.spk.empty() || !v.wav.empty()) voices.push_back(std::move(v));
    }
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
    VoiceProfile v;
    v.id = id;
    v.name = name;
    v.wav = cfg.ref_wav;
    v.text = cfg.ref_text_file;
    v.spk = cfg.ref_spk;
    v.rvq = cfg.ref_rvq;
    return v;
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
        std::atomic<bool> done{false};
    };

    HWAVEOUT wave_ = nullptr;
    std::mutex mu_;
    std::deque<std::unique_ptr<Buffer>> active_;

    static void CALLBACK wave_callback(HWAVEOUT, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR) {
        if (msg != WOM_DONE || !p1) return;
        auto * hdr = reinterpret_cast<WAVEHDR *>(p1);
        auto * b = reinterpret_cast<Buffer *>(hdr->dwUser);
        if (b) b->done.store(true, std::memory_order_release);
    }

    void cleanup_locked() {
        for (auto it = active_.begin(); it != active_.end();) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                waveOutUnprepareHeader(wave_, &(*it)->hdr, sizeof(WAVEHDR));
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
    }

public:
    bool open() {
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = 24000;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = (WORD) (fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        return waveOutOpen(&wave_, WAVE_MAPPER, &fmt,
                           reinterpret_cast<DWORD_PTR>(&AudioPlayer::wave_callback), 0,
                           CALLBACK_FUNCTION) == MMSYSERR_NOERROR;
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
        b->hdr.dwUser = reinterpret_cast<DWORD_PTR>(b.get());
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
        for (auto & b : active_) waveOutUnprepareHeader(wave_, &b->hdr, sizeof(WAVEHDR));
        active_.clear();
    }

    void cleanup() {
        std::lock_guard<std::mutex> lock(mu_);
        if (wave_) cleanup_locked();
    }

    ~AudioPlayer() {
        stop();
        if (wave_) waveOutClose(wave_);
    }
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
    qt_voice_ref extracted_ref_{};
    std::vector<float> ref_spk_;
    std::vector<int32_t> ref_codes_;
    int ref_T_ = 0;
    std::string ref_text_;
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
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - c->started).count();
            post_status(c->self->hwnd_, L"Speaking - first audio in " + std::to_wstring(ms) + L" ms");
        }
        return c->self->audio_.queue(samples, n_samples) && !cancel_cb(c);
    }

    void clear_reference() {
        if (extracted_ref_.ref_spk_emb || extracted_ref_.ref_codes) qt_voice_ref_free(&extracted_ref_);
        extracted_ref_ = {};
        ref_spk_.clear();
        ref_codes_.clear();
        ref_T_ = 0;
        ref_text_.clear();
    }

    bool load_reference(const VoiceProfile & voice) {
        clear_reference();
        if (!voice.text.empty()) {
            ref_text_ = read_text_file_utf8(voice.text);
            if (ref_text_.empty()) post_status(hwnd_, L"Reference transcript could not be read; using speaker-only clone.");
        }
        if (!voice.spk.empty()) {
            if (!read_spk_file_local(voice.spk, ref_spk_)) {
                post_status(hwnd_, L"Could not read cached speaker embedding for " + utf8_to_wide(voice.name) + L".");
                return false;
            }
            if (!voice.rvq.empty()) {
                int K = qt_num_codebooks(q_);
                if (!rvq_read_file(voice.rvq.c_str(), K, RVQ_CODE_BITS, ref_codes_, &ref_T_)) {
                    post_status(hwnd_, L"Could not read cached RVQ data for " + utf8_to_wide(voice.name) + L".");
                    return false;
                }
            }
            return true;
        }
        if (!voice.wav.empty()) {
            int n = 0;
            float * raw = audio_read_mono(voice.wav.c_str(), 24000, &n);
            if (!raw || n <= 0) {
                if (raw) std::free(raw);
                post_status(hwnd_, L"Could not decode reference WAV for " + utf8_to_wide(voice.name) + L".");
                return false;
            }
            qt_status rc = qt_extract_voice_ref(q_, raw, n, &extracted_ref_);
            std::free(raw);
            if (rc != QT_STATUS_OK) {
                post_status(hwnd_, L"Voice reference extraction failed: " + utf8_to_wide(qt_last_error()));
                return false;
            }
        }
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
        if (extracted_ref_.ref_spk_emb) {
            spk = extracted_ref_.ref_spk_emb;
            spk_dim = extracted_ref_.ref_spk_dim;
            if (!ref_text_.empty()) {
                codes = extracted_ref_.ref_codes;
                ref_T = extracted_ref_.ref_T;
            }
        } else if (!ref_spk_.empty()) {
            spk = ref_spk_.data();
            spk_dim = (int) ref_spk_.size();
            if (!ref_text_.empty() && !ref_codes_.empty()) {
                codes = ref_codes_.data();
                ref_T = ref_T_;
            }
        }
        p.ref_spk_emb = spk;
        p.ref_spk_dim = spk_dim;
        p.ref_codes = codes;
        p.ref_T = ref_T;
        p.ref_text = (codes && !ref_text_.empty()) ? ref_text_.c_str() : nullptr;
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
        if (!load_reference(profile_from_config(cfg_, "__initial", "initial voice"))) return false;
        if (cfg_.warmup) {
            post_status(hwnd_, L"Warming model...");
            qt_tts_params p{};
            qt_tts_default_params(&p);
            p.text = "Ready.";
            apply_common_params(p);
            p.max_new_tokens = std::min(8, cfg_.max_tokens);
            qt_audio out{};
            qt_status rc = qt_synthesize(q_, &p, &out);
            qt_audio_free(&out);
            if (rc != QT_STATUS_OK) {
                post_status(hwnd_, L"Ready (warmup skipped: " + utf8_to_wide(qt_last_error()) + L")");
                return true;
            }
        }
        post_status(hwnd_, L"Ready - Enter speaks, Shift+Enter adds a line, Esc stops.");
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
                if (load_reference(voice)) post_status(hwnd_, L"Voice ready: " + utf8_to_wide(voice.name));
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
    Engine(Config cfg, HWND hwnd) : cfg_(std::move(cfg)), hwnd_(hwnd) { thread_ = std::thread([this] { run(); }); }

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
        post_status(hwnd_, L"Stopping...");
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
        clear_reference();
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
                                     16, 362, 650, 28, hwnd, nullptr, nullptr, nullptr);
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
            MoveWindow(g_status, 16, h - 62, std::max(120, w - 32), 32, TRUE);
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

    g_voices = discover_voices(cfg.voice_dir);
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
