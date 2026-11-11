// SPDX-License-Identifier: GPL-3.0-only
// Beamer Camera, helper side (camera PHASE2 §4.6.2-§4.6.5, CW4): install state, the kernel objects named after the
// install's InstallGuid, the shared-memory client (heartbeat, live/hold/waiting, seqlocked slots), demand
// (in_use, want, loaded_version) and camera creation.
//
// Who writes what: the source DLL owns the section and the frame event and writes the source fields; this helper
// creates the Demand event and writes helper_pid, beat_qpc, mode, latest and the slots. Everything the source
// writes is validated here before use (§4.6.3, other direction).
//
// Threads: one idle thread (waits on the Demand event and an internal wake event, both auto-reset, so a wait never
// spins; polls every 1 s when Demand is unavailable) and, while a sink is open, the engine's video thread through
// the sink_* calls. All picture state is under mu_. The heartbeat is written by whichever thread owns the picture:
// the idle thread while no sink is open, the sink's caller (the engine's presenter) while one is (§4.6.4).
#include "video_backend_win.h"

#include <aclapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <mfvirtualcamera.h>
#include <sddl.h>

#include <algorithm>
#include <mutex>

namespace beamer::audio::win {

// ------------------------------------------------------------------ helpers
int64_t qpc_now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t qpc_freq() {
    static const int64_t f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x.QuadPart;
    }();
    return f;
}
double qpc_ms(int64_t ticks) { return double(ticks) * 1000.0 / double(qpc_freq()); }
static int64_t ms_ticks(int ms) { return int64_t(ms) * qpc_freq() / 1000; }

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += char(c);
                }
        }
    }
    return o + "\"";
}

bool media_foundation_available() {
    static const bool ok = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
    return ok;
}

bool mf_startup() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = media_foundation_available() && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
    return ok;
}

vcam::MediaType validate_want(uint32_t w, uint32_t h, uint32_t fps) {
    for (const auto& t : vcam::kOffered)
        if (t.w == w && t.h == h && t.fps == fps) return t;
    return vcam::kOffered[0];
}

bool install_guid_ok(const std::wstring& g) {
    if (g.empty() || g.size() > 38) return false;
    for (wchar_t c : g)
        if (!iswxdigit(c) && c != L'-' && c != L'{' && c != L'}') return false;
    return true;
}

// ------------------------------------------------------------------ registry / install state
RegPaths reg_paths(const std::wstring& base) {
    RegPaths p;
    if (base.empty()) {
        p.camera = vcam::kRegKey;
        p.clsid = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + vcam::kClsidText;
    } else {
        p.camera = base + L"\\Camera";
        p.clsid = base + L"\\Classes\\CLSID\\" + vcam::kClsidText;
    }
    return p;
}

static bool reg_str(HKEY root, const std::wstring& key, const wchar_t* name, std::wstring* out) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return false;
    std::wstring v(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, v.data(), &bytes) != ERROR_SUCCESS)
        return false;
    v.resize(wcslen(v.c_str()));
    *out = v;
    return true;
}
static bool reg_dword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD* out) {
    DWORD sz = sizeof(DWORD);
    return RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, out, &sz) == ERROR_SUCCESS;
}
static bool reg_key_exists(HKEY root, const std::wstring& key) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

using PFN_IsSupported = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, BOOL*);
using PFN_Create = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess, LPCWSTR,
                                            LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);
static HMODULE sensorgroup() {
    static HMODULE m = media_foundation_available() ? LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
    return m;
}

bool virtual_camera_supported() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        if (!mf_startup() || !sensorgroup()) return;
        auto f = reinterpret_cast<PFN_IsSupported>(GetProcAddress(sensorgroup(), "MFIsVirtualCameraTypeSupported"));
        if (!f || !GetProcAddress(sensorgroup(), "MFCreateVirtualCamera")) return;
        BOOL s = FALSE;
        ok = SUCCEEDED(f(MFVirtualCameraType_SoftwareCameraSource, &s)) && s;
    });
    return ok;
}

