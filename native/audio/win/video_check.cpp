// SPDX-License-Identifier: GPL-3.0-only
// video_check.exe: CW4's local harness (camera PHASE2 §12 CW4). It drives the Windows VideoBackend directly (no
// engine) and stands in for the parts that are not there yet: the source DLL (a section owner that follows
// shm_protocol.h, and an in-process reader that decides what the DLL would deliver, with §4.6.3's checks) and the
// engine (feeds AUs of an Annex-B file at 30 fps, presents each clean picture at once, answers decoder events the
// way §4.5.6 says: PLI start/size -> restart from the file's IDR). Nothing here registers a COM class or creates a
// virtual camera: the registry seam (--reg) points the backend at test keys under HKLM\SOFTWARE\BeamerTest.
//
//   video_check unit                                 pure rules: display size, coded-offset UV copy, IDR scan, want
//                                                    validation, InstallGuid, placeholder render, app usage (fake
//                                                    HKCU tree), install states (test HKLM keys; needs elevation)
//   video_check setup --reg K --owner-sid S [--dll P] [--guid G] | clear --reg K       (elevated) test install keys
//   video_check standin --reg K [--version V] [--dll-version N] [--squat] [--foreign-owner] [--seconds N]
//        (elevated, session 0 is fine) owns Global\BeamerCamera.<guid>.Shm/.Frame like the source DLL and applies
//        the commands the harness writes into Global\BeamerCamera.<guid>.Ctl (streaming, want); --squat pre-creates
//        the .Demand event itself (a foreign owner), --version writes another layout version
//   video_check play FILE [--sw] [--seconds N] [--reg K] [--bands]  decode + present at 30 fps (helper side)
//   video_check scenario NAME --reg K [FILE...]      in-session acceptance scenarios (see run_scenario)
// Every result is one JSON line on stdout.
#include "video_backend_win.h"

#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <shellapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

using namespace beamer::audio;
using namespace beamer::audio::win;
namespace vcam = beamer::vcam;

namespace {

// ------------------------------------------------------------------ small utils
int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        g_fails++;
        fprintf(stdout, "{\"check\":%s,\"ok\":false}\n", json_str(what).c_str());
    }
}
double pct(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}
double now_ms() { return qpc_ms(qpc_now()); }
void sleep_ms(int ms) { Sleep(DWORD(ms)); }
double cpu_s() {
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    auto t = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return t(k) + t(u);
}

std::vector<uint8_t> read_file(const std::wstring& p) {
    std::vector<uint8_t> d;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    if (!d.empty() && fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    fclose(f);
    return d;
}

// Annex-B -> access units (a new AU starts at an AUD/SPS/PPS/SEI or a slice with first_mb == 0 after a VCL NAL).
std::vector<std::vector<uint8_t>> split_annexb(const std::vector<uint8_t>& d) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < d.size(); i++)
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            starts.push_back(i);
            i += 2;
        }
    std::vector<std::vector<uint8_t>> aus;
    std::vector<uint8_t> cur;
    bool has_vcl = false;
    for (size_t k = 0; k < starts.size(); k++) {
        size_t s = starts[k], e = k + 1 < starts.size() ? starts[k + 1] : d.size();
        size_t h = s + (d[s + 2] == 1 ? 3 : 4);
        if (h >= d.size()) break;
        int type = d[h] & 31;
        bool vcl = type == 1 || type == 5;
        bool first_mb0 = vcl && h + 1 < d.size() && (d[h + 1] & 0x80);
        if (has_vcl && (type == 9 || type == 7 || type == 8 || type == 6 || first_mb0)) {
            aus.push_back(cur);
            cur.clear();
            has_vcl = false;
        }
        cur.insert(cur.end(), d.begin() + long(s), d.begin() + long(e));
        if (vcl) has_vcl = true;
    }
    if (!cur.empty()) aus.push_back(cur);
    return aus;
}

std::wstring arg_after(const std::vector<std::wstring>& a, const wchar_t* name, const std::wstring& def = L"") {
    for (size_t i = 0; i + 1 < a.size(); i++)
        if (a[i] == name) return a[i + 1];
    return def;
}
bool has_arg(const std::vector<std::wstring>& a, const wchar_t* name) { return std::find(a.begin(), a.end(), name) != a.end(); }

// ------------------------------------------------------------------ the source stand-in: control block
// Written by the harness (helper side), applied by `standin` into the source fields of the real header.
struct Ctl {
    volatile LONG seq;              // +1 per command
    volatile LONG streaming;
    volatile LONG want_w, want_h, want_fps;
    volatile LONG quit;
    volatile LONG64 applied_qpc;    // when the stand-in applied the last command (Demand set right after)
    volatile LONG applied_seq;
};

std::wstring guid_of(const std::wstring& reg) {
    InstallInfo i = read_install(reg);
    return i.guid;
}
std::wstring obj(const std::wstring& guid, const wchar_t* sfx) { return L"Global\\BeamerCamera." + guid + sfx; }

// ------------------------------------------------------------------ the DLL's decision, emulated (§4.6.3/§4.6.4)
struct Delivery {
    double t = 0;
    std::string what;   // builtin | ph | live
    uint32_t picture = 0, w = 0, h = 0;
};
class Reader {
 public:
    explicit Reader(const std::wstring& guid) : guid_(guid) {}
    ~Reader() { stop(); }
    bool open() {
        map_ = OpenFileMappingW(FILE_MAP_READ, FALSE, obj(guid_, L".Shm").c_str());
        if (!map_) return false;
        base_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_READ, 0, 0, SIZE_T(vcam::kSectionBytes)));
        hdr_ = reinterpret_cast<const vcam::Header*>(base_);
        return base_ != nullptr;
    }
    void start() {
        run_ = true;
        th_ = std::thread([this] {
            timeBeginPeriod(1);
            double next = now_ms();
            while (run_) {
                deliver();
                next += 1000.0 / 30.0;
                double w = next - now_ms();
                if (w > 0) Sleep(DWORD(w));
                else next = now_ms();
            }
            timeEndPeriod(1);
        });
    }
    void stop() {
        run_ = false;
        if (th_.joinable()) th_.join();
    }
    // One delivery: what the source would hand the app now.
    double max_gap = 0, max_gap_at = 0, prev_t = 0;
    Delivery deliver(std::vector<uint8_t>* copy = nullptr) {
        std::lock_guard<std::mutex> g(mu_);
        Delivery d;
        d.t = now_ms();
        if (prev_t > 0 && d.t - prev_t > max_gap) {
            max_gap = d.t - prev_t;
            max_gap_at = d.t;
        }
        prev_t = d.t;
        const int64_t now = qpc_now();
        const int64_t beat = int64_t(hdr_->beat_qpc.load(std::memory_order_acquire));
        const uint32_t mode = hdr_->mode.load(std::memory_order_acquire);
        const uint32_t latest = hdr_->latest.load(std::memory_order_acquire);
        bool fresh = beat >= now - qpc_freq() / 2 && beat <= now + qpc_freq();
        uint32_t slot = 99;
        if (fresh && mode == vcam::kModeWaiting) slot = vcam::kLiveSlots;
        else if (fresh && (mode == vcam::kModeLive || mode == vcam::kModeHold) && latest < vcam::kLiveSlots) slot = latest;
        d.what = "builtin";
        if (slot != 99) {
            const vcam::Slot& s = hdr_->slots[slot];
            uint32_t s1 = s.seq.load(std::memory_order_acquire);
            uint32_t w = *reinterpret_cast<const volatile uint32_t*>(&s.w), h = *reinterpret_cast<const volatile uint32_t*>(&s.h),
                     st = *reinterpret_cast<const volatile uint32_t*>(&s.stride), pic = *reinterpret_cast<const volatile uint32_t*>(&s.picture);
            bool ok = !(s1 & 1) && !(w & 1) && !(h & 1) && w >= vcam::kMinW && w <= vcam::kMaxW && h >= vcam::kMinH && h <= vcam::kMaxH &&
                      st >= w && st <= 4096 && uint64_t(st) * h * 3 / 2 <= vcam::kSlotBytes;
            if (ok && copy) {
                copy->resize(size_t(st) * h * 3 / 2);
                memcpy(copy->data(), base_ + vcam::kDataOffset + uint64_t(slot) * vcam::kSlotBytes, copy->size());
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (ok && s.seq.load(std::memory_order_acquire) == s1) {
                d.what = slot == vcam::kLiveSlots ? "ph" : "live";
                d.picture = pic;
                d.w = w;
                d.h = h;
            } else {
                d.what = last_.what == "builtin" ? "builtin" : last_.what;  // torn: previous good picture
                d.picture = last_.picture;
                rejected_++;
            }
        }
        if (d.what != last_.what || d.picture != last_.picture) log_.push_back(d);
        last_ = d;
        count_[d.what]++;
        return d;
    }
    std::vector<Delivery> log() {
        std::lock_guard<std::mutex> g(mu_);
        return log_;
    }
    void clear() {
        std::lock_guard<std::mutex> g(mu_);
        log_.clear();
        count_.clear();
    }
    std::map<std::string, int> counts() {
        std::lock_guard<std::mutex> g(mu_);
        return count_;
    }
    // First change to `what` at or after t (ms), -1 if none.
    double first(const std::string& what, double t) {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& d : log_)
            if (d.t >= t && d.what == what) return d.t;
        return -1;
    }
    bool seen(const std::string& what, double t0, double t1) {
        std::lock_guard<std::mutex> g(mu_);
        for (size_t i = 0; i < log_.size(); i++) {
            double start = log_[i].t, end = i + 1 < log_.size() ? log_[i + 1].t : 1e300;
            if (log_[i].what == what && end > t0 && start < t1) return true;
        }
        return false;
    }
    const vcam::Header* hdr() const { return hdr_; }
    uint64_t rejected() const { return rejected_; }

 private:
    std::wstring guid_;
    HANDLE map_ = nullptr;
    uint8_t* base_ = nullptr;
    const vcam::Header* hdr_ = nullptr;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::mutex mu_;
    std::vector<Delivery> log_;
    std::map<std::string, int> count_;
    Delivery last_;
    uint64_t rejected_ = 0;
};

