// SPDX-License-Identifier: GPL-3.0-only
// backend_check.exe: drives the Windows Backend directly (no engine) to check WP4's acceptance points on lebig.
//
//   backend_check list [--all]                  snapshot as the engine sees it; --all adds blocked/hidden endpoints
//   backend_check state [--save F] [--compare F] mute/master/channel volumes of every active endpoint
//   backend_check watch [--seconds S]           devices_changed events (debounced, de-duplicated)
//   backend_check mic [--device KEY] [--seconds S]
//   backend_check system-test [--amp A]         process loopback + endpoint mute: in = out, own tone excluded
//   backend_check vmic-test                     render into CABLE Input, record CABLE Output, demand latency
//   backend_check render-test                   render to the default output (muted) checked on its loopback
//   backend_check journal-test                  kill a muting helper (and its guard): state restored exactly
//   backend_check invalidate-test               injected AUDCLNT_E_DEVICE_INVALIDATED: streams recover
//   backend_check exclusive-test                exclusive open of CABLE Input by another process vs our shared stream
//   backend_check restore                       restore_journal() (as `beamer-audio restore`)
// Child modes (spawned by the tests): record, sleeper, hold-mute, exclusive.
// Every command takes --state-dir D (default: %TEMP%\beamer-wp4-check) so the real Beamer journal is never touched.
#include "backend_win.h"

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

using namespace beamer::audio;
using namespace beamer::audio::win;

static const double kPi = 3.14159265358979323846;
static int g_fail = 0;

// ------------------------------------------------------------------ args

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> kv;
    bool has(const char* k) const { return kv.count(k) > 0; }
    std::string get(const char* k, const std::string& d = "") const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
    double num(const char* k, double d) const { return has(k) ? atof(get(k).c_str()) : d; }
};
static Args parse(const std::vector<std::string>& a) {
    Args r;
    for (size_t i = 1; i < a.size(); i++) {
        if (a[i].rfind("--", 0) == 0) {
            std::string k = a[i].substr(2);
            if (i + 1 < a.size() && a[i + 1].rfind("--", 0) != 0) r.kv[k] = a[++i];
            else r.kv[k] = "1";
        } else r.pos.push_back(a[i]);
    }
    return r;
}
static std::vector<std::string> wargs() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; i < n; i++) out.push_back(utf8(w[i]));
    LocalFree(w);
    return out;
}

static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}
static void check(bool ok, const char* what, const std::string& detail = "") {
    out("%s  %s%s%s", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) g_fail++;
}
static double dbfs(double x) { return x > 1e-10 ? 20 * std::log10(x) : -200.0; }
static double secs() { return now_ns() / 1e9; }

static std::wstring g_state_dir;
static WinOptions options() {
    WinOptions o;
    o.state_dir = g_state_dir;
    o.log_level = "info";
    return o;
}

// ------------------------------------------------------------------ meters

// Goertzel of `freq` on channel 0 plus rms/peak, after skipping the first `skip` seconds.
struct Meter : CaptureSink {
    double freq = 997, skip_s = 0.3;
    std::atomic<uint64_t> frames{0}, calls{0};
    uint64_t used = 0;
    double s1 = 0, s2 = 0, sumsq = 0, peak = 0, coeff = 0;
    std::atomic<int64_t> first_age_us{INT64_MIN};
    uint64_t last_host = 0; std::atomic<uint64_t> nonmono{0};
    std::atomic<bool> armed{true};
    void reset_analysis() { used = 0; s1 = s2 = sumsq = peak = 0; }
    void on_capture(const float* x, int n, int ch, uint64_t host_ns) override {
        if (!coeff) coeff = 2 * std::cos(2 * kPi * freq / kRate);
        if (first_age_us.load() == INT64_MIN) first_age_us = (int64_t)(now_ns() - host_ns) / 1000;
        if (last_host && host_ns < last_host) nonmono++;
        last_host = host_ns;
        uint64_t f0 = frames.fetch_add(n);
        calls++;
        if (!armed) return;
        for (int i = 0; i < n; i++) {
            if ((double)(f0 + i) / kRate < skip_s) continue;
            double v = x[(size_t)i * ch];
            sumsq += v * v;
            peak = std::max(peak, std::abs(v));
            double s0 = v + coeff * s1 - s2; s2 = s1; s1 = s0;
            used++;
        }
    }
    double tone_db() const { return used ? dbfs(std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) * 2 / used) : -200; }
    double rms_db() const { return used ? dbfs(std::sqrt(sumsq / used)) : -200; }
};