InstallInfo read_install(const std::wstring& base) {
    InstallInfo i;
    RegPaths p = reg_paths(base);
    i.clsid_path.clear();
    if (!virtual_camera_supported()) {
        i.install = "unsupported";
        i.detail = media_foundation_available() ? "MFCreateVirtualCamera unavailable (Windows 11 needed)" : "Media Foundation missing";
        return i;
    }
    bool have_key = reg_key_exists(HKEY_LOCAL_MACHINE, p.camera);
    bool have_clsid = reg_str(HKEY_LOCAL_MACHINE, p.clsid + L"\\InprocServer32", nullptr, &i.clsid_path);
    if (!have_key && !have_clsid) {
        i.install = "missing";
        return i;
    }
    DWORD ver = 0;
    reg_str(HKEY_LOCAL_MACHINE, p.camera, L"InstallGuid", &i.guid);
    reg_str(HKEY_LOCAL_MACHINE, p.camera, L"OwnerSid", &i.owner_sid);
    reg_str(HKEY_LOCAL_MACHINE, p.camera, L"DllPath", &i.dll_path);
    reg_dword(HKEY_LOCAL_MACHINE, p.camera, L"DllVersion", &ver);
    i.dll_version = int(ver);
    PSID sid = nullptr;
    bool sid_ok = !i.owner_sid.empty() && ConvertStringSidToSidW(i.owner_sid.c_str(), &sid);
    if (sid) LocalFree(sid);
    DWORD attr = i.dll_path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(i.dll_path.c_str());
    if (!have_key) i.detail = "camera key missing";
    else if (!have_clsid) i.detail = "COM class missing";
    else if (!install_guid_ok(i.guid)) i.detail = "InstallGuid invalid";
    else if (!sid_ok) i.detail = "OwnerSid invalid";
    else if (i.dll_version <= 0) i.detail = "DllVersion missing";
    else if (_wcsicmp(i.clsid_path.c_str(), i.dll_path.c_str()) != 0) i.detail = "COM class points elsewhere";
    else if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) i.detail = "DLL file missing";
    if (!i.detail.empty()) {
        i.install = "broken";
        return i;
    }
    i.install = vcam::kDllVersion > i.dll_version ? "outdated" : "ok";
    return i;
}

HRESULT create_and_start_camera() {
    if (!virtual_camera_supported()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    auto f = reinterpret_cast<PFN_Create>(GetProcAddress(sensorgroup(), "MFCreateVirtualCamera"));
    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = f(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_CurrentUser,
                   vcam::kFriendlyName, vcam::kClsidText, nullptr, 0, &cam);
    if (FAILED(hr)) return hr;
    return cam->Start(nullptr);  // System lifetime: the camera stays when this object (and the helper) goes
}

HRESULT remove_camera() {
    if (!virtual_camera_supported()) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    auto f = reinterpret_cast<PFN_Create>(GetProcAddress(sensorgroup(), "MFCreateVirtualCamera"));
    ComPtr<IMFVirtualCamera> cam;
    HRESULT hr = f(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_CurrentUser,
                   vcam::kFriendlyName, vcam::kClsidText, nullptr, 0, &cam);
    if (FAILED(hr)) return hr;
    return cam->Remove();
}

// The camera's devnode (SWD\VCAMDEVAPI\<hash>) is a generic "Windows Virtual Camera Device"; the name we gave
// MFCreateVirtualCamera is on its video-camera interface. Present = such an interface exists (CM lists only
// interfaces of started devices with CM_GET_DEVICE_INTERFACE_LIST_PRESENT) and is named "Beamer Camera...".
bool camera_device_present() {
    static const GUID kCategoryVideoCamera = {0xe5323777, 0xf976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}};
    ULONG len = 0;
    GUID cat = kCategoryVideoCamera;
    if (CM_Get_Device_Interface_List_SizeW(&len, &cat, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len <= 1) return false;
    std::wstring list(len, L'\0');
    if (CM_Get_Device_Interface_ListW(&cat, nullptr, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return false;
    for (const wchar_t* path = list.c_str(); *path; path += wcslen(path) + 1) {
        std::wstring p = path;
        for (auto& c : p) c = wchar_t(towupper(c));
        if (p.find(L"VCAMDEVAPI") == std::wstring::npos) continue;
        DEVPROPTYPE type = 0;
        wchar_t name[256] = {};
        ULONG sz = sizeof name;
        if (CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_DeviceInterface_FriendlyName, &type, reinterpret_cast<PBYTE>(name), &sz, 0) !=
            CR_SUCCESS)
            continue;
        if (_wcsnicmp(name, vcam::kFriendlyName, wcslen(vcam::kFriendlyName)) == 0) return true;
    }
    return false;
}

// ------------------------------------------------------------------ kernel objects
struct Vcam::Shm {
    HANDLE demand = nullptr, map = nullptr, frame = nullptr;
    vcam::Header* hdr = nullptr;
    uint8_t* base = nullptr;
    std::wstring guid;
    uint32_t latest = 0;           // our own copy (the shared value is never read back)
    int64_t next_open_try = 0;
    int64_t next_demand_try = 0;
};

static std::wstring obj_name(const std::wstring& guid, const wchar_t* suffix) {
    return L"Global\\BeamerCamera." + guid + suffix;
}

// The section and frame event are the source's (Frame Server, LocalService): only LocalService or SYSTEM may own
// them (§4.6.3, as the DLL checks them). Anything else was made by someone who wants the pictures (R3 #7).
static bool owner_is_service(HANDLE h) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) return false;
    const bool ok = owner && (IsWellKnownSid(owner, WinLocalServiceSid) || IsWellKnownSid(owner, WinLocalSystemSid));
    LocalFree(sd);
    return ok;
}

