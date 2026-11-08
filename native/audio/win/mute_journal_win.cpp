// SPDX-License-Identifier: GPL-3.0-only
// Windows backend: endpoint mute with a crash journal and a guard process.
//
// Leaving a machine muted is the worst failure of the system-sound path, so every mute is written to
// <state_dir>/audio-mute-journal.json (mute, master and channel volumes as they were) before it is applied:
// - normal stop / quit: the MuteManager puts the endpoint back and removes its entry;
// - helper killed: the guard (`beamer-audio _guard --pid <helper>`), started at the first mute, restores the
//   entries of that helper within ~1 s;
// - helper and guard both killed: the next helper start restores every entry whose owner is gone (restore_journal,
//   before `ready`), and so does `beamer-audio restore`.
// Entries carry the owner's pid and process creation time, so a live helper's mute is never undone by another.
#include "backend_win.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace beamer::audio::win {

// ------------------------------------------------------------------ minimal JSON (this file's own format only)

namespace {
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};
struct JParser {
    const char* p; const char* end; bool ok = true;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool lit(const char* w) { size_t n = strlen(w); if ((size_t)(end - p) >= n && !memcmp(p, w, n)) { p += n; return true; } return false; }
    std::string str() {
        std::string out;
        if (p >= end || *p != '"') { ok = false; return out; }
        p++;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') { out += c; continue; }
            if (p >= end) { ok = false; return out; }
            char e = *p++;
            switch (e) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': {
                    if (end - p < 4) { ok = false; return out; }
                    unsigned cp = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                    else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e;
            }
        }
        if (p >= end) ok = false; else p++;
        return out;
    }
    JVal val(int depth = 0) {
        JVal v;
        ws();
        if (p >= end || depth > 16) { ok = false; return v; }
        if (*p == '{') {
            p++; v.t = JVal::Obj; ws();
            if (p < end && *p == '}') { p++; return v; }
            while (ok) {
                ws(); std::string k = str(); ws();
                if (p >= end || *p != ':') { ok = false; break; }
                p++;
                v.o.emplace_back(k, val(depth + 1));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; break; }
                ok = false;
            }
        } else if (*p == '[') {
            p++; v.t = JVal::Arr; ws();
            if (p < end && *p == ']') { p++; return v; }
            while (ok) {
                v.a.push_back(val(depth + 1));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; break; }
                ok = false;
            }
        } else if (*p == '"') { v.t = JVal::Str; v.s = str(); }
        else if (lit("true")) { v.t = JVal::Bool; v.b = true; }
        else if (lit("false")) { v.t = JVal::Bool; }
        else if (lit("null")) {}
        else {
            std::string tok(p, std::min<size_t>(64, (size_t)(end - p)));
            char* e = nullptr;
            v.t = JVal::Num;
            v.n = strtod(tok.c_str(), &e);
            size_t used = e ? (size_t)(e - tok.c_str()) : 0;
            if (!used) ok = false;
            p += used;
        }
        return v;
    }
};

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

struct Owner { DWORD pid = 0; uint64_t created = 0; };
uint64_t process_created(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return 0;
    return ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
}
Owner self_owner() {
    static Owner o = [] { Owner x; x.pid = GetCurrentProcessId(); x.created = process_created(GetCurrentProcess()); return x; }();
    return o;
}
bool owner_alive(const Owner& o) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, o.pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE && (o.created == 0 || process_created(h) == o.created);
    CloseHandle(h);
    return alive;
}

struct Entry { Owner owner; EndpointLevel level; };

std::wstring journal_file(const std::wstring& dir) {
    std::wstring d = dir;
    if (d.empty()) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        d = n ? std::wstring(buf, n) + L"\\Beamer" : L".";
    }
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    return d + L"audio-mute-journal.json";
}