// Talks to the stand-in.
class SourceCtl {
 public:
    bool open(const std::wstring& guid) {
        map_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, obj(guid, L".Ctl").c_str());
        if (!map_) return false;
        c_ = static_cast<Ctl*>(MapViewOfFile(map_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Ctl)));
        ev_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, obj(guid, L".CtlEv").c_str());
        return c_ && ev_;
    }
    // Returns the QPC (ms) at which the stand-in applied it.
    double set(bool streaming, uint32_t w = 1280, uint32_t h = 720, uint32_t fps = 30) {
        c_->streaming = streaming;
        c_->want_w = LONG(w);
        c_->want_h = LONG(h);
        c_->want_fps = LONG(fps);
        LONG s = InterlockedIncrement(&c_->seq);
        SetEvent(ev_);
        for (int i = 0; i < 2000 && c_->applied_seq != s; i++) Sleep(1);
        return qpc_ms(c_->applied_qpc);
    }

 private:
    HANDLE map_ = nullptr, ev_ = nullptr;
    Ctl* c_ = nullptr;
};

// ------------------------------------------------------------------ engine stand-in
struct Events : VideoBackendListener, VideoSinkListener {
    std::mutex mu;
    std::vector<std::pair<double, std::string>> ev;  // (ms, "what:detail")
    std::vector<std::pair<double, bool>> vcam_changes;  // (ms, in_use)
    Vcam* vcam = nullptr;
    WinVideoBackend* backend = nullptr;
    // decode bookkeeping (on_decoded comes from the sink's decoder thread)
    std::atomic<uint32_t> decoding{0};
    std::atomic<int64_t> last_idr{-1};           // frame number of the newest IDR handed to the sink
    std::atomic<int> lagged{0}, ok_count{0}, fail_count{0};
    std::vector<double> dec_ms;                  // under mu
    std::map<uint32_t, bool> results;            // under mu
    std::condition_variable dcv;
    std::string slow;                            // feed() calls > 50 ms (decode slow): "t:ms:kind:frame"
    double last_call = 0, max_call_gap = 0, max_call_gap_at = 0;  // presenter: gaps between sink calls
    void called() {
        double t = now_ms();
        if (last_call > 0 && t - last_call > max_call_gap) {
            max_call_gap = t - last_call;
            max_call_gap_at = t;
        }
        last_call = t;
    }
    void cameras_changed() override {}
    void vcam_changed() override {
        double t = now_ms();
        VcamState s = backend ? backend->vcamref().state(false) : VcamState{};
        std::lock_guard<std::mutex> g(mu);
        vcam_changes.push_back({t, s.in_use});
    }
    void permission_changed() override {}
    void camera_lost(CameraHandle*, const std::string&) override {}
    void on_decoded(uint32_t frame, bool ok, double ms) override {
        if (frame != decoding.load()) lagged++;
        std::lock_guard<std::mutex> g(mu);
        if (ok) {
            ok_count++;
            dec_ms.push_back(ms);
        } else {
            fail_count++;
        }
        results[frame] = ok;
        dcv.notify_all();
    }
    void on_sink_event(const std::string& what, const std::string& detail) override {
        std::lock_guard<std::mutex> g(mu);
        // The engine's rule for a new decoder (R3 #2): ask for an IDR only when none is in the sink, i.e. no IDR
        // was handed whose on_decoded has not come yet (both arrive on this thread, in order).
        const int64_t li = last_idr.load();
        const bool idr_in_sink = what.rfind("decoder_", 0) == 0 && li >= 0 && !results.count(uint32_t(li));
        ev.push_back({now_ms(), what + ":" + detail + (idr_in_sink ? "|idr_in_sink" : "")});
    }
    std::vector<std::pair<double, std::string>> take() {
        std::lock_guard<std::mutex> g(mu);
        auto e = ev;
        ev.clear();
        return e;
    }
};

struct Stream {
    std::vector<std::vector<uint8_t>> aus;
    size_t idx = 0;
    uint32_t frame = 0;
    bool need_idr = false;
    int idrs_sent = 0;  // restarts from the file's IDR asked for by the engine stand-in (the first IDR not counted)
};

// Feeds one AU and, like the engine's presenter, keeps ticking hold() (every 33 ms) until its on_decoded arrives;
// presents it when clean. Returns true when presented.
bool feed(WinVideoSink* sink, Events& ev, Stream& s) {
    if (s.need_idr) {
        s.idx = 0;  // PLI start/size answered by the file's first AU (an IDR)
        s.need_idr = false;
        s.idrs_sent++;
    }
    const auto& au = s.aus[s.idx % s.aus.size()];
    s.idx++;
    uint32_t f = s.frame++;
    ev.decoding = f;
    if (au_has_idr(au.data(), au.size())) ev.last_idr = int64_t(f);
    double t0 = now_ms();
    sink->decode(au.data(), au.size(), f, true);  // returns at once (queued)
    bool ok = false, got = false;
    for (int tick = 0; tick < 150 && !got; tick++) {  // <= 5 s
        std::unique_lock<std::mutex> g(ev.mu);
        got = ev.dcv.wait_for(g, std::chrono::milliseconds(33), [&] { return ev.results.count(f) != 0; });
        if (got) ok = ev.results[f];
        g.unlock();
        if (!got) {
            sink->hold();  // the presenter's tick while nothing new is decoded
            ev.called();
        }
    }
    double dt = now_ms() - t0;
    if (dt > 50) {
        char b[96];
        snprintf(b, sizeof b, "%s\"%.0f:%.0f:%s:f%u\"", ev.slow.empty() ? "" : ",", t0, dt, sink->decoder_kind().c_str(), f);
        ev.slow += b;
    }
    if (ok) {
        sink->present(f);
        ev.called();
        return true;
    }
    sink->hold();
    ev.called();
    return false;
}