// The SIDs that may own a Demand event we did not create in this run: our user, or our token's default owner
// (an elevated token's objects are owned by Administrators).
static bool owner_is_us(PSID owner) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    bool ok = false;
    for (TOKEN_INFORMATION_CLASS cls : {TokenUser, TokenOwner}) {
        DWORD n = 0;
        GetTokenInformation(tok, cls, nullptr, 0, &n);
        std::vector<uint8_t> buf(n ? n : 1);
        if (!GetTokenInformation(tok, cls, buf.data(), n, &n)) continue;
        PSID s = cls == TokenUser ? reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid
                                  : reinterpret_cast<TOKEN_OWNER*>(buf.data())->Owner;
        if (EqualSid(s, owner)) ok = true;
    }
    CloseHandle(tok);
    return ok;
}

Vcam::Vcam(const WinVideoOptions& o) : opt_(o), shm_(std::make_unique<Shm>()) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Vcam::~Vcam() {
    stop();
    std::lock_guard<std::mutex> g(mu_);
    close_objects_locked();
    if (wake_) CloseHandle(wake_);
}

void Vcam::close_objects_locked() {
    if (shm_->base) UnmapViewOfFile(shm_->base);
    if (shm_->map) CloseHandle(shm_->map);
    if (shm_->frame) CloseHandle(shm_->frame);
    if (shm_->demand) CloseHandle(shm_->demand);
    *shm_ = Shm();
}