struct Tone : RenderSource {
    double amp = 0.2, freq = 997, ph = 0;
    std::atomic<uint64_t> frames{0};
    void on_render(float* x, int n, int ch, uint64_t) override {
        double d = 2 * kPi * freq / kRate;
        for (int i = 0; i < n; i++) {
            float v = (float)(amp * std::sin(ph));
            ph += d;
            if (ph > 2 * kPi) ph -= 2 * kPi;
            for (int c = 0; c < ch; c++) x[(size_t)i * ch + c] = v;
        }
        frames += n;
    }
};

struct Listener : BackendListener {
    std::atomic<int> changes{0}, lost{0};
    std::function<void()> on_change;
    std::string why;
    void devices_changed() override { changes++; if (on_change) on_change(); }
    void stream_lost(StreamHandle*, const std::string& w) override { lost++; why = w; }
};

// ------------------------------------------------------------------ endpoint state (all active endpoints)

static std::map<std::string, EndpointLevel> read_all_levels() {
    std::map<std::string, EndpointLevel> m;
    auto e = make_enumerator();
    for (EDataFlow f : {eCapture, eRender}) {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(e->EnumAudioEndpoints(f, DEVICE_STATE_ACTIVE, &col))) continue;
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            ComPtr<IMMDevice> d;
            col->Item(i, &d);
            EndpointLevel l;
            if (read_level(d.Get(), &l)) {
                l.name = describe_endpoint(d.Get(), f, false).name;
                m[l.device] = l;
            }
        }
    }
    return m;
}
static std::string level_text(const EndpointLevel& l) {
    std::ostringstream s;
    s.precision(9);
    s << (l.mute ? 1 : 0) << " " << l.master;
    for (float c : l.channels) s << " " << c;
    return s.str();
}
static void save_levels(const std::string& path, const std::map<std::string, EndpointLevel>& m) {
    std::ofstream f(path, std::ios::binary);
    for (auto& [id, l] : m) f << id << "\t" << level_text(l) << "\t" << l.name << "\n";
}
static std::map<std::string, std::string> load_levels(const std::string& path) {
    std::map<std::string, std::string> m;
    std::ifstream f(path, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        auto a = line.find('\t'), b = line.find('\t', a + 1);
        if (a != std::string::npos && b != std::string::npos) m[line.substr(0, a)] = line.substr(a + 1, b - a - 1);
    }
    return m;
}
// Compares the current state with `before`; returns the number of endpoints that differ.
static int compare_levels(const std::map<std::string, EndpointLevel>& before, const char* label) {
    auto now = read_all_levels();
    int diff = 0;
    for (auto& [id, l] : before) {
        auto it = now.find(id);
        if (it == now.end()) { out("  %s: %s gone", label, l.name.c_str()); diff++; continue; }
        if (level_text(it->second) != level_text(l)) {
            out("  %s: %s was [%s] now [%s]", label, l.name.c_str(), level_text(l).c_str(), level_text(it->second).c_str());
            diff++;
        }
    }
    return diff;
}
static bool is_muted(const std::string& id) {
    auto e = make_enumerator();
    auto d = device_by_id(e.Get(), id);
    EndpointLevel l;
    return d && read_level(d.Get(), &l) && l.mute;
}

// ------------------------------------------------------------------ child processes

struct Child {
    HANDLE proc = nullptr, out = nullptr;
    DWORD pid = 0;
    std::string buf;
    bool start(const std::string& args) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + wide(args) + L" --state-dir \"" + g_state_dir + L"\"";
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE r, w;
        CreatePipe(&r, &w, &sa, 0);
        SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(w);
        if (!ok) { CloseHandle(r); return false; }
        CloseHandle(pi.hThread);
        proc = pi.hProcess; pid = pi.dwProcessId; out = r;
        return true;
    }
    // Next stdout line, or "" after timeout / EOF.
    std::string line(double timeout_s) {
        double end = secs() + timeout_s;
        while (true) {
            auto nl = buf.find('\n');
            if (nl != std::string::npos) { std::string l = buf.substr(0, nl); buf.erase(0, nl + 1); if (!l.empty() && l.back() == '\r') l.pop_back(); return l; }
            DWORD avail = 0;
            if (!PeekNamedPipe(out, nullptr, 0, nullptr, &avail, nullptr)) return "";
            if (avail) {
                char tmp[4096]; DWORD n = 0;
                if (!ReadFile(out, tmp, std::min<DWORD>(avail, sizeof tmp), &n, nullptr) || !n) return "";
                buf.append(tmp, n);
                continue;
            }
            if (secs() > end) return "";
            Sleep(2);
        }
    }
    std::string wait_for(const std::string& prefix, double timeout_s) {
        double end = secs() + timeout_s;
        while (secs() < end) {
            std::string l = line(end - secs());
            if (l.rfind(prefix, 0) == 0) return l;
            if (l.empty() && WaitForSingleObject(proc, 0) == WAIT_OBJECT_0 && buf.empty()) { DWORD a = 0; if (!PeekNamedPipe(out, nullptr, 0, nullptr, &a, nullptr) || !a) return ""; }
        }
        return "";
    }
    void kill() { if (proc) TerminateProcess(proc, 9); }
    void wait(DWORD ms = INFINITE) { if (proc) WaitForSingleObject(proc, ms); }
    ~Child() { if (proc) CloseHandle(proc); if (out) CloseHandle(out); }
};