// ------------------------------------------------------------------ unit tests
void unit(const std::vector<std::wstring>& a) {
    // display size
    check(display_size_ok(1920, 1080), "1080p display ok");
    check(display_size_ok(1280, 720) && display_size_ok(640, 360) && display_size_ok(64, 64), "small sizes ok");
    check(!display_size_ok(1920, 1088), "coded 1088 is not a display size");
    check(!display_size_ok(2560, 1440) && !display_size_ok(1281, 720) && !display_size_ok(62, 64) && !display_size_ok(0, 0),
          "bad display sizes refused");
    // coded-offset UV copy: a 1920x1088 coded picture, pitch 2048, rows 1080..1087 poisoned
    {
        const int w = 1920, h = 1080, ch = 1088;
        const size_t pitch = 2048;
        std::vector<uint8_t> src(pitch * ch * 3 / 2, 0xEE);
        for (int y = 0; y < h; y++) memset(&src[pitch * y], (y * 7) % 200, w);
        for (int y = 0; y < h / 2; y++) memset(&src[pitch * ch + pitch * y], 0x40 + (y & 0x3F), w);
        std::vector<uint8_t> dst(size_t(w) * h * 3 / 2, 0);
        copy_nv12_display(src.data(), pitch, ch, w, h, dst.data(), w);
        bool ok = true;
        for (int y = 0; y < h && ok; y++) ok = dst[size_t(y) * w] == uint8_t((y * 7) % 200) && dst[size_t(y) * w + w - 1] == uint8_t((y * 7) % 200);
        for (int y = 0; y < h / 2 && ok; y++) ok = dst[size_t(w) * h + size_t(y) * w] == uint8_t(0x40 + (y & 0x3F));
        check(ok, "NV12 copy: Y rows 0..h-1, UV from pitch*coded_h");
        check(std::find(dst.begin(), dst.end(), 0xEE) == dst.end(), "NV12 copy: nothing of rows 1080-1087 copied");
        check(dst.size() == size_t(1920) * 1080 * 3 / 2 && dst.size() <= vcam::kSlotBytes, "1080p picture fits a slot");
        // R3 #11: the exact read extent, so a short decoder buffer is never copied
        check(nv12_bytes_read(pitch, ch, w, h) == pitch * ch + pitch * (h / 2 - 1) + w && nv12_bytes_read(pitch, ch, w, h) <= src.size(),
              "NV12 read extent (1080p, pitch 2048) inside the coded buffer");
        check(nv12_bytes_read(1280, 720, 1272, 720) == size_t(1280) * 720 + size_t(1280) * 359 + 1272, "NV12 read extent, cropped 1272 in 1280");
        check(nv12_bytes_read(1000, 720, 1272, 720) == SIZE_MAX && nv12_bytes_read(1280, 700, 1280, 720) == SIZE_MAX,
              "NV12 read extent: pitch < width or coded height < height refused");
    }
    // IDR scan
    {
        const uint8_t idr[] = {0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3, 0, 0, 1, 0x65, 0x88};
        const uint8_t p[] = {0, 0, 0, 1, 0x41, 0x9a, 0, 0, 1, 0x06, 5};
        check(au_has_idr(idr, sizeof idr) && !au_has_idr(p, sizeof p), "IDR scan");
    }
    // want validation
    {
        auto w = validate_want(1920, 1080, 30);
        check(w.w == 1920 && w.h == 1080, "want 1080p accepted");
        w = validate_want(640, 360, 30);
        check(w.w == 640 && w.h == 360, "want 360p accepted");
        w = validate_want(800, 600, 30);
        check(w.w == 1280 && w.h == 720, "want 800x600 -> 1280x720");
        w = validate_want(1920, 1080, 60);
        check(w.w == 1280 && w.h == 720, "want 1080p60 -> 1280x720");
        w = validate_want(0xFFFFFFFF, 0xFFFFFFFF, 0);
        check(w.w == 1280, "want garbage -> 1280x720");
    }
    check(install_guid_ok(L"{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}") && !install_guid_ok(L"..\\x") && !install_guid_ok(L"") &&
              !install_guid_ok(std::wstring(40, L'a')),
          "InstallGuid charset");
    // placeholder render
    {
        Placeholder p{"Waiting for alexandres-macbook-air", "MacBook Air Camera"};
        std::string sizes;
        for (auto t : vcam::kOffered) {
            std::vector<uint8_t> nv;
            std::string err;
            double t0 = now_ms();
            bool ok = render_placeholder(p, int(t.w), int(t.h), &nv, &err);
            double ms = now_ms() - t0;
            check(ok && nv.size() == size_t(t.w) * t.h * 3 / 2, "placeholder renders");
            int mn = 255, mx = 0;
            for (size_t i = 0; i < size_t(t.w) * t.h; i++) {
                mn = std::min<int>(mn, nv[i]);
                mx = std::max<int>(mx, nv[i]);
            }
            check(mn >= 16 && mx <= 235 && mx - mn > 100, "placeholder: limited range with text contrast");
            char b[96];
            snprintf(b, sizeof b, "%s\"%ux%u\":%.1f", sizes.empty() ? "" : ",", t.w, t.h, ms);
            sizes += b;
            if (has_arg(a, L"--dump")) {
                std::wstring f = L"ph_" + std::to_wstring(t.w) + L"x" + std::to_wstring(t.h) + L".nv12";
                FILE* o = _wfopen(f.c_str(), L"wb");
                if (o) {
                    fwrite(nv.data(), 1, nv.size(), o);
                    fclose(o);
                }
            }
        }
        printf("{\"placeholder_render_ms\":{%s}}\n", sizes.c_str());
    }
    // app usage on a fake HKCU tree
    {
        const std::wstring root = L"Software\\BeamerTest\\CW4\\webcam";
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\BeamerTest\\CW4");
        auto put = [&](const std::wstring& sub, uint64_t start, uint64_t stop) {
            HKEY k;
            RegCreateKeyExW(HKEY_CURRENT_USER, (root + L"\\" + sub).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
            RegSetValueExW(k, L"LastUsedTimeStart", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&start), 8);
            RegSetValueExW(k, L"LastUsedTimeStop", 0, REG_QWORD, reinterpret_cast<const BYTE*>(&stop), 8);
            RegCloseKey(k);
        };
        int n = -1;
        check(camera_app_in_use(root, &n).empty() && n == 0, "app usage: empty store");
        put(L"NonPackaged\\C:#Windows#System32#notepad.exe", 200, 100);
        std::string one = camera_app_in_use(root, &n);
        check(n == 1 && !one.empty(), "app usage: one desktop app");
        put(L"NonPackaged\\C:#Windows#System32#mspaint.exe", 50, 100);  // used before, stopped
        check(camera_app_in_use(root, &n) == one && n == 1, "app usage: stopped entries ignored");
        // a packaged app: the calculator is installed on every Windows 11
        put(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe", 300, 0);
        check(camera_app_in_use(root, &n).empty() && n == 2, "app usage: two in use -> no name");
        put(L"NonPackaged\\C:#Windows#System32#notepad.exe", 200, 300);
        std::string pk = camera_app_in_use(root, &n);
        check(n == 1 && !pk.empty() && pk.find('_') == std::string::npos, "app usage: packaged display name");
        printf("{\"app_desktop\":%s,\"app_packaged\":%s}\n", json_str(one).c_str(), json_str(pk).c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\BeamerTest\\CW4");
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\BeamerTest\\CW4");
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\BeamerTest");
    }
    // install states on test HKLM keys (elevated only)
    {
        const std::wstring base = L"SOFTWARE\\BeamerTest\\CW4unit";
        RegPaths p = reg_paths(base);
        HKEY k;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, base.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr) != ERROR_SUCCESS) {
            printf("{\"install_states\":\"skipped (not elevated)\"}\n");
        } else {
            RegCloseKey(k);
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::string seq;
            auto st = [&](const char* expect, const char* label) {
                InstallInfo i = read_install(base);
                seq += std::string(seq.empty() ? "" : ",") + "\"" + label + "\":" + json_str(i.install + (i.detail.empty() ? "" : " (" + i.detail + ")"));
                check(i.install == expect, label);
            };
            st("missing", "missing");
            auto setv = [&](const std::wstring& key, const wchar_t* name, const std::wstring& v) {
                HKEY kk;
                RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &kk, nullptr);
                RegSetValueExW(kk, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * 2));
                RegCloseKey(kk);
            };
            auto setd = [&](DWORD v) {
                HKEY kk;
                RegCreateKeyExW(HKEY_LOCAL_MACHINE, p.camera.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &kk, nullptr);
                RegSetValueExW(kk, L"DllVersion", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), 4);
                RegCloseKey(kk);
            };
            setv(p.clsid + L"\\InprocServer32", nullptr, self);
            st("broken", "clsid_only");
            setv(p.camera, L"InstallGuid", L"{00000000-1111-2222-3333-444444444444}");
            setv(p.camera, L"OwnerSid", L"S-1-5-21-1-2-3-1001");
            setv(p.camera, L"DllPath", self);
            setd(1);
            st("ok", "ok");
            setd(0);
            st("broken", "no_version");
            setd(1);
            setv(p.camera, L"DllPath", L"C:\\nope\\BeamerCameraSource.dll");
            st("broken", "path_mismatch");
            setv(p.clsid + L"\\InprocServer32", nullptr, L"C:\\nope\\BeamerCameraSource.dll");
            st("broken", "file_missing");
            setv(p.camera, L"DllPath", self);
            setv(p.clsid + L"\\InprocServer32", nullptr, self);
            setv(p.camera, L"OwnerSid", L"not a sid");
            st("broken", "bad_sid");
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, base.c_str());
            RegDeleteKeyExW(HKEY_LOCAL_MACHINE, base.c_str(), KEY_WOW64_64KEY, 0);
            printf("{\"install_states\":{%s}}\n", seq.c_str());
        }
    }
    printf("{\"unit\":\"%s\",\"failures\":%d}\n", g_fails ? "FAIL" : "ok", g_fails);
}