void Vcam::open_objects_locked() {
    const bool usable = (install_.install == "ok" || install_.install == "outdated") && install_guid_ok(install_.guid);
    if (!usable || shm_->guid != install_.guid) {
        if (shm_->demand || shm_->map) close_objects_locked();
        shm_error_.clear();
        demand_error_.clear();
        if (!usable) return;
        shm_->guid = install_.guid;
    }
    const int64_t now = qpc_now();
    if (!shm_->demand && now >= shm_->next_demand_try) {
        shm_->next_demand_try = now + ms_ticks(5000);
        std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;" + install_.owner_sid + L")";
        PSECURITY_DESCRIPTOR sd = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) sa.lpSecurityDescriptor = sd;
        HANDLE h = CreateEventExW(&sa, obj_name(shm_->guid, L".Demand").c_str(), 0, EVENT_ALL_ACCESS);
        DWORD e = GetLastError();
        if (sd) LocalFree(sd);
        if (!h) {
            demand_error_ = e == ERROR_ACCESS_DENIED ? "squatted" : "demand event: error " + std::to_string(e);
        } else if (e == ERROR_ALREADY_EXISTS) {
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR osd = nullptr;
            bool mine = GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &osd) ==
                            ERROR_SUCCESS && owner && owner_is_us(owner);
            if (osd) LocalFree(osd);
            if (mine) {
                shm_->demand = h;
                demand_error_.clear();
            } else {
                CloseHandle(h);
                demand_error_ = "squatted";  // §4.6.3: refused, fall back to polling every 1 s
            }
        } else {
            shm_->demand = h;
            demand_error_.clear();
        }
        if (!demand_error_.empty()) BLOG_W("vcam: Demand event not usable (%s), polling", demand_error_.c_str());
    }
    if (!shm_->map && now >= shm_->next_open_try) {
        shm_->next_open_try = now + ms_ticks(1000);
        HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE | READ_CONTROL, FALSE, obj_name(shm_->guid, L".Shm").c_str());
        if (!m) return;  // the source is not activated (no app has used the camera since Frame Server started)
        if (!owner_is_service(m)) {
            // Not the source's: never write a picture into it (R3 #7). Retried every 1 s (the real one may come).
            CloseHandle(m);
            if (shm_error_ != "squatted") BLOG_W("vcam: the shared-memory section is not owned by LocalService or SYSTEM: refused (squatted)");
            shm_error_ = "squatted";
            return;
        }
        void* v = MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, SIZE_T(vcam::kSectionBytes));
        if (!v) {
            CloseHandle(m);
            shm_error_ = "outdated";  // smaller than this helper's layout
            return;
        }
        auto* hdr = static_cast<vcam::Header*>(v);
        const uint32_t magic = *reinterpret_cast<volatile uint32_t*>(&hdr->magic);
        const uint32_t version = *reinterpret_cast<volatile uint32_t*>(&hdr->version);
        const uint32_t slots = *reinterpret_cast<volatile uint32_t*>(&hdr->slot_count);
        if (magic != vcam::kMagic || version < 1 || version > vcam::kVersion || slots != vcam::kSlotCount) {
            UnmapViewOfFile(v);
            CloseHandle(m);
            if (shm_error_ != "outdated") BLOG_W("vcam: unknown shared-memory header (magic %08x version %u slots %u)", magic, version, slots);
            shm_error_ = "outdated";
            return;
        }
        shm_error_.clear();
        shm_->map = m;
        shm_->base = static_cast<uint8_t*>(v);
        shm_->hdr = hdr;
        shm_->frame = OpenEventW(EVENT_MODIFY_STATE | READ_CONTROL, FALSE, obj_name(shm_->guid, L".Frame").c_str());
        if (shm_->frame && !owner_is_service(shm_->frame)) {  // a foreign event: not signalled (the source polls)
            CloseHandle(shm_->frame);
            shm_->frame = nullptr;
            BLOG_W("vcam: the frame event is not owned by LocalService or SYSTEM: not used");
        }
        hdr->helper_pid = GetCurrentProcessId();
        if (!sink_open_) beat_locked(now);  // the idle loop owns the picture: alive from the first delivery
        seen_state_seq_ = hdr->state_seq.load(std::memory_order_acquire) - 1;  // read the state now
        BLOG_I("vcam: shared memory open (version %u)", version);
        // Put the current picture state into the fresh section.
        if (show_ == Show::kPlaceholder && !ph_nv12_.empty()) {
            std::string st = ph_state_;
            ph_key_.clear();
            show_placeholder_locked(st, true);
        }
    }
}