// Serializes every journal access across processes (helper, guard, `restore`).
struct JournalLock {
    HANDLE m;
    JournalLock() : m(CreateMutexW(nullptr, FALSE, L"Local\\BeamerAudioMuteJournal")) { if (m) WaitForSingleObject(m, 5000); }
    ~JournalLock() { if (m) { ReleaseMutex(m); CloseHandle(m); } }
};

std::vector<Entry> read_entries(const std::wstring& path) {
    std::vector<Entry> out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string text;
    char buf[4096];
    DWORD n;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) text.append(buf, n);
    CloseHandle(f);
    JParser jp{text.data(), text.data() + text.size()};
    JVal root = jp.val();
    const JVal* es = root.get("entries");
    if (!jp.ok || !es || es->t != JVal::Arr) {
        BLOG_W("mute journal unreadable, ignored (%u bytes)", (unsigned)text.size());
        return out;
    }
    for (const auto& e : es->a) {
        Entry x;
        const JVal *pid = e.get("pid"), *cr = e.get("created"), *dev = e.get("device"), *nm = e.get("name"), *mu = e.get("mute"),
                   *ma = e.get("master"), *ch = e.get("channels");
        if (!dev || dev->t != JVal::Str || !mu || !ma) continue;
        x.owner.pid = pid ? (DWORD)pid->n : 0;
        x.owner.created = cr && cr->t == JVal::Str ? _strtoui64(cr->s.c_str(), nullptr, 10) : 0;
        x.level.device = dev->s;
        x.level.name = nm && nm->t == JVal::Str ? nm->s : "";
        x.level.mute = mu->t == JVal::Bool && mu->b;
        x.level.master = (float)ma->n;
        if (ch && ch->t == JVal::Arr) for (auto& c : ch->a) x.level.channels.push_back((float)c.n);
        out.push_back(std::move(x));
    }
    return out;
}

bool write_entries(const std::wstring& path, const std::vector<Entry>& entries) {
    if (entries.empty()) {
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        return true;
    }
    std::string s = "{\"version\":1,\"entries\":[";
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        char num[64];
        s += i ? ",\n" : "\n";
        s += "{\"pid\":" + std::to_string(e.owner.pid) + ",\"created\":\"" + std::to_string(e.owner.created) + "\",\"device\":" +
             jstr(e.level.device) + ",\"name\":" + jstr(e.level.name) + ",\"mute\":" + (e.level.mute ? "true" : "false");
        snprintf(num, sizeof num, "%.9g", e.level.master);
        s += std::string(",\"master\":") + num + ",\"channels\":[";
        for (size_t c = 0; c < e.level.channels.size(); c++) {
            snprintf(num, sizeof num, "%s%.9g", c ? "," : "", e.level.channels[c]);
            s += num;
        }
        s += "]}";
    }
    s += "\n]}\n";
    // Atomic: write a temp file, flush it, then replace.
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        // The state dir may not exist yet (first run): create it once and retry.
        std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
        CreateDirectoryW(dir.c_str(), nullptr);
        f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
    }
    DWORD w = 0;
    bool ok = WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

ComPtr<IAudioEndpointVolume> endpoint_volume(IMMDevice* d) {
    ComPtr<IAudioEndpointVolume> v;
    if (d) d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &v);
    return v;
}

// Puts one journal entry back. True when the endpoint was found (and written as needed).
bool restore_entry(IMMDeviceEnumerator* e, const Entry& x) {
    ComPtr<IMMDevice> d;
    if (!e || FAILED(e->GetDevice(wide(x.level.device).c_str(), &d))) {
        BLOG_W("mute journal: endpoint %s (%s) not present, entry kept", x.level.device.c_str(), x.level.name.c_str());
        return false;
    }
    if (!write_level(d.Get(), x.level)) return false;
    BLOG_I("restored endpoint %s (%s): mute=%d master=%.3f", x.level.name.c_str(), x.level.device.c_str(), x.level.mute ? 1 : 0, x.level.master);
    return true;
}
}  // namespace

// ------------------------------------------------------------------ levels