// Children of `parent` running this same executable (the guard), not its conhost.exe.
static std::vector<DWORD> children_of(DWORD parent) {
    std::vector<DWORD> v;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring self = exe;
    self = self.substr(self.find_last_of(L"\\/") + 1);
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
        if (pe.th32ParentProcessID == parent && _wcsicmp(pe.szExeFile, self.c_str()) == 0) v.push_back(pe.th32ProcessID);
    CloseHandle(s);
    return v;
}

// Raw WASAPI recorder (not through the Backend): what an app recording "CABLE Output" does.
static int child_record(const Args& a) {
    ComScope com;
    auto e = make_enumerator();
    auto d = device_by_id(e.Get(), a.get("device"));
    if (!d) { out("error no device"); return 2; }
    Client c;
    HRESULT hr = open_endpoint_client(d.Get(), ClientKind::Capture, 2, &c);
    if (FAILED(hr)) { out("error open %s", hr_text(hr).c_str()); return 1; }
    Sleep((DWORD)(a.num("delay", 0) * 1000));
    c.client->Start();
    out("started %llu", (unsigned long long)now_ns());
    Meter m;
    m.freq = a.num("freq", 997);
    double end = secs() + a.num("seconds", 3);
    std::vector<float> conv(48000 * 2);
    while (secs() < end) {
        WaitForSingleObject(c.event, 100);
        UINT32 pk = 0;
        while (SUCCEEDED(c.capture->GetNextPacketSize(&pk)) && pk) {
            BYTE* p; UINT32 n; DWORD fl;
            if (FAILED(c.capture->GetBuffer(&p, &n, &fl, nullptr, nullptr))) break;
            n = std::min<UINT32>(n, 48000);
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(conv.begin(), conv.begin() + n, 0.f);
            else convert_channels(reinterpret_cast<float*>(p), c.channels, conv.data(), 1, n);
            m.on_capture(conv.data(), (int)n, 1, now_ns());
            c.capture->ReleaseBuffer(n);
        }
    }
    c.client->Stop();
    out("stopped %llu", (unsigned long long)now_ns());
    out("result frames=%llu tone_dbfs=%.2f rms_dbfs=%.2f", (unsigned long long)m.frames.load(), m.tone_db(), m.rms_db());
    return 0;
}

static int child_sleeper(const Args& a) { Sleep((DWORD)(a.num("seconds", 30) * 1000)); return 0; }

// Holds a system capture with mute_local (as the helper does while sound goes to another machine).
static int child_hold_mute(const Args& a) {
    ComScope com;
    auto b = make_win_backend(options());
    Meter m;
    CaptureSpec cs{CaptureKind::System, "", 2, true, {}};
    std::string err;
    auto s = b->open_capture(cs, &m, &err);
    if (!s) { out("error %s", err.c_str()); return 1; }
    out("muted %s", s->device_in_use().c_str());
    Sleep((DWORD)(a.num("seconds", 30) * 1000));
    return 0;
}

static int child_exclusive(const Args& a) {
    ComScope com;
    auto e = make_enumerator();
    auto d = device_by_id(e.Get(), a.get("device"));
    if (!d) { out("error no device"); return 2; }
    ComPtr<IAudioClient> c;
    d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c);
    WAVEFORMATEX* mix = nullptr;
    c->GetMixFormat(&mix);
    REFERENCE_TIME def = 0, mn = 0;
    c->GetDevicePeriod(&def, &mn);
    // Exclusive mode: try the mix format, then 16-bit stereo 48 kHz.
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, def, def, mix, nullptr);
    if (FAILED(hr)) {
        WAVEFORMATEX f{WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
        c.Reset();
        d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c);
        hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, def, def, &f, nullptr);
    }
    CoTaskMemFree(mix);
    out("exclusive %s", hr_text(hr).c_str());
    if (SUCCEEDED(hr)) { c->Start(); Sleep((DWORD)(a.num("seconds", 2) * 1000)); c->Stop(); }
    out("done");
    return 0;
}