// ------------------------------------------------------------------ setup / clear test install keys (elevated)
int setup(const std::vector<std::wstring>& a) {
    std::wstring base = arg_after(a, L"--reg");
    if (base.empty()) return 2;
    RegPaths p = reg_paths(base);
    std::wstring dll = arg_after(a, L"--dll");
    if (dll.empty()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        dll = self;
    }
    std::wstring guid = arg_after(a, L"--guid", L"{C4C4C4C4-0000-4000-8000-00000000CA04}");
    std::wstring sid = arg_after(a, L"--owner-sid");
    auto setv = [&](const std::wstring& key, const wchar_t* name, const std::wstring& v) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr)) return false;
        bool ok = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * 2)) == 0;
        RegCloseKey(k);
        return ok;
    };
    DWORD ver = DWORD(vcam::kDllVersion);
    bool ok = setv(p.clsid + L"\\InprocServer32", nullptr, dll) && setv(p.camera, L"InstallGuid", guid) && setv(p.camera, L"OwnerSid", sid) &&
              setv(p.camera, L"DllPath", dll);
    HKEY k;
    if (ok && !RegCreateKeyExW(HKEY_LOCAL_MACHINE, p.camera.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr)) {
        RegSetValueExW(k, L"DllVersion", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&ver), 4);
        RegCloseKey(k);
    }
    InstallInfo i = read_install(base);
    printf("{\"setup\":%s,\"install\":%s,\"guid\":%s}\n", ok ? "true" : "false", json_str(i.install).c_str(), json_str(utf8(i.guid)).c_str());
    return ok ? 0 : 4;
}
int clear(const std::vector<std::wstring>& a) {
    std::wstring base = arg_after(a, L"--reg");
    if (base.empty()) return 2;
    LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, base.c_str());
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, base.c_str(), KEY_WOW64_64KEY, 0);
    HKEY k;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\BeamerTest", 0, KEY_READ | KEY_WOW64_64KEY, &k)) {
        DWORD n = 0;
        RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &n, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(k);
        if (!n) RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\BeamerTest", KEY_WOW64_64KEY, 0);
    }
    printf("{\"clear\":%ld}\n", r);
    return 0;
}

// ------------------------------------------------------------------ the source stand-in (section owner)
int standin(const std::vector<std::wstring>& a) {
    std::wstring base = arg_after(a, L"--reg");
    InstallInfo i = read_install(base);
    if (!install_guid_ok(i.guid)) {
        printf("{\"standin\":false,\"error\":\"no test install keys\"}\n");
        return 4;
    }
    const uint32_t version = uint32_t(_wtoi(arg_after(a, L"--version", L"1").c_str()));
    const uint32_t dllv = uint32_t(_wtoi(arg_after(a, L"--dll-version", L"1").c_str()));
    const double seconds = _wtof(arg_after(a, L"--seconds", L"600").c_str());
    // Owner SYSTEM, like the DLL's objects (LocalService); the helper refuses any other owner (R3 #7). Setting an
    // owner other than oneself needs SeRestorePrivilege (elevated). --foreign-owner keeps this process's default
    // owner (Administrators): a squatter.
    const bool foreign = has_arg(a, L"--foreign-owner");
    if (!foreign) {
        HANDLE tok = nullptr;
        TOKEN_PRIVILEGES tp{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &tok) && LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        }
        if (tok) CloseHandle(tok);
    }
    std::wstring sddl = std::wstring(foreign ? L"" : L"O:SY") + L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GA;;;" + i.owner_sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    HANDLE squat = nullptr;
    if (has_arg(a, L"--squat")) {
        // A foreign .Demand: created first, by this (elevated: owner Administrators) process, open to everyone.
        PSECURITY_DESCRIPTOR osd = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &osd, nullptr);
        SECURITY_ATTRIBUTES osa{sizeof osa, osd, FALSE};
        squat = CreateEventW(&osa, FALSE, FALSE, obj(i.guid, L".Demand").c_str());
    }
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(vcam::kSectionBytes >> 32), DWORD(vcam::kSectionBytes),
                                  obj(i.guid, L".Shm").c_str());
    if (!m) {
        printf("{\"standin\":false,\"error\":\"section %lu\"}\n", GetLastError());
        return 4;
    }
    auto* h = static_cast<vcam::Header*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(vcam::kSectionBytes)));
    HANDLE frame = CreateEventW(&sa, FALSE, FALSE, obj(i.guid, L".Frame").c_str());
    HANDLE cm = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 4096, obj(i.guid, L".Ctl").c_str());
    auto* c = static_cast<Ctl*>(MapViewOfFile(cm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ctl)));
    HANDLE cev = CreateEventW(&sa, FALSE, FALSE, obj(i.guid, L".CtlEv").c_str());
    h->magic = vcam::kMagic;
    h->version = version;
    h->header_bytes = uint32_t(sizeof(vcam::Header));
    h->slot_count = vcam::kSlotCount;
    h->dll_version = dllv;
    h->source_pid = GetCurrentProcessId();
    h->want_w = 1280;
    h->want_h = 720;
    h->want_fps = 30;
    h->state_seq.store(1);
    printf("{\"standin\":true,\"guid\":%s,\"version\":%u,\"squat\":%s}\n", json_str(utf8(i.guid)).c_str(), version, squat ? "true" : "false");
    fflush(stdout);
    LONG seen = 0;
    const double end = now_ms() + seconds * 1000;
    while (now_ms() < end && !c->quit) {
        WaitForSingleObject(cev, 100);
        if (c->seq == seen) continue;
        seen = c->seq;
        h->streaming = uint32_t(c->streaming);
        h->want_w = uint32_t(c->want_w);
        h->want_h = uint32_t(c->want_h);
        h->want_fps = uint32_t(c->want_fps);
        h->state_qpc = uint64_t(qpc_now());
        h->state_seq.fetch_add(1);
        c->applied_qpc = qpc_now();
        c->applied_seq = seen;
        // As the DLL: the Demand event is the helper's; open it each time (it may come and go with the helper).
        HANDLE d = OpenEventW(EVENT_MODIFY_STATE, FALSE, obj(i.guid, L".Demand").c_str());
        if (d) {
            SetEvent(d);
            CloseHandle(d);
        }
    }
    (void)frame;
    printf("{\"standin\":\"exit\"}\n");
    return 0;
}