bool read_level(IMMDevice* d, EndpointLevel* out) {
    auto v = endpoint_volume(d);
    if (!v) return false;
    BOOL m = FALSE;
    UINT n = 0;
    if (FAILED(v->GetMute(&m)) || FAILED(v->GetMasterVolumeLevelScalar(&out->master)) || FAILED(v->GetChannelCount(&n))) return false;
    out->mute = m != FALSE;
    out->channels.assign(n, 0.f);
    for (UINT i = 0; i < n; i++)
        if (FAILED(v->GetChannelVolumeLevelScalar(i, &out->channels[i]))) return false;
    v->QueryHardwareSupport(&out->hw);
    out->device = device_id(d);
    return true;
}

bool write_level(IMMDevice* d, const EndpointLevel& want) {
    auto v = endpoint_volume(d);
    EndpointLevel cur;
    if (!v || !read_level(d, &cur)) return false;
    bool ok = true;
    auto differs = [](float a, float b) { return std::fabs(a - b) > 1e-6f; };
    if (differs(cur.master, want.master)) ok &= SUCCEEDED(v->SetMasterVolumeLevelScalar(want.master, nullptr));
    if (want.channels.size() == cur.channels.size()) {
        for (UINT i = 0; i < (UINT)want.channels.size(); i++) {
            float now = 0;
            if (SUCCEEDED(v->GetChannelVolumeLevelScalar(i, &now)) && differs(now, want.channels[i]))
                ok &= SUCCEEDED(v->SetChannelVolumeLevelScalar(i, want.channels[i], nullptr));
        }
    }
    if (cur.mute != want.mute) ok &= SUCCEEDED(v->SetMute(want.mute ? TRUE : FALSE, nullptr));
    return ok;
}

// ------------------------------------------------------------------ MuteManager

MuteManager::MuteManager(std::wstring state_dir) : dir_(std::move(state_dir)) {}

MuteManager::~MuteManager() {
    release_all();
    if (guard_) CloseHandle(guard_);  // the guard sees this process exit and finds nothing left to restore
}

std::wstring MuteManager::journal_path() const { return journal_file(dir_); }

int MuteManager::held_count() {
    std::lock_guard<std::mutex> g(m_);
    return (int)held_.size();
}

void MuteManager::write_journal_locked() {
    JournalLock lock;
    std::wstring path = journal_path();
    std::vector<Entry> keep;
    for (auto& e : read_entries(path)) {
        bool mine = e.owner.pid == self_owner().pid && e.owner.created == self_owner().created;
        if (!mine) keep.push_back(e);  // another (live or not yet restored) owner's entries stay untouched
    }
    for (auto& h : held_) keep.push_back(Entry{self_owner(), h.saved});
    if (!write_entries(path, keep)) BLOG_E("mute journal: cannot write %s", utf8(path).c_str());
}

void MuteManager::start_guard_locked() {
    if (guard_) {
        DWORD code = 0;
        if (GetExitCodeProcess(guard_, &code) && code == STILL_ACTIVE) return;
        CloseHandle(guard_);
        guard_ = nullptr;
    }
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" _guard --pid " + std::to_wstring(GetCurrentProcessId());
    if (!dir_.empty()) cmd += L" --state-dir \"" + dir_ + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // No handle inheritance: the guard must not hold Beamer's stdio pipes open (Beamer watches for EOF).
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        guard_ = pi.hProcess;
        CloseHandle(pi.hThread);
        BLOG_I("mute guard started, pid %lu", (unsigned long)pi.dwProcessId);
    } else {
        BLOG_W("mute guard not started (error %lu): a killed helper stays muted until the next start", GetLastError());
    }
}