// ------------------------------------------------------------------ commands

static void print_snapshot(Backend& b, bool all) {
    auto s = b.snapshot();
    out("default_input  %s", s.default_input.c_str());
    out("default_output %s", s.default_output.c_str());
    out("virtual_mic    kind=%s present=%d in_use=%d", s.vmic_kind.c_str(), s.vmic_present, s.vmic_in_use);
    std::string f;
    for (auto& w : b.features()) f += w + " ";
    out("features       %s", f.c_str());
    out("permission     mic=%s system_audio=%s", b.permission("mic").c_str(), b.permission("system_audio").c_str());
    const char* roles[] = {"normal", "virtual_mic", "virtual_mic_feed"};
    for (auto& d : s.devices)
        out("%s %-3s %-9s %-10s %dch %5.0f  %-17s %s  [%s]", d.key == s.default_input || d.key == s.default_output ? "*" : " ", d.input ? "in" : "out",
            d.transport.c_str(), form_word(d.form).c_str(), d.channels, d.nominal_rate, roles[(int)d.role], d.name.c_str(), d.manufacturer.c_str());
    if (!all) return;
    out("-- every active endpoint (blocked and hidden included):");
    auto e = make_enumerator();
    for (auto& ep : enumerate_endpoints(e.Get(), DEVICE_STATE_ACTIVE, true)) {
        out("%-7s %-3s %s", ep.blocked ? "BLOCKED" : ep.hidden ? "hidden" : "listed", ep.render ? "out" : "in", ep.name.c_str());
        out("        id=%s enum=%s form_factor=%d transport=%s mfr=%s provider=%s", ep.id.c_str(), ep.enumerator.c_str(), ep.form_factor,
            ep.info.transport.c_str(), ep.manufacturer.c_str(), ep.driver_provider.c_str());
        out("        adapter=%s | %s | root=%s%s%s", ep.adapter.c_str(), ep.adapter_instance.c_str(),
            ep.ancestors.empty() ? "" : ep.ancestors.back().c_str(), ep.why.empty() ? "" : " | ", ep.why.c_str());
    }
}

static int cmd_list(const Args& a) {
    auto b = make_win_backend(options());
    print_snapshot(*b, a.has("all"));
    // Acceptance: Wave:3, G733, DualSense listed; no Elgato virtual endpoint; no CABLE Output / 16ch as a device.
    if (a.has("check")) {
        auto s = b->snapshot();
        auto listed = [&](const char* needle, Role role) {
            for (auto& d : s.devices) if (d.role == role && d.name.find(needle) != std::string::npos) return true;
            return false;
        };
        bool wave = listed("Elgato Wave:3", Role::Normal), g733 = listed("G733", Role::Normal), ds = listed("DualSense", Role::Normal);
        bool elgato_virtual = false, cable_bad = false;
        for (auto& d : s.devices) {
            elgato_virtual |= d.name.find("Elgato Virtual Audio") != std::string::npos;
            cable_bad |= (d.name.find("CABLE Output") != std::string::npos && d.role == Role::Normal) || d.name.find("CABLE In 16") != std::string::npos;
        }
        check(wave && g733 && ds, "list shows Wave:3, G733, DualSense");
        check(!elgato_virtual, "no Elgato virtual endpoint listed");
        check(!cable_bad, "CABLE Output only as the virtual mic, CABLE In 16ch absent");
    }
    return g_fail ? 1 : 0;
}

static int cmd_state(const Args& a) {
    ComScope com;
    auto m = read_all_levels();
    if (a.has("save")) { save_levels(a.get("save"), m); out("saved %u endpoints to %s", (unsigned)m.size(), a.get("save").c_str()); }
    if (a.has("compare")) {
        auto before = load_levels(a.get("compare"));
        int diff = 0;
        for (auto& [id, t] : before) {
            auto it = m.find(id);
            if (it == m.end()) { out("  gone: %s", id.c_str()); diff++; }
            else if (level_text(it->second) != t) { out("  differs: %s was [%s] now [%s]", it->second.name.c_str(), t.c_str(), level_text(it->second).c_str()); diff++; }
        }
        check(diff == 0 && before.size() == m.size(), "endpoint state identical to the saved one",
              std::to_string(m.size()) + " endpoints, " + std::to_string(diff) + " differ");
        return diff ? 1 : 0;
    }
    if (!a.has("save"))
        for (auto& [id, l] : m) out("%-60s %s", l.name.c_str(), level_text(l).c_str());
    return 0;
}