void Vcam::read_source_locked(int64_t now, bool* changed) {
    vcam::Header* h = shm_->hdr;
    if (h) {
        const uint32_t seq = h->state_seq.load(std::memory_order_acquire);
        if (seq != seen_state_seq_) {
            seen_state_seq_ = seq;
            const uint32_t s = *reinterpret_cast<volatile uint32_t*>(&h->streaming);
            const uint32_t ww = *reinterpret_cast<volatile uint32_t*>(&h->want_w);
            const uint32_t wh = *reinterpret_cast<volatile uint32_t*>(&h->want_h);
            const uint32_t wf = *reinterpret_cast<volatile uint32_t*>(&h->want_fps);
            const uint32_t dv = *reinterpret_cast<volatile uint32_t*>(&h->dll_version);
            streaming_ = s == 1;  // anything but 0/1 is treated as "not streaming"
            vcam::MediaType w = validate_want(ww, wh, wf);
            if (w.w != want_.w || w.h != want_.h) {
                want_ = w;
                if (in_use_) *changed = true;
            }
            int lv = dv >= 1 && dv <= 100000 ? int(dv) : 0;
            if (lv != loaded_version_) {
                loaded_version_ = lv;
                *changed = true;
            }
        }
    }
    if (streaming_) {
        off_since_ = 0;
        if (!in_use_) {
            in_use_ = true;  // on at once (§3.3)
            app_read_at_ = 0;
            *changed = true;
        }
    } else if (in_use_) {
        if (!off_since_) off_since_ = now;
        if (now - off_since_ >= ms_ticks(opt_.in_use_off_ms)) {  // Frame Server re-creates the source: debounce
            in_use_ = false;
            off_since_ = 0;
            app_.clear();
            *changed = true;
        }
    }
}

void Vcam::beat_locked(int64_t now) {
    if (shm_->hdr) shm_->hdr->beat_qpc.store(uint64_t(now), std::memory_order_release);
}