// ------------------------------------------------------------------ helper-side runs
struct Rig {
    WinVideoOptions opt;
    std::unique_ptr<WinVideoBackend> backend;
    Events ev;
    std::unique_ptr<Reader> reader;
    SourceCtl ctl;
    bool have_ctl = false;
    std::wstring guid;
    explicit Rig(const std::vector<std::wstring>& a) {
        opt.reg_base = arg_after(a, L"--reg");
        opt.create_camera = false;
        opt.dev = true;
        if (has_arg(a, L"--sw")) opt.decoder = "sw";
        backend = std::make_unique<WinVideoBackend>(opt);
        ev.backend = backend.get();
        guid = guid_of(opt.reg_base);
        have_ctl = !guid.empty() && ctl.open(guid);
        backend->set_listener(&ev);
        for (int i = 0; i < 40; i++) {  // the vcam loop opens the section within ~1 s
            reader = std::make_unique<Reader>(guid);
            if (!guid.empty() && reader->open()) break;
            reader.reset();
            sleep_ms(50);
        }
        if (reader) reader->start();
    }
    ~Rig() {
        if (reader) reader->stop();
    }
};

// Plays FILE through a sink at 30 fps for `seconds`; returns a JSON fragment of the numbers.
std::string play(Rig& rig, const std::wstring& file, double seconds, const std::function<void(double, WinVideoSink*)>& each = nullptr,
                 bool restart_on_fail = true, int* size_rejects = nullptr, bool stop_after_3_rejects = false) {
    Stream s;
    s.aus = split_annexb(read_file(file));
    if (s.aus.empty()) return "\"error\":\"no AUs\"";
    std::string err;
    auto h = rig.backend->open_sink(&rig.ev, &err);
    auto* sink = dynamic_cast<WinVideoSink*>(h.get());
    if (!sink) return "\"error\":" + json_str(err);
    sink->show_placeholder("starting");
    timeBeginPeriod(1);
    double t0 = now_ms(), next = t0, cpu0 = cpu_s();
    int presented = 0, rejects = 0;
    std::vector<double> reject_t;
    std::string kinds;
    bool stopped = false;
    while (now_ms() - t0 < seconds * 1000) {
        for (auto& e : rig.ev.take()) {
            const std::string& w = e.second;
            if (w.rfind("decoder_", 0) == 0) {
                // engine: a new decoder gets an IDR (PLI start), unless the sink holds one for it (R3 #2)
                if (w.find("|idr_in_sink") == std::string::npos) s.need_idr = true;
                kinds += std::string(kinds.empty() ? "" : ",") + json_str(w.substr(0, w.find(':')) + "@" + std::to_string(int(e.first - t0)));
            } else if (w.rfind("size_rejected", 0) == 0) {
                rejects++;
                reject_t.push_back(e.first);
                s.need_idr = true;  // PLI size
                if (stop_after_3_rejects && rejects >= 3 && e.first - reject_t[reject_t.size() - 3] <= 10000) {
                    sink->show_placeholder("error");  // engine: no loop, error placeholder, stream fails
                    stopped = true;
                }
            }
        }
        if (stopped) break;
        int fails = rig.ev.fail_count;
        if (!s.need_idr || true) {
            if (feed(sink, rig.ev, s)) presented++;
        }
        if (restart_on_fail && rig.ev.fail_count > fails && sink->decoder_kind() != "") s.need_idr = true;  // PLI loss/decoder
        if (each) each(now_ms() - t0, sink);
        next += 1000.0 / 30.0;
        double w = next - now_ms();
        if (w > 0) Sleep(DWORD(w));
        else next = now_ms();
    }
    double wall = (now_ms() - t0) / 1000.0, cpu = cpu_s() - cpu0;
    timeEndPeriod(1);
    if (size_rejects) *size_rejects = rejects;
    SinkCounters c = sink->counters();
    char b[4096];
    snprintf(b, sizeof b,
             "\"decoder\":\"%s\",\"aus\":%zu,\"fed\":%u,\"decoded_ok\":%d,\"presented\":%d,\"dec_ms_p50\":%.2f,\"dec_ms_p95\":%.2f,"
             "\"dec_ms_max\":%.2f,\"decoder_lag_frames\":%d,\"backlogged\":%llu,\"size_rejected_events\":%d,\"discarded\":%llu,\"errors\":%llu,\"device_lost\":%llu,"
             "\"skipped\":%llu,\"cpu_pct_core\":%.1f,\"idr_restarts\":%d,\"decoder_events\":[%s],\"output_type\":\"%dx%d coded h %d\",\"slow_calls\":[%s]",
             sink->decoder_kind().c_str(), s.aus.size(), s.frame, rig.ev.ok_count.load(), presented, pct(rig.ev.dec_ms, 0.5), pct(rig.ev.dec_ms, 0.95),
             pct(rig.ev.dec_ms, 1.0), rig.ev.lagged.load(), (unsigned long long)c.backlogged, rejects, (unsigned long long)c.discarded, (unsigned long long)c.decode_errors,
             (unsigned long long)c.device_lost, (unsigned long long)c.skipped, cpu / wall * 100.0, s.idrs_sent, kinds.c_str(), c.w, c.h, c.coded_h, rig.ev.slow.c_str());
    if (stopped) {
        // keep the sink open a moment with the error placeholder, beating, as the engine would until close
        for (int i = 0; i < 10; i++) {
            sink->hold();
            sleep_ms(33);
        }
        return std::string(b) + ",\"shown_after_error\":" + json_str(rig.backend->vcamref().shown());
    }
    return b;
}