static int cmd_watch(const Args& a) {
    auto b = make_win_backend(options());
    Listener l;
    double t0 = secs();
    l.on_change = [&] {
        auto s = b->snapshot();
        out("%.3f devices_changed: %u devices, in=%s out=%s vmic present=%d in_use=%d", secs() - t0, (unsigned)s.devices.size(),
            s.default_input.c_str(), s.default_output.c_str(), s.vmic_present, s.vmic_in_use);
    };
    b->set_listener(&l);
    out("watching %.0f s", a.num("seconds", 30));
    Sleep((DWORD)(a.num("seconds", 30) * 1000));
    b->set_listener(nullptr);
    return 0;
}

static int cmd_mic(const Args& a) {
    auto b = make_win_backend(options());
    Meter m;
    CaptureSpec cs{CaptureKind::Mic, a.get("device"), (int)a.num("channels", 1), false, {}};
    std::string err;
    double t0 = secs();
    auto s = b->open_capture(cs, &m, &err);
    if (!s) { check(false, "mic capture opens", err); return 1; }
    double open_ms = (secs() - t0) * 1000;
    double sec = a.num("seconds", 3);
    Sleep((DWORD)(sec * 1000));
    std::string dev = s->device_in_use();
    s.reset();
    double rate = m.frames.load() / sec;
    out("mic %s: open %.1f ms, %llu frames in %.1f s (%.0f/s), %llu callbacks, first packet age %lld us, rms %.1f dBFS, peak %.1f dBFS, non-monotonic %llu",
        dev.c_str(), open_ms, (unsigned long long)m.frames.load(), sec, rate, (unsigned long long)m.calls.load(), (long long)m.first_age_us.load(),
        m.rms_db(), dbfs(m.peak), (unsigned long long)m.nonmono.load());
    check(rate > 47000 && rate < 49500, "mic delivers 48 kHz", std::to_string((int)rate) + " frames/s");
    return g_fail ? 1 : 0;
}

static int cmd_system_test(const Args& a) {
    ComScope com;
    auto before = read_all_levels();
    auto b = make_win_backend(options());
    double amp = a.num("amp", 0.2);  // -13.98 dBFS
    double want_db = dbfs(amp);
    std::string out_id = b->snapshot().default_output;
    if (out_id.empty()) { check(false, "a default output exists"); return 1; }
    std::string err;

    // 0. Does process loopback deliver a steady clock with nothing of ours playing?
    {
        Meter m;
        CaptureSpec cs{CaptureKind::System, "", 2, false, {(int64_t)GetCurrentProcessId()}};
        auto s = b->open_capture(cs, &m, &err);
        if (!s) { check(false, "system capture opens", err); return 1; }
        Sleep(2000);
        s.reset();
        out("process loopback, idle 2 s: %llu frames (%.0f/s), %llu callbacks", (unsigned long long)m.frames.load(), m.frames.load() / 2.0, (unsigned long long)m.calls.load());
    }

    // 1. Muted endpoint, tone from a process outside the excluded tree: in = out on process loopback and on the
    //    endpoint loopback (AEC reference). The tone is ours, so exclude an idle sleeper's tree instead of ours.
    Child sleeper;
    sleeper.start("sleeper --seconds 30");
    {
        Meter sys, ref;
        CaptureSpec cs{CaptureKind::System, "", 2, true, {(int64_t)sleeper.pid}};
        auto s = b->open_capture(cs, &sys, &err);
        if (!s) { check(false, "system capture with mute opens", err); return 1; }
        check(is_muted(out_id), "default output muted while capturing", s->device_in_use());
        check(held_mutes(b.get()) == 1, "backend holds one mute");
        CaptureSpec rs{CaptureKind::Reference, "", 2, false, {}};
        auto r = b->open_capture(rs, &ref, &err);
        if (!r) check(false, "reference capture opens", err);
        Tone tone;
        tone.amp = amp;
        RenderSpec ps{"", false, 2};
        auto p = b->open_render(ps, &tone, &err);
        if (!p) { check(false, "render opens", err); return 1; }
        Sleep(300);
        sys.reset_analysis(); ref.reset_analysis();
        sys.skip_s = ref.skip_s = 0;
        Sleep(2500);
        p.reset(); r.reset();
        double got = sys.tone_db(), got_ref = ref.tone_db();
        check(std::fabs(got - want_db) < 0.1, "process loopback keeps the level with the endpoint muted",
              "tone " + std::to_string(got) + " dBFS, rendered " + std::to_string(want_db));
        check(std::fabs(got_ref - want_db) < 0.1, "endpoint loopback (reference) keeps the level with the endpoint muted",
              "tone " + std::to_string(got_ref) + " dBFS");
        s.reset();
        check(!is_muted(out_id) || before[out_id].mute, "mute released at stop");
    }
    sleeper.kill();

    // 2. Exclude our own tree (as the helper excludes Beamer's): our own tone must be absent from the capture.
    {
        Meter sys;
        CaptureSpec cs{CaptureKind::System, "", 2, true, {(int64_t)GetCurrentProcessId()}};
        auto s = b->open_capture(cs, &sys, &err);
        if (!s) { check(false, "system capture (exclude self) opens", err); return 1; }
        Tone tone;
        tone.amp = amp;
        RenderSpec ps{"", false, 2};
        auto p = b->open_render(ps, &tone, &err);
        Sleep(300);
        sys.reset_analysis();
        sys.skip_s = 0;
        Sleep(2500);
        p.reset();
        s.reset();
        double got = sys.tone_db();
        check(got <= -90, "own tone absent from its own system capture", "tone " + std::to_string(got) + " dBFS (rendered " + std::to_string(want_db) + ")");
    }
    b.reset();
    int diff = compare_levels(before, "after system-test");
    check(diff == 0, "all active endpoints back to their state before the test", std::to_string(before.size()) + " endpoints");
    return g_fail ? 1 : 0;
}