static void seqlock_begin(vcam::Slot& s, uint32_t* q) {
    uint32_t v = s.seq.load(std::memory_order_relaxed);
    if (v & 1) v++;
    *q = v;
    s.seq.store(v + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

void Vcam::write_live_locked(const uint8_t* nv12, int w, int h) {
    vcam::Header* hdr = shm_->hdr;
    if (!hdr) return;
    const uint32_t i = (shm_->latest + 1) % vcam::kLiveSlots;
    vcam::Slot& s = hdr->slots[i];
    uint32_t q;
    seqlock_begin(s, &q);
    memcpy(shm_->base + vcam::kDataOffset + uint64_t(i) * vcam::kSlotBytes, nv12, size_t(w) * h * 3 / 2);
    s.w = uint32_t(w);
    s.h = uint32_t(h);
    s.stride = uint32_t(w);
    s.written_qpc = uint64_t(qpc_now());
    s.picture = ++picture_;
    s.seq.store(q + 2, std::memory_order_release);
    shm_->latest = i;
    hdr->latest.store(i, std::memory_order_release);
    hdr->mode.store(vcam::kModeLive, std::memory_order_release);
    if (shm_->frame) SetEvent(shm_->frame);
}

void Vcam::show_placeholder_locked(const std::string& state, bool force) {
    const vcam::MediaType size = in_use_ ? want_ : vcam::kOffered[0];
    const std::string key = state + "|" + std::to_string(size.w) + "x" + std::to_string(size.h) + "|" + std::to_string(texts_version_);
    bool wrote = false;
    if (key != ph_key_ || force) {
        if (key != ph_key_) {
            Placeholder p;
            auto it = texts_.find(state);
            if (it != texts_.end()) p = it->second;
            if (p.title.empty()) p.title = "Beamer Camera";  // a state without texts (§4.6.4)
            std::string err;
            std::vector<uint8_t> buf;
            if (!render_placeholder(p, int(size.w), int(size.h), &buf, &err)) {
                BLOG_W("vcam: placeholder render failed: %s", err.c_str());
                buf.assign(size_t(size.w) * size.h * 3 / 2, 0);
                std::fill(buf.begin(), buf.begin() + size_t(size.w) * size.h, uint8_t(24));
                std::fill(buf.begin() + size_t(size.w) * size.h, buf.end(), uint8_t(128));
            }
            ph_nv12_.swap(buf);
            renders_++;
        }
        ph_key_ = key;
        if (vcam::Header* hdr = shm_->hdr) {
            vcam::Slot& s = hdr->slots[vcam::kLiveSlots];
            uint32_t q;
            seqlock_begin(s, &q);
            memcpy(shm_->base + vcam::kDataOffset + uint64_t(vcam::kLiveSlots) * vcam::kSlotBytes, ph_nv12_.data(), ph_nv12_.size());
            s.w = size.w;
            s.h = size.h;
            s.stride = size.w;
            s.written_qpc = uint64_t(qpc_now());
            s.picture = ++picture_;
            s.seq.store(q + 2, std::memory_order_release);
            wrote = true;
        } else {
            ph_key_.clear();  // nothing written: write it when the section opens
        }
    }
    show_ = Show::kPlaceholder;
    ph_state_ = state;
    if (vcam::Header* hdr = shm_->hdr) {
        const bool was = hdr->mode.load(std::memory_order_relaxed) == vcam::kModeWaiting;
        hdr->mode.store(vcam::kModeWaiting, std::memory_order_release);
        if ((wrote || !was) && shm_->frame) SetEvent(shm_->frame);
    }
}

void Vcam::tick_locked(int64_t now, bool idle) {
    if (close_hold_until_ && now >= close_hold_until_) {
        close_hold_until_ = 0;
        std::string st = pending_ph_.empty() ? (sink_open_ ? std::string() : idle_) : pending_ph_;
        pending_ph_.clear();
        if (!st.empty()) show_placeholder_locked(st, false);
    }
    if (!sink_open_ && show_ == Show::kNone) show_placeholder_locked(idle_, false);
    // A placeholder follows the app's size and the texts (re-rendered only when its key changes).
    if (show_ == Show::kPlaceholder) show_placeholder_locked(ph_state_, false);
    if (idle && !sink_open_) beat_locked(now);
}

// ------------------------------------------------------------------ public API
void Vcam::start(VideoBackendListener* l) {
    {
        {
            std::lock_guard<std::mutex> lg(lmu_);
            listener_ = l;
        }
        std::lock_guard<std::mutex> g(mu_);
        install_ = read_install(opt_.reg_base);
        open_objects_locked();
        show_placeholder_locked(idle_, false);
    }
    bool present = camera_device_present();
    {
        std::lock_guard<std::mutex> g(mu_);
        present_ = present;
    }
    thread_ = std::thread([this] { run(); });
    kick_create(true);  // at ready (§4.6.5)
}

void Vcam::stop() {
    quit_ = true;
    if (wake_) SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    if (create_thread_.joinable()) create_thread_.join();
}

void Vcam::notify() {
    std::lock_guard<std::mutex> g(lmu_);
    if (listener_) listener_->vcam_changed();
}

void Vcam::detach_listener() {
    std::lock_guard<std::mutex> g(lmu_);
    listener_ = nullptr;
}

void Vcam::poll() {
    bool changed = false;
    std::lock_guard<std::mutex> g(mu_);
    install_ = read_install(opt_.reg_base);
    open_objects_locked();
    read_source_locked(qpc_now(), &changed);
}

std::string vcam_event_json(const VcamState& s) {
    std::string o = "{\"ev\":\"vcam\",\"install\":" + json_str(s.install) + ",\"dll_version\":" + std::to_string(s.dll_version) +
                    ",\"loaded_version\":" + std::to_string(s.loaded_version) + ",\"bundled_version\":" + std::to_string(s.bundled_version) +
                    ",\"present\":" + (s.present ? "true" : "false") + ",\"creating\":" + (s.creating ? "true" : "false") +
                    ",\"in_use\":" + (s.in_use ? "true" : "false") + ",\"app\":" + (s.app.empty() ? "null" : json_str(s.app)) +
                    ",\"want\":";
    if (s.in_use && s.want.w > 0)
        o += "{\"w\":" + std::to_string(s.want.w) + ",\"h\":" + std::to_string(s.want.h) + ",\"fps\":" + std::to_string(s.want.fps) + "}";
    else
        o += "null";
    o += ",\"error\":" + (s.error.empty() ? std::string("null") : json_str(s.error)) + "}";
    return o;
}

void Vcam::post_demand_check() {
    if (wake_) SetEvent(wake_);
}

void Vcam::run() {
    SetThreadDescription(GetCurrentThread(), L"vcam");
    ComScope com;
    while (!quit_) {
        HANDLE hs[2];
        DWORD n = 0;
        hs[n++] = wake_;
        int wait_ms;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (shm_->demand) hs[n++] = shm_->demand;
            wait_ms = sink_open_ ? 100 : (in_use_ || close_hold_until_ ? 40 : 100);
        }
        // Both events are auto-reset: a wait consumes the signal, so this loop never spins (C4 surprise 6).
        DWORD r = WaitForMultipleObjects(n, hs, FALSE, DWORD(wait_ms));
        if (quit_) break;
        const int64_t now = qpc_now();
        bool changed = false, read_app = false;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!shm_->map || !shm_->demand) {
                if (r == WAIT_OBJECT_0 + 1) shm_->next_open_try = 0;  // Demand fired: try at once
                open_objects_locked();
            }
            read_source_locked(now, &changed);
            tick_locked(now, true);
            read_app = in_use_ && now >= app_read_at_;
            if (read_app) app_read_at_ = now + ms_ticks(2000);
        }
        if (changed) notify();
        if (read_app) {
            std::string app = camera_app_in_use(opt_.usage_root);
            bool ch = false;
            {
                std::lock_guard<std::mutex> g(mu_);
                if (in_use_ && app != app_) {
                    app_ = app;
                    ch = true;
                }
            }
            if (ch) notify();
        }
    }
}