// Band test picture (ffmpeg geq, see the report): band b = row/40; Y = b==26 ? 210 : 40 + (b*37 % 160);
// Cb = 128 + 60 sin(b), Cr = 128 + 60 cos(b). Checks rows 3..36 of every band (away from compression edges) in
// columns 0..799, and the last 8 rows.
std::string check_bands(const std::vector<uint8_t>& nv, int w, int h, int* bad_y, int* bad_uv, int* bottom_bad) {
    *bad_y = *bad_uv = *bottom_bad = 0;
    auto ey = [](int b) { return b == 26 ? 210 : 40 + (b * 37) % 160; };
    for (int y = 0; y < h; y++) {
        int b = y / 40, r = y % 40;
        if (r < 3 || r > 36) continue;
        for (int x = 0; x < 800; x += 16) {
            if (std::abs(int(nv[size_t(y) * w + x]) - ey(b)) > 8) (*bad_y)++;
        }
    }
    for (int y = 1072; y < 1080 && y < h; y++)
        for (int x = 0; x < 800; x += 16)
            if (std::abs(int(nv[size_t(y) * w + x]) - 210) > 8) (*bottom_bad)++;
    const uint8_t* uv = nv.data() + size_t(w) * h;
    for (int cy = 0; cy < h / 2; cy++) {
        int y = cy * 2, b = y / 40, r = y % 40;
        if (r < 4 || r > 35) continue;
        int ecb = int(std::lround(128 + 60 * std::sin(double(b)))), ecr = int(std::lround(128 + 60 * std::cos(double(b))));
        for (int x = 0; x < 800; x += 16) {
            if (std::abs(int(uv[size_t(cy) * w + x]) - ecb) > 8 || std::abs(int(uv[size_t(cy) * w + x + 1]) - ecr) > 8) (*bad_uv)++;
        }
    }
    char buf[128];
    snprintf(buf, sizeof buf, "\"bad_y\":%d,\"bad_uv\":%d,\"bottom_rows_bad\":%d", *bad_y, *bad_uv, *bottom_bad);
    return buf;
}

int cmd_play(const std::vector<std::wstring>& a) {
    if (has_arg(a, L"--negative")) H264Decoder::test_uv_at_display_height = true;
    Rig rig(a);
    std::wstring file = a.size() > 2 ? a[2] : L"";
    double secs = _wtof(arg_after(a, L"--seconds", L"20").c_str());
    if (rig.have_ctl) rig.ctl.set(true, 1920, 1080);
    std::vector<uint8_t> copy;
    Delivery last;
    int band_checks = 0, bad_y = 0, bad_uv = 0, bottom = 0, wrong_size = 0;
    std::string r = play(rig, file, secs, [&](double t, WinVideoSink*) {
        if (!rig.reader || t < 1000) return;
        if (int(t) % 1000 < 34) {  // once a second: what the source would copy
            Delivery d = rig.reader->deliver(&copy);
            if (d.what == "live") {
                if (has_arg(a, L"--bands")) {
                    int y, uv, bt;
                    check_bands(copy, int(d.w), int(d.h), &y, &uv, &bt);
                    band_checks++;
                    bad_y += y;
                    bad_uv += uv;
                    bottom += bt;
                }
                if (has_arg(a, L"--expect")) {
                    std::wstring e = arg_after(a, L"--expect");
                    if (std::to_wstring(d.w) + L"x" + std::to_wstring(d.h) != e) wrong_size++;
                }
                last = d;
            }
        }
    });
    auto counts = rig.reader ? rig.reader->counts() : std::map<std::string, int>{};
    printf("{\"play\":%s,%s,\"slot\":\"%ux%u\",\"slot_wrong_size\":%d,\"band_checks\":%d,\"bad_y\":%d,\"bad_uv\":%d,\"bottom_rows_bad\":%d,"
           "\"source_saw\":{\"live\":%d,\"ph\":%d,\"builtin\":%d},\"presenter_max_gap_ms\":%.0f,\"presenter_gap_at\":%.0f,"
           "\"reader_max_gap_ms\":%.0f,\"reader_gap_at\":%.0f}\n",
           json_str(utf8(file)).c_str(), r.c_str(), last.w, last.h, wrong_size, band_checks, bad_y, bad_uv, bottom, counts["live"], counts["ph"],
           counts["builtin"], rig.ev.max_call_gap, rig.ev.max_call_gap_at, rig.reader ? rig.reader->max_gap : -1, rig.reader ? rig.reader->max_gap_at : -1);
    if (rig.have_ctl) rig.ctl.set(false);
    return 0;
}