static int cmd_vmic_test(const Args& a) {
    ComScope com;
    auto b = make_win_backend(options());
    auto s = b->snapshot();
    std::string cable_out;
    for (auto& d : s.devices) if (d.role == Role::VirtualMic) cable_out = d.key;
    check(s.vmic_kind == "vbcable" && s.vmic_present, "VB-CABLE present", cable_out);
    if (cable_out.empty()) return 1;
    check(!s.vmic_in_use, "virtual mic idle before the test");

    std::atomic<uint64_t> on_ns{0}, off_ns{0};
    Listener l;
    l.on_change = [&] {
        bool u = b->snapshot().vmic_in_use;
        if (u && !on_ns) on_ns = now_ns();
        if (!u && on_ns && !off_ns) off_ns = now_ns();
    };
    b->set_listener(&l);

    Tone tone;
    tone.amp = a.num("amp", 0.1);
    RenderSpec ps{"", true, 2};
    std::string err;
    auto p = b->open_render(ps, &tone, &err);
    check((bool)p, "render into the virtual mic (CABLE Input) opens", p ? p->device_in_use() : err);
    if (!p) return 1;
    Child rec;
    rec.start("record --device " + cable_out + " --seconds 3 --delay 0.5");
    std::string started = rec.wait_for("started ", 10);
    std::string stopped = rec.wait_for("stopped ", 10);
    std::string result = rec.wait_for("result ", 5);
    rec.wait(5000);
    uint64_t t_start = started.empty() ? 0 : _strtoui64(started.c_str() + 8, nullptr, 10);
    uint64_t t_stop = stopped.empty() ? 0 : _strtoui64(stopped.c_str() + 8, nullptr, 10);
    double lat_ms = on_ns && t_start ? ((double)on_ns.load() - (double)t_start) / 1e6 : -1;
    check(on_ns && lat_ms <= 300, "demand true <= 300 ms after a recorder opens CABLE Output", std::to_string(lat_ms) + " ms");
    for (int i = 0; i < 50 && !off_ns; i++) Sleep(100);
    double off_ms = off_ns && t_stop ? ((double)off_ns.load() - (double)t_stop) / 1e6 : -1;
    check(off_ns > 0, "demand false after the recorder stops (raw, no hold in the backend)", std::to_string(off_ms) + " ms");
    double tone_db = -200;
    auto k = result.find("tone_dbfs=");
    if (k != std::string::npos) tone_db = atof(result.c_str() + k + 10);
    check(std::fabs(tone_db - dbfs(tone.amp)) < 0.5, "the recorder hears the helper's tone on CABLE Output",
          std::to_string(tone_db) + " dBFS, rendered " + std::to_string(dbfs(tone.amp)));
    p.reset();
    b->set_listener(nullptr);
    return g_fail ? 1 : 0;
}