bool MuteManager::acquire(IMMDevice* device, const std::string& name, std::string* err) {
    std::lock_guard<std::mutex> g(m_);
    std::string id = device_id(device);
    for (auto& h : held_)
        if (h.saved.device == id) { h.refs++; return true; }
    Held h;
    if (!read_level(device, &h.saved)) { if (err) *err = "internal: cannot read the endpoint volume"; return false; }
    h.saved.name = name;
    h.refs = 1;
    held_.push_back(h);
    write_journal_locked();
    // The journal must be on disk before the mute: without it a crash would leave the machine muted.
    {
        JournalLock lock;
        bool found = false;
        for (auto& e : read_entries(journal_path())) found |= e.level.device == id && e.owner.pid == self_owner().pid;
        if (!found) {
            held_.pop_back();
            if (err) *err = "internal: cannot write the mute journal";
            return false;
        }
    }
    start_guard_locked();
    auto v = endpoint_volume(device);
    if (!v || FAILED(v->SetMute(TRUE, nullptr))) {
        held_.pop_back();
        write_journal_locked();
        if (err) *err = "internal: SetMute failed";
        return false;
    }
    if (!(h.saved.hw & ENDPOINT_HARDWARE_SUPPORT_MUTE))
        BLOG_W("endpoint %s mutes in software only: loopback under software mute is unverified", name.c_str());
    BLOG_I("muted endpoint %s (was mute=%d master=%.3f)", name.c_str(), h.saved.mute ? 1 : 0, h.saved.master);
    return true;
}

void MuteManager::release(const std::string& id) {
    std::lock_guard<std::mutex> g(m_);
    for (size_t i = 0; i < held_.size(); i++) {
        if (held_[i].saved.device != id) continue;
        if (--held_[i].refs > 0) return;
        auto e = make_enumerator();
        ComPtr<IMMDevice> d;
        bool ok = e && SUCCEEDED(e->GetDevice(wide(id).c_str(), &d)) && write_level(d.Get(), held_[i].saved);
        if (!ok) {
            // Device gone (unplugged while muted): keep the entry so the next start, or replug + restore, puts it back.
            BLOG_W("endpoint %s not restored now (gone?); left in the journal", held_[i].saved.name.c_str());
            {
                JournalLock lock;
                std::wstring path = journal_path();
                auto all = read_entries(path);
                for (auto& x : all)
                    if (x.level.device == id && x.owner.pid == self_owner().pid) x.owner = Owner{};  // orphan: any start restores it
                write_entries(path, all);
            }
        } else {
            BLOG_I("restored endpoint %s", held_[i].saved.name.c_str());
        }
        held_.erase(held_.begin() + i);
        if (ok) write_journal_locked();
        return;
    }
}

void MuteManager::release_all() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> g(m_);
        for (auto& h : held_) { ids.push_back(h.saved.device); h.refs = 1; }
    }
    for (auto& id : ids) release(id);
}

int MuteManager::restore_journal() {
    JournalLock lock;
    std::wstring path = journal_path();
    auto all = read_entries(path);
    if (all.empty()) return 0;
    auto e = make_enumerator();
    std::vector<Entry> keep;
    int restored = 0;
    for (auto& x : all) {
        bool mine = x.owner.pid == self_owner().pid && x.owner.created == self_owner().created;
        if (mine || (x.owner.pid && owner_alive(x.owner))) { keep.push_back(x); continue; }
        if (restore_entry(e.Get(), x)) restored++;
        else { x.owner = Owner{}; keep.push_back(x); }
    }
    write_entries(path, keep);
    return restored;
}

int MuteManager::restore_entries_of(const std::wstring& state_dir, DWORD owner_pid) {
    JournalLock lock;
    std::wstring path = journal_file(state_dir);
    auto all = read_entries(path);
    auto e = make_enumerator();
    std::vector<Entry> keep;
    int restored = 0;
    for (auto& x : all) {
        if (x.owner.pid != owner_pid) { keep.push_back(x); continue; }
        if (restore_entry(e.Get(), x)) restored++;
        else { x.owner = Owner{}; keep.push_back(x); }
    }
    if (restored || keep.size() != all.size()) write_entries(path, keep);
    return restored;
}

}  // namespace beamer::audio::win