void Vcam::kick_create(bool force) {
    const int64_t now = qpc_now();
    std::string sig;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (install_.install != "ok" || present_ || creating_) return;
        sig = utf8(install_.guid) + "|" + std::to_string(install_.dll_version);
        // Automatic attempts (vcam() at every `vcam` command) are rate-limited so that a failing creation and the
        // vcam event it triggers cannot loop.
        if (!force && last_create_try_ && now - last_create_try_ < ms_ticks(2000) && created_for_ == sig) return;
        last_create_try_ = now;
        created_for_ = sig;
        creating_ = true;
        create_attempts_++;
    }
    if (create_thread_.joinable()) create_thread_.join();
    notify();  // creating: true
    create_thread_ = std::thread([this] {
        SetThreadDescription(GetCurrentThread(), L"vcam-create");
        ComScope com;
        HRESULT hr = S_OK;
        const int64_t t0 = qpc_now();
        if (opt_.create_camera) hr = create_and_start_camera();
        bool present = opt_.create_camera ? SUCCEEDED(hr) || camera_device_present() : camera_device_present();
        {
            std::lock_guard<std::mutex> g(mu_);
            create_error_ = FAILED(hr) ? "create: " + hr_text(hr) : std::string();
            present_ = present;
        }
        BLOG_I("vcam: camera create %s (%.0f ms)%s", FAILED(hr) ? hr_text(hr).c_str() : "ok", qpc_ms(qpc_now() - t0),
               opt_.create_camera ? "" : " [creation disabled]");
        creating_ = false;
        notify();
    });
}

VcamState Vcam::state(bool trigger_create) {
    bool present = camera_device_present();
    {
        std::lock_guard<std::mutex> g(mu_);
        InstallInfo i = read_install(opt_.reg_base);
        if (i.install != install_.install || i.guid != install_.guid) BLOG_I("vcam: install %s -> %s", install_.install.c_str(), i.install.c_str());
        install_ = i;
        if (!creating_) present_ = present;
        open_objects_locked();
    }
    if (trigger_create) kick_create(false);
    std::lock_guard<std::mutex> g(mu_);
    VcamState s;
    s.install = install_.install;
    if (shm_error_ == "outdated" && (s.install == "ok" || s.install == "outdated")) s.install = "outdated";
    s.dll_version = install_.dll_version;
    s.loaded_version = loaded_version_;
    s.bundled_version = vcam::kDllVersion;
    s.present = present_;
    s.in_use = in_use_;
    s.creating = creating_;
    s.app = in_use_ ? app_ : std::string();
    if (in_use_) s.want = VideoFormat{int(want_.w), int(want_.h), int(want_.fps)};
    else s.want = VideoFormat{0, 0, 0};
    if (!create_error_.empty()) s.error = create_error_;
    else if (shm_error_ == "squatted") s.error = "squatted";
    else if (!demand_error_.empty()) s.error = demand_error_;
    else if (install_.install == "broken") s.error = install_.detail;
    return s;
}