static int cmd_render_test(const Args& a) {
    ComScope com;
    auto before = read_all_levels();
    auto b = make_win_backend(options());
    std::string err;
    // Muted (so nobody hears the tone): system capture with mute_local holds the default output's mute.
    Meter sys, ref;
    CaptureSpec cs{CaptureKind::System, "", 2, true, {}};
    auto s = b->open_capture(cs, &sys, &err);
    if (!s) { check(false, "mute holder opens", err); return 1; }
    std::string out_id = s->device_in_use();
    CaptureSpec rs{CaptureKind::Reference, out_id, 2, false, {}};
    auto r = b->open_capture(rs, &ref, &err);
    for (int ch : {1, 2}) {
        Tone tone;
        tone.amp = a.num("amp", 0.1);
        RenderSpec ps{out_id, false, ch};
        auto p = b->open_render(ps, &tone, &err);
        if (!p) { check(false, "render to an endpoint opens", err); continue; }
        Sleep(300);
        ref.reset_analysis();
        ref.skip_s = 0;
        uint64_t f0 = tone.frames.load();
        double t0 = secs();
        Sleep(2000);
        double rate = (tone.frames.load() - f0) / (secs() - t0);
        p.reset();
        check(std::fabs(ref.tone_db() - dbfs(tone.amp)) < 0.1, ch == 1 ? "mono render to an endpoint at unity (loopback)" : "stereo render to an endpoint at unity (loopback)",
              std::to_string(ref.tone_db()) + " dBFS, " + std::to_string((int)rate) + " frames/s pulled");
    }
    r.reset();
    s.reset();
    b.reset();
    check(compare_levels(before, "after render-test") == 0, "all active endpoints back to their state");
    return g_fail ? 1 : 0;
}

static int cmd_journal_test(const Args&) {
    ComScope com;
    auto before = read_all_levels();
    std::string out_id;
    {
        auto b = make_win_backend(options());
        out_id = b->snapshot().default_output;
        b->restore_journal();
    }
    // A. Helper killed while muting: its guard restores within ~1 s.
    {
        Child h;
        h.start("hold-mute --seconds 60");
        std::string l = h.wait_for("muted ", 10);
        check(!l.empty() && is_muted(out_id), "child helper muted the default output", l);
        h.kill();
        h.wait();
        double t0 = secs();
        while (secs() - t0 < 3 && is_muted(out_id) && !before[out_id].mute) Sleep(20);
        double ms = (secs() - t0) * 1000;
        check(compare_levels(before, "after kill (guard)") == 0, "guard restored the endpoint after TerminateProcess of the helper", std::to_string((int)ms) + " ms");
    }
    // B. Helper and guard both killed: the endpoint stays muted until the next start's restore_journal.
    {
        Child h;
        h.start("hold-mute --seconds 60");
        std::string l = h.wait_for("muted ", 10);
        Sleep(200);
        auto guards = children_of(h.pid);
        check(!l.empty() && guards.size() == 1, "child helper started one guard process", std::to_string(guards.size()) + " children");
        for (DWORD g : guards) {
            HANDLE gh = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, g);
            if (gh) { TerminateProcess(gh, 9); WaitForSingleObject(gh, 2000); CloseHandle(gh); }
        }
        h.kill();
        h.wait();
        Sleep(1500);
        check(is_muted(out_id), "with helper and guard killed the endpoint is still muted (expected)");
        auto b = make_win_backend(options());
        int n = b->restore_journal();
        check(n == 1, "next start's restore_journal() restores one endpoint", std::to_string(n));
        check(compare_levels(before, "after restore_journal") == 0, "state restored exactly (mute, master, channel volumes, all active endpoints)");
        b.reset();
        auto b2 = make_win_backend(options());
        check(b2->restore_journal() == 0, "journal empty afterwards");
    }
    return g_fail ? 1 : 0;
}

static int cmd_invalidate_test(const Args&) {
    ComScope com;
    auto before = read_all_levels();
    auto b = make_win_backend(options());
    Listener l;
    b->set_listener(&l);
    std::string err;
    Meter mic, sys;
    Tone tone;
    tone.amp = 0.0;  // silence: the render stream is checked by its pull rate only
    auto m = b->open_capture(CaptureSpec{CaptureKind::Mic, "", 1, false, {}}, &mic, &err);
    auto s = b->open_capture(CaptureSpec{CaptureKind::System, "", 2, true, {}}, &sys, &err);
    auto r = b->open_render(RenderSpec{"", true, 2}, &tone, &err);
    if (!m || !s || !r) { check(false, "streams open", err); return 1; }
    Sleep(500);
    for (StreamHandle* h : {m.get(), s.get(), r.get()}) inject_invalidation(h);
    Sleep(300);
    uint64_t fm = mic.frames, fs = sys.frames, fr = tone.frames;
    Sleep(1000);
    check(mic.frames - fm > 40000, "mic capture recovered after invalidation", std::to_string(mic.frames - fm) + " frames/s");
    check(sys.frames - fs > 40000, "system capture recovered after invalidation", std::to_string(sys.frames - fs) + " frames/s");
    check(tone.frames - fr > 40000, "virtual-mic render recovered after invalidation", std::to_string(tone.frames - fr) + " frames/s");
    check(held_mutes(b.get()) == 1, "mute still held once after recovery");
    check(l.lost == 0, "no stream_lost when the device comes back");
    m.reset(); s.reset(); r.reset();
    b->set_listener(nullptr);
    b.reset();
    check(compare_levels(before, "after invalidate-test") == 0, "all active endpoints back to their state");
    return g_fail ? 1 : 0;
}