// ------------------------------------------------------------------ scenarios
int scenario(const std::vector<std::wstring>& a) {
    std::wstring name = a.size() > 2 ? a[2] : L"";
    Rig rig(a);
    if (!rig.reader || !rig.have_ctl) {
        printf("{\"scenario\":%s,\"error\":\"stand-in not running (section/ctl not found)\"}\n", json_str(utf8(name)).c_str());
        return 4;
    }
    std::wstring file = arg_after(a, L"--file");
    auto& R = *rig.reader;
    if (name == L"demand") {
        // in_use on/off latency, want validation, loaded_version
        sleep_ms(300);
        std::vector<double> on_ms, off_ms;
        std::string wants;
        for (int i = 0; i < 10; i++) {
            {
                std::lock_guard<std::mutex> g(rig.ev.mu);
                rig.ev.vcam_changes.clear();
            }
            uint32_t ww = i % 3 == 0 ? 1920 : i % 3 == 1 ? 800 : 640, wh = i % 3 == 0 ? 1080 : i % 3 == 1 ? 600 : 360;
            double t_on = rig.ctl.set(true, ww, wh);
            double seen = -1;
            for (int k = 0; k < 200 && seen < 0; k++) {
                sleep_ms(2);
                std::lock_guard<std::mutex> g(rig.ev.mu);
                for (auto& c : rig.ev.vcam_changes)
                    if (c.second) seen = c.first;
            }
            on_ms.push_back(seen < 0 ? 9999 : seen - t_on);
            VcamState st = rig.backend->vcam();
            if (i < 3) wants += std::string(wants.empty() ? "" : ",") + "\"" + std::to_string(ww) + "x" + std::to_string(wh) + "->" +
                                std::to_string(st.want.w) + "x" + std::to_string(st.want.h) + "\"";
            sleep_ms(200);
            {
                std::lock_guard<std::mutex> g(rig.ev.mu);
                rig.ev.vcam_changes.clear();
            }
            double t_off = rig.ctl.set(false);
            seen = -1;
            for (int k = 0; k < 400 && seen < 0; k++) {
                sleep_ms(2);
                std::lock_guard<std::mutex> g(rig.ev.mu);
                for (auto& c : rig.ev.vcam_changes)
                    if (!c.second) seen = c.first;
            }
            off_ms.push_back(seen < 0 ? 9999 : seen - t_off);
            // stop -> start blips shorter than 300 ms do not turn in_use off (Frame Server re-creates the source)
        }
        {
            std::lock_guard<std::mutex> g(rig.ev.mu);
            rig.ev.vcam_changes.clear();
        }
        rig.ctl.set(true);
        sleep_ms(100);
        rig.ctl.set(false);
        sleep_ms(100);
        rig.ctl.set(true);
        sleep_ms(500);
        int offs = 0;
        {
            std::lock_guard<std::mutex> g(rig.ev.mu);
            for (auto& c : rig.ev.vcam_changes)
                if (!c.second) offs++;
        }
        VcamState st = rig.backend->vcam();
        rig.ctl.set(false);
        printf("{\"scenario\":\"demand\",\"in_use_on_ms_p50\":%.2f,\"in_use_on_ms_max\":%.2f,\"in_use_off_ms_p50\":%.1f,\"in_use_off_ms_min\":%.1f,"
               "\"in_use_off_ms_max\":%.1f,\"blip_100ms_off_events\":%d,\"wants\":[%s],\"loaded_version\":%d,\"install\":%s,\"error\":%s}\n",
               pct(on_ms, 0.5), pct(on_ms, 1.0), pct(off_ms, 0.5), pct(off_ms, 0.0), pct(off_ms, 1.0), offs, wants.c_str(), st.loaded_version,
               json_str(st.install).c_str(), json_str(st.error).c_str());
        return 0;
    }
    if (name == L"status") {  // install / loaded_version / error as the backend reports them (stand-in variants)
        sleep_ms(1500);
        rig.ctl.set(true, 1920, 1080);
        sleep_ms(1300);
        VcamState st = rig.backend->vcam();
        printf("{\"scenario\":\"status\",\"vcam\":%s}\n", vcam_event_json(st).c_str());
        rig.ctl.set(false);
        return 0;
    }
    if (name == L"states") {
        rig.ctl.set(true, 1280, 720);
        std::map<std::string, Placeholder> texts = {{"waiting", {"Waiting for alexandres-macbook-air", "MacBook Air Camera"}},
                                                    {"sharing_off", {"Turned off on alexandres-macbook-air", ""}},
                                                    {"starting", {"Starting the camera on alexandres-macbook-air", ""}},
                                                    {"reconnecting", {"Reconnecting to alexandres-macbook-air", ""}}};
        rig.backend->vcam_placeholders(texts, "waiting");
        sleep_ms(500);
        // 1. idle placeholder change -> on screen
        std::vector<double> ph_ms;
        for (int i = 0; i < 6; i++) {
            uint32_t before = R.deliver().picture;
            double t = now_ms();
            rig.backend->vcam_placeholders(texts, i % 2 ? "waiting" : "sharing_off");
            double seen = -1;
            for (int k = 0; k < 300 && seen < 0; k++) {
                Delivery d = R.deliver();
                if (d.what == "ph" && d.picture != before) seen = now_ms() - t;
                else sleep_ms(1);
            }
            ph_ms.push_back(seen < 0 ? 9999 : seen);
            sleep_ms(200);
        }
        // the source delivers at 30 fps, so on screen = written + up to one frame period
        // 2. open_sink -> starting; live; stall -> hold; 2 s -> reconnecting (engine-driven); live again
        Stream s;
        s.aus = split_annexb(read_file(file));
        std::string err;
        R.clear();
        double t_open = now_ms();
        auto h = rig.backend->open_sink(&rig.ev, &err);
        auto* sink = dynamic_cast<WinVideoSink*>(h.get());
        std::string shown_open = rig.backend->vcamref().shown();
        while (sink && sink->decoder_kind().empty() && now_ms() - t_open < 3000) {
            sink->hold();
            sleep_ms(10);
        }
        rig.ev.take();
        s.need_idr = true;
        double t_live = -1;
        for (int i = 0; i < 60; i++) {
            if (feed(sink, rig.ev, s) && t_live < 0) t_live = now_ms();
            sleep_ms(33);
        }
        // stall: the engine keeps calling hold() each tick; after 2 s it asks for "reconnecting"
        double t_stall = now_ms();
        std::string shown_stall;
        double t_recon = -1;
        while (now_ms() - t_stall < 2600) {
            sink->hold();
            if (shown_stall.empty() && now_ms() - t_stall > 300) shown_stall = rig.backend->vcamref().shown();
            if (t_recon < 0 && now_ms() - t_stall >= 2000) {
                sink->show_placeholder("reconnecting");
                t_recon = now_ms();
            }
            sleep_ms(33);
        }
        std::string shown_recon = rig.backend->vcamref().shown();
        bool builtin_during_stall = R.seen("builtin", t_stall, now_ms());
        bool ph_during_hold = R.seen("ph", t_stall, t_recon - 1);
        double recon_on_screen = R.first("ph", t_recon);
        // back live (the next clean picture), then close: hold 500 ms, then the idle placeholder
        s.need_idr = true;
        for (int i = 0; i < 30; i++) {
            feed(sink, rig.ev, s);
            sleep_ms(33);
        }
        R.clear();
        double t_close = now_ms();
        h.reset();
        sleep_ms(1000);
        double idle_at = R.first("ph", t_close);
        bool builtin_after_close = R.seen("builtin", t_close, now_ms());
        // 3. close, then a new sink presenting within 500 ms: no placeholder in between
        auto h2 = rig.backend->open_sink(&rig.ev, &err);
        auto* sink2 = dynamic_cast<WinVideoSink*>(h2.get());
        while (sink2 && sink2->decoder_kind().empty()) {
            sink2->hold();
            sleep_ms(10);
        }
        rig.ev.take();
        s.need_idr = true;
        for (int i = 0; i < 30; i++) {
            feed(sink2, rig.ev, s);
            sleep_ms(33);
        }
        R.clear();
        double t_close2 = now_ms();
        h2.reset();
        sleep_ms(250);
        auto h3 = rig.backend->open_sink(&rig.ev, &err);
        auto* sink3 = dynamic_cast<WinVideoSink*>(h3.get());
        sink3->show_placeholder("starting");
        double t3 = now_ms();
        while (sink3->decoder_kind().empty() && now_ms() - t3 < 1000) {
            sink3->hold();
            sleep_ms(10);
        }
        rig.ev.take();
        s.need_idr = true;
        double t_live3 = -1;
        double t_last_call = 0;
        for (int i = 0; i < 20; i++) {
            if (feed(sink3, rig.ev, s) && t_live3 < 0) t_live3 = now_ms();
            t_last_call = now_ms();
            sleep_ms(33);
        }
        bool ph_between = R.seen("ph", t_close2, t_live3 > 0 ? t_live3 : now_ms());
        bool builtin_between = R.seen("builtin", t_close2, now_ms());
        // 4. a wedged presenter: nothing calls the sink -> the DLL's built-in image after 500 ms
        R.clear();
        double t_wedge = t_last_call;  // the presenter's last call (its last beat)
        sleep_ms(1500);
        double builtin_at = R.first("builtin", t_wedge);
        for (int i = 0; i < 10; i++) {
            feed(sink3, rig.ev, s);
            sleep_ms(33);
        }
        h3.reset();
        rig.ctl.set(false);
        printf("{\"scenario\":\"states\",\"idle_change_on_screen_ms\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],\"shown_at_open\":%s,"
               "\"first_live_after_open_ms\":%.0f,\"shown_during_stall\":%s,\"placeholder_before_2s\":%s,\"builtin_during_stall\":%s,"
               "\"reconnecting_on_screen_ms\":%.1f,\"shown_after_2s\":%s,\"close_hold_then_idle_ms\":%.1f,\"builtin_after_close\":%s,"
               "\"reopen_250ms_placeholder_seen\":%s,\"reopen_builtin_seen\":%s,\"wedge_builtin_after_ms\":%.1f}\n",
               ph_ms[0], ph_ms[1], ph_ms[2], ph_ms[3], ph_ms[4], ph_ms[5], json_str(shown_open).c_str(), t_live < 0 ? -1 : t_live - t_open,
               json_str(shown_stall).c_str(), ph_during_hold ? "true" : "false", builtin_during_stall ? "true" : "false",
               recon_on_screen < 0 ? -1 : recon_on_screen - t_recon, json_str(shown_recon).c_str(), idle_at < 0 ? -1 : idle_at - t_close,
               builtin_after_close ? "true" : "false", ph_between ? "true" : "false", builtin_between ? "true" : "false",
               builtin_at < 0 ? -1 : builtin_at - t_wedge);
        return 0;
    }
    if (name == L"reject") {
        rig.ctl.set(true, 1280, 720);
        sleep_ms(300);
        R.clear();
        double t0 = now_ms();
        int rejects = 0;
        std::string r = play(rig, file, 8, nullptr, false, &rejects, true);
        bool live_seen = R.seen("live", t0, now_ms());
        auto st = rig.backend->vcamref().shown();
        rig.ctl.set(false);
        printf("{\"scenario\":\"reject\",%s,\"live_slot_written\":%s,\"shown_at_end\":%s}\n", r.c_str(), live_seen ? "true" : "false",
               json_str(st).c_str());
        return 0;
    }
    if (name == L"devloss") {
        rig.ctl.set(true, 1280, 720);
        sleep_ms(300);
        R.clear();
        double t_loss = -1, t_sw = -1, t_hw_ready = -1, t_hw_back = -1;
        int ok_at_loss = 0;
        const double total = _wtof(arg_after(a, L"--seconds", L"45").c_str());
        std::string r = play(rig, file, total, [&](double t, WinVideoSink* sink) {
            if (t_loss < 0 && t > 3000 && sink->decoder_kind() == "hw") {
                sink->simulate_device_removed();
                t_loss = now_ms();
                printf("{\"devloss_at\":%.0f}\n", t_loss);
                ok_at_loss = rig.ev.ok_count;
            }
            if (t_loss > 0 && t_sw < 0 && sink->decoder_kind() == "sw" && rig.ev.ok_count > ok_at_loss + 1) t_sw = now_ms();
            if (t_sw > 0 && t_hw_back < 0 && sink->decoder_kind() == "hw") t_hw_back = now_ms();
        });
        // decoder_hw (standby) event time
        bool builtin = R.seen("builtin", 0, now_ms());
        auto counts = R.counts();
        rig.ctl.set(false);
        (void)t_hw_ready;
        printf("{\"scenario\":\"devloss\",%s,\"sw_picture_after_loss_ms\":%.0f,\"hw_back_after_loss_ms\":%.0f,\"builtin_seen\":%s,"
               "\"source_saw\":{\"live\":%d,\"ph\":%d,\"builtin\":%d}}\n",
               r.c_str(), t_sw < 0 ? -1 : t_sw - t_loss, t_hw_back < 0 ? -1 : t_hw_back - t_loss, builtin ? "true" : "false", counts["live"],
               counts["ph"], counts["builtin"]);
        return 0;
    }
    if (name == L"idlecpu") {
        rig.ctl.set(true, 1280, 720);
        sleep_ms(500);
        double c0 = cpu_s(), t0 = now_ms();
        sleep_ms(10000);
        double busy = (cpu_s() - c0) / ((now_ms() - t0) / 1000.0) * 100.0;
        rig.ctl.set(false);
        sleep_ms(500);
        c0 = cpu_s();
        t0 = now_ms();
        sleep_ms(10000);
        double idle = (cpu_s() - c0) / ((now_ms() - t0) / 1000.0) * 100.0;
        printf("{\"scenario\":\"idlecpu\",\"cpu_pct_core_in_use_no_sink\":%.3f,\"cpu_pct_core_not_in_use\":%.3f}\n", busy, idle);
        return 0;
    }
    printf("{\"scenario\":%s,\"error\":\"unknown\"}\n", json_str(utf8(name)).c_str());
    return 2;
}

