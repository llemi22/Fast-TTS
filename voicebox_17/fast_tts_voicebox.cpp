#define WIN32_LEAN_AND_MEAN
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
constexpr int  ID_EDIT = 1001;
constexpr int  ID_SPEAK = 1002;
constexpr int  ID_STOP = 1003;
constexpr int  RVQ_CODE_BITS = 11;

HWND g_main = nullptr;
HWND g_edit = nullptr;
HWND g_status = nullptr;

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
    if (!PostMessageW(hwnd, WM_FAST_STATUS, 0, reinterpret_cast<LPARAM>(copy))) {
        delete copy;
    }
}

bool has_nonspace(const std::wstring & s) {
    for (wchar_t c : s) {
        if (!iswspace(c)) return true;
    }
    return false;
}

std::string read_text_file_utf8(const std::string & path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() >= 3 && (unsigned char) s[0] == 0xEF && (unsigned char) s[1] == 0xBB && (unsigned char) s[2] == 0xBF) {
        s.erase(0, 3);
    }
    return s;
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

struct Config {
    std::string model;
    std::string codec;
    std::string lang = "English";
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
        for (auto & b : active_) {
            waveOutUnprepareHeader(wave_, &b->hdr, sizeof(WAVEHDR));
        }
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

    bool load_reference() {
        if (!cfg_.ref_text_file.empty()) {
            ref_text_ = read_text_file_utf8(cfg_.ref_text_file);
            if (ref_text_.empty()) {
                post_status(hwnd_, L"Reference transcript could not be read; using x-vector-only clone if possible.");
            }
        }

        if (!cfg_.ref_spk.empty()) {
            if (!read_spk_file_local(cfg_.ref_spk, ref_spk_)) {
                post_status(hwnd_, L"Could not read --ref-spk.");
                return false;
            }
            if (!cfg_.ref_rvq.empty()) {
                int K = qt_num_codebooks(q_);
                if (!rvq_read_file(cfg_.ref_rvq.c_str(), K, RVQ_CODE_BITS, ref_codes_, &ref_T_)) {
                    post_status(hwnd_, L"Could not read --ref-rvq.");
                    return false;
                }
            }
            return true;
        }

        if (!cfg_.ref_wav.empty()) {
            int n = 0;
            float * raw = audio_read_mono(cfg_.ref_wav.c_str(), 24000, &n);
            if (!raw || n <= 0) {
                if (raw) std::free(raw);
                post_status(hwnd_, L"Could not decode --ref-wav.");
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
        if (!load_reference()) return false;

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
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
                if (quit_) break;
                job = std::move(queue_.front());
                queue_.pop_front();
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

            if (rc == QT_STATUS_CANCELLED) {
                post_status(hwnd_, L"Stopped.");
            } else if (rc != QT_STATUS_OK) {
                post_status(hwnd_, L"Synthesis failed: " + utf8_to_wide(qt_last_error()));
            } else {
                post_status(hwnd_, L"Ready - type the next sentence and press Enter.");
            }
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
        post_status(hwnd_, L"Stopping...");
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
        if (extracted_ref_.ref_spk_emb || extracted_ref_.ref_codes) qt_voice_ref_free(&extracted_ref_);
        if (q_) qt_free(q_);
    }
};

std::unique_ptr<Engine> g_engine;

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
            g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                     16, 16, 650, 250, hwnd, (HMENU) ID_EDIT, nullptr, nullptr);
            HWND speak = CreateWindowW(L"BUTTON", L"Speak", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                       16, 280, 100, 34, hwnd, (HMENU) ID_SPEAK, nullptr, nullptr);
            HWND stop = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      126, 280, 100, 34, hwnd, (HMENU) ID_STOP, nullptr, nullptr);
            g_status = CreateWindowW(L"STATIC", L"Starting...", WS_CHILD | WS_VISIBLE,
                                     16, 326, 650, 28, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(g_edit, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(speak, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(stop, WM_SETFONT, (WPARAM) font, TRUE);
            SendMessageW(g_status, WM_SETFONT, (WPARAM) font, TRUE);
            SetFocus(g_edit);
            return 0;
        }
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            MoveWindow(g_edit, 16, 16, std::max(120, w - 32), std::max(80, h - 140), TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_SPEAK), 16, h - 108, 100, 34, TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_STOP), 126, h - 108, 100, 34, TRUE);
            MoveWindow(g_status, 16, h - 62, std::max(120, w - 32), 32, TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == ID_SPEAK) submit_text();
            else if (LOWORD(wp) == ID_STOP && g_engine) g_engine->cancel();
            return 0;
        case WM_FAST_STATUS: {
            std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring *>(lp));
            if (s && g_status) SetWindowTextW(g_status, s->c_str());
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    Config cfg;
    if (!parse_args(cfg)) {
        MessageBoxW(nullptr,
                    L"Usage:\nfast-tts-voicebox.exe --model <qwen-talker-1.7b-base-Q8_0.gguf> --codec <qwen-tokenizer-12hz-Q8_0.gguf> [--lang English] [--ref-wav file.wav] [--ref-text transcript.txt] [--ref-spk voice.spk --ref-rvq voice.rvq]",
                    L"Fast TTS", MB_OK | MB_ICONINFORMATION);
        return 2;
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
                           CW_USEDEFAULT, CW_USEDEFAULT, 720, 430,
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