bool Vcam::enable(bool on, std::string* err) {
    if (on) {
        {
            std::lock_guard<std::mutex> g(mu_);
            install_ = read_install(opt_.reg_base);
            open_objects_locked();
            if (install_.install != "ok") {
                if (err) *err = "not_installed: Beamer Camera is " + install_.install;
                return false;
            }
        }
        bool present = camera_device_present();
        {
            std::lock_guard<std::mutex> g(mu_);
            present_ = present;
        }
        kick_create(true);
        return true;
    }
    HRESULT hr = opt_.create_camera ? remove_camera() : S_OK;
    {
        std::lock_guard<std::mutex> g(mu_);
        present_ = false;
    }
    notify();
    if (FAILED(hr)) {
        if (err) *err = "internal: Remove " + hr_text(hr);
        return false;
    }
    return true;
}

void Vcam::set_placeholders(const std::map<std::string, Placeholder>& texts, const std::string& idle) {
    std::lock_guard<std::mutex> g(mu_);
    texts_ = texts;
    if (!idle.empty()) idle_ = idle;
    texts_version_++;
    if (!sink_open_ && !close_hold_until_) show_placeholder_locked(idle_, false);  // on screen at once (<= 100 ms)
    else if (show_ == Show::kPlaceholder) show_placeholder_locked(ph_state_, false);
}

void Vcam::sink_opened() {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t now = qpc_now();
    sink_open_ = true;
    if (close_hold_until_ > now) pending_ph_ = "starting";  // the previous stream's picture stays until 500 ms
    else show_placeholder_locked("starting", false);
    beat_locked(now);
}

void Vcam::sink_closed() {
    {
        std::lock_guard<std::mutex> g(mu_);
        const int64_t now = qpc_now();
        sink_open_ = false;
        pending_ph_.clear();
        if (have_live_ && (show_ == Show::kLive || show_ == Show::kHold)) {
            show_ = Show::kHold;  // last picture for 500 ms, then the idle placeholder (§4.6.4)
            if (shm_->hdr) shm_->hdr->mode.store(vcam::kModeHold, std::memory_order_release);
            if (!close_hold_until_) close_hold_until_ = now + ms_ticks(opt_.close_hold_ms);
        } else {
            close_hold_until_ = 0;
            show_placeholder_locked(idle_, false);
        }
        beat_locked(now);
    }
    SetEvent(wake_);
}

void Vcam::sink_present(const uint8_t* nv12, int w, int h) {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t now = qpc_now();
    write_live_locked(nv12, w, h);
    show_ = Show::kLive;
    have_live_ = true;
    close_hold_until_ = 0;
    pending_ph_.clear();
    beat_locked(now);
}

void Vcam::sink_hold() {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t now = qpc_now();
    tick_locked(now, false);
    if (show_ == Show::kLive) {
        show_ = Show::kHold;
        if (shm_->hdr) shm_->hdr->mode.store(vcam::kModeHold, std::memory_order_release);
    }
    beat_locked(now);
}

void Vcam::sink_placeholder(const std::string& state) {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t now = qpc_now();
    if (close_hold_until_ > now) pending_ph_ = state;
    else show_placeholder_locked(state, false);
    beat_locked(now);
}

std::string Vcam::shown() {
    std::lock_guard<std::mutex> g(mu_);
    switch (show_) {
        case Show::kLive: return "live";
        case Show::kHold: return "hold";
        case Show::kPlaceholder: return "ph:" + ph_state_;
        default: return "none";
    }
}

}  // namespace beamer::audio::win