// R3 #11: a stream whose display width is below its coded width (e.g. 1272 inside 1280), decoded straight through
// H264Decoder (hardware or software): the picture is a black left part and a white right part split at
// --edge (ffmpeg drawbox); in every decoded row the edge must be at the same column (a wrong row pitch shears it).
int cropcheck(const std::vector<std::wstring>& a) {
    const std::wstring file = a.size() > 2 ? a[2] : L"";
    const bool hw = !has_arg(a, L"--sw");
    const int edge = _wtoi(arg_after(a, L"--edge", L"636").c_str());
    auto aus = split_annexb(read_file(file));
    std::string info;
    HRESULT hr = S_OK;
    auto d = H264Decoder::create(hw, &info, &hr);
    if (!d) {
        printf("{\"cropcheck\":false,\"error\":%s}\n", json_str(info).c_str());
        return 4;
    }
    std::vector<uint8_t> pic;
    int pics = 0, bad_rows = 0, rows = 0, w = 0, h = 0, worst = 0;
    for (size_t i = 0; i < aus.size(); i++) {
        DecodeOut r = d->decode(aus[i].data(), aus[i].size(), uint32_t(i), [&](int pw, int ph) -> uint8_t* {
            pic.assign(size_t(pw) * ph * 3 / 2, 0);
            w = pw;
            h = ph;
            return pic.data();
        });
        if (r.result != DecodeResult::kOk || pic.empty()) continue;
        pics++;
        for (int y = 0; y < h; y += 7) {
            const uint8_t* row = &pic[size_t(y) * w];
            int x = 0;
            while (x < w && row[x] < 128) x++;  // first bright pixel
            rows++;
            const int off = std::abs(x - edge);
            worst = std::max(worst, off);
            if (off > 2) bad_rows++;
        }
    }
    const bool ok = pics > 0 && bad_rows == 0;
    printf("{\"cropcheck\":%s,\"decoder\":\"%s\",\"display\":\"%dx%d\",\"pictures\":%d,\"rows_checked\":%d,\"rows_bad\":%d,"
           "\"worst_edge_offset\":%d}\n",
           ok ? "true" : "false", hw ? "hw" : "sw", w, h, pics, rows, bad_rows, worst);
    return ok ? 0 : 1;
}

// install / vcam {} without a helper restart: the backend's own view through the test keys
int cmd_installflow(const std::vector<std::wstring>& a) {
    WinVideoOptions o;
    o.reg_base = arg_after(a, L"--reg");
    o.create_camera = false;
    WinVideoBackend b(o);
    Events ev;
    ev.backend = &b;
    b.set_listener(&ev);
    sleep_ms(300);
    VcamState s1 = b.vcam();
    int a1 = b.vcamref().create_attempts();
    printf("{\"installflow\":\"before\",\"install\":%s,\"create_attempts\":%d}\n", json_str(s1.install).c_str(), a1);
    fflush(stdout);
    // the caller runs vcam-install (elevated, test keys) now; then `vcam {}` = vcam()
    std::wstring wait_file = arg_after(a, L"--wait-file");
    for (int i = 0; i < 600 && GetFileAttributesW(wait_file.c_str()) == INVALID_FILE_ATTRIBUTES; i++) sleep_ms(100);
    VcamState s2 = b.vcam();
    sleep_ms(300);
    int a2 = b.vcamref().create_attempts();
    printf("{\"installflow\":\"after\",\"install\":%s,\"dll_version\":%d,\"create_attempts\":%d}\n", json_str(s2.install).c_str(), s2.dll_version, a2);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    _setmode(_fileno(stdout), _O_BINARY);
    ComScope com;
    std::vector<std::wstring> a(argv, argv + argc);
    if (a.size() < 2) {
        printf("usage: video_check unit|setup|clear|standin|play|scenario|installflow ...\n");
        return 2;
    }
    const std::wstring& c = a[1];
    if (c == L"unit") {
        unit(a);
        return g_fails ? 1 : 0;
    }
    if (c == L"setup") return setup(a);
    if (c == L"clear") return clear(a);
    if (c == L"standin") return standin(a);
    if (c == L"cropcheck") return cropcheck(a);
    if (c == L"play") return cmd_play(a);
    if (c == L"scenario") return scenario(a);
    if (c == L"installflow") return cmd_installflow(a);
    if (c == L"present") {  // read-only: is a "Beamer Camera" virtual camera present (and the real install state)
        InstallInfo i = read_install(L"");
        printf("{\"present\":%s,\"install\":%s,\"detail\":%s}\n", camera_device_present() ? "true" : "false", json_str(i.install).c_str(),
               json_str(i.detail).c_str());
        return 0;
    }
    return 2;
}