static int cmd_exclusive_test(const Args&) {
    ComScope com;
    auto b = make_win_backend(options());
    Listener l;
    b->set_listener(&l);
    std::string err;
    Tone tone;
    tone.amp = 0;
    auto r = b->open_render(RenderSpec{"", true, 2}, &tone, &err);
    if (!r) { check(false, "virtual-mic render opens", err); return 1; }
    std::string id = r->device_in_use();
    Sleep(300);
    Child x;
    x.start("exclusive --device " + id + " --seconds 2");
    std::string res = x.wait_for("exclusive ", 10);
    out("other process, exclusive open of CABLE Input while we render shared: %s", res.c_str());
    x.wait_for("done", 10);
    x.wait(5000);
    uint64_t f0 = tone.frames;
    Sleep(1000);
    out("after: render pulled %llu frames in 1 s, stream_lost=%d (%s)", (unsigned long long)(tone.frames - f0), l.lost.load(), l.why.c_str());
    r.reset();
    // And the other way: their exclusive stream first, then our shared open.
    Child y;
    y.start("exclusive --device " + id + " --seconds 3");
    res = y.wait_for("exclusive ", 10);
    auto r2 = b->open_render(RenderSpec{"", true, 2}, &tone, &err);
    out("our shared open while another process holds CABLE Input exclusively (%s): %s", res.c_str(), r2 ? "opened" : err.c_str());
    check(!r2 && err.rfind("device_busy", 0) == 0, "shared open refused as device_busy while another process is exclusive");
    r2.reset();
    y.wait(10000);
    // Real AUDCLNT_E_DEVICE_INVALIDATED (taken exclusively by another process) for longer than the 3 s recovery
    // window: stream_lost must be delivered once.
    l.lost = 0;
    auto r3 = b->open_render(RenderSpec{"", true, 2}, &tone, &err);
    Child z;
    z.start("exclusive --device " + id + " --seconds 6");
    z.wait_for("exclusive ", 10);
    double t0 = secs();
    while (secs() - t0 < 5 && l.lost == 0) Sleep(20);
    check(l.lost == 1, "stream_lost after 3 s without the device", std::to_string(secs() - t0) + " s after the exclusive open: " + l.why);
    z.wait(10000);
    r3.reset();
    b->set_listener(nullptr);
    return g_fail ? 1 : 0;
}

static int cmd_restore(const Args&) {
    auto b = make_win_backend(options());
    out("restored %d", b->restore_journal());
    return 0;
}

int main(int argc, char** argv) {
    int rc = 0;
    if (run_guard_if_requested(argc, argv, &rc)) return rc;
    auto argv8 = wargs();
    Args a = parse(argv8);
    if (a.has("state-dir")) g_state_dir = wide(a.get("state-dir"));
    else {
        wchar_t t[MAX_PATH];
        GetTempPathW(MAX_PATH, t);
        g_state_dir = std::wstring(t) + L"beamer-wp4-check";
    }
    CreateDirectoryW(g_state_dir.c_str(), nullptr);
    set_log_level(a.get("log-level", "info"));
    std::string cmd = a.pos.empty() ? "list" : a.pos[0];
    ComScope com;
    if (cmd == "list") return cmd_list(a);
    if (cmd == "state") return cmd_state(a);
    if (cmd == "watch") return cmd_watch(a);
    if (cmd == "mic") return cmd_mic(a);
    if (cmd == "system-test") return cmd_system_test(a);
    if (cmd == "vmic-test") return cmd_vmic_test(a);
    if (cmd == "render-test") return cmd_render_test(a);
    if (cmd == "journal-test") return cmd_journal_test(a);
    if (cmd == "invalidate-test") return cmd_invalidate_test(a);
    if (cmd == "exclusive-test") return cmd_exclusive_test(a);
    if (cmd == "restore") return cmd_restore(a);
    if (cmd == "record") return child_record(a);
    if (cmd == "sleeper") return child_sleeper(a);
    if (cmd == "hold-mute") return child_hold_mute(a);
    if (cmd == "exclusive") return child_exclusive(a);
    fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
