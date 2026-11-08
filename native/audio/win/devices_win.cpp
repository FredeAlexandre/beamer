// SPDX-License-Identifier: GPL-3.0-only
// Windows backend: utilities, logging, MMDevice inventory and classification (transport, form, role, blocklist).
// This translation unit defines the GUIDs of the Windows audio headers (initguid.h first).
#include <initguid.h>
#include "backend_win.h"

#include <avrt.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "propsys.lib")

namespace beamer::audio::win {

// ------------------------------------------------------------------ logging

static std::atomic<int> g_level{(int)LogLevel::Info};
static std::mutex g_log_mutex;

void set_log_level(const std::string& level) {
    std::string l = lower(level);
    g_level = l == "debug" ? 0 : l == "warn" ? 2 : l == "error" ? 3 : 1;
}

void logf(LogLevel level, const char* fmt, ...) {
    if ((int)level < g_level.load()) return;
    static const char* names[] = {"debug", "info", "warn", "error"};
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s: ", names[(int)level]);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    size_t len = std::min<size_t>(sizeof buf - 2, (size_t)n + (m > 0 ? (size_t)m : 0));
    for (size_t i = n; i < len; i++) if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';  // one line per message
    buf[len++] = '\n';
    std::lock_guard<std::mutex> g(g_log_mutex);
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, (DWORD)len, &w, nullptr);
}

// ------------------------------------------------------------------ strings, time, COM

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::string utf8(const wchar_t* w) { return w ? utf8(std::wstring(w)) : std::string(); }
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string hr_text(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof b, "0x%08lx", (unsigned long)hr);
    return b;
}
static int64_t qpc_freq() {
    static int64_t f = [] { LARGE_INTEGER l; QueryPerformanceFrequency(&l); return l.QuadPart; }();
    return f;
}
uint64_t now_ns() {
    LARGE_INTEGER l;
    QueryPerformanceCounter(&l);
    int64_t f = qpc_freq();
    return (uint64_t)(l.QuadPart / f) * 1000000000ull + (uint64_t)((l.QuadPart % f) * 1000000000ll / f);
}
uint64_t qpc100_to_ns(uint64_t t100) { return t100 * 100ull; }

void ensure_com() {
    thread_local bool done = false;
    if (done) return;
    done = true;
    // S_OK / S_FALSE: initialized (balanced at thread exit by the OS); RPC_E_CHANGED_MODE: the thread is STA, fine.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

MmcssScope::MmcssScope() { h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx); }
MmcssScope::~MmcssScope() { if (h) AvRevertMmThreadCharacteristics(h); }

// ------------------------------------------------------------------ properties

// Not in the SDK headers under these names: the audio adapter's friendly name and devnode instance id as the
// endpoint property store carries them ({1}.<instance id>).
static const PROPERTYKEY kAdapterName = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 6};
static const PROPERTYKEY kAdapterInstance = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};

static std::string prop_string(IPropertyStore* ps, const PROPERTYKEY& k) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string s;
    if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt == VT_LPWSTR) s = utf8(v.pwszVal);
    PropVariantClear(&v);
    return s;
}
static int prop_u32(IPropertyStore* ps, const PROPERTYKEY& k, int dflt) {
    PROPVARIANT v;
    PropVariantInit(&v);
    int r = dflt;
    if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt == VT_UI4) r = (int)v.ulVal;
    PropVariantClear(&v);
    return r;
}

static std::string devnode_string(DEVINST di, const DEVPROPKEY& k) {
    DEVPROPTYPE t;
    wchar_t buf[512];
    ULONG n = sizeof buf;
    if (CM_Get_DevNode_PropertyW(di, &k, &t, (PBYTE)buf, &n, 0) == CR_SUCCESS && t == DEVPROP_TYPE_STRING) return utf8(buf);
    return {};
}

static bool starts_with_ci(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && lower(s.substr(0, prefix.size())) == lower(prefix);
}
static bool contains_ci(const std::string& s, const std::string& needle) { return lower(s).find(lower(needle)) != std::string::npos; }

// ------------------------------------------------------------------ blocklist

struct BlockRule { int kind; const char* pattern; };  // 0 name (exact), 1 adapter prefix, 2 ancestor prefix
#define BLOCK_NAME(x) {0, x},
#define BLOCK_ADAPTER(x) {1, x},
#define BLOCK_ANCESTOR(x) {2, x},
static const BlockRule kBlockRules[] = {
#include "blocklist.inc"
};
#undef BLOCK_NAME
#undef BLOCK_ADAPTER
#undef BLOCK_ANCESTOR

static std::string block_reason(const Endpoint& ep) {
    for (const auto& r : kBlockRules) {
        if (r.kind == 0 && lower(ep.name) == lower(r.pattern)) return std::string("blocklist name ") + r.pattern;
        if (r.kind == 1 && starts_with_ci(ep.adapter_instance, r.pattern)) return std::string("blocklist adapter ") + r.pattern;
        if (r.kind == 2)
            for (const auto& a : ep.ancestors)
                if (starts_with_ci(a, r.pattern)) return std::string("blocklist ancestor ") + r.pattern;
    }
    bool elgato = contains_ci(ep.name, "elgato") || contains_ci(ep.adapter, "elgato") || contains_ci(ep.manufacturer, "elgato");
    if (elgato && ep.info.transport == "virtual") return "Elgato virtual endpoint";
    return {};
}

// ------------------------------------------------------------------ classification

bool is_vbcable(const Endpoint& ep) {
    return contains_ci(ep.adapter, "VB-Audio Virtual Cable") || contains_ci(ep.name, "(VB-Audio Virtual Cable)");
}
bool is_vbcable_input(const Endpoint& ep) { return ep.render && is_vbcable(ep) && lower(ep.description) == "cable input"; }
bool is_vbcable_output(const Endpoint& ep) { return !ep.render && is_vbcable(ep) && lower(ep.description) == "cable output"; }

// True for a devnode on a hardware bus root (where a physical device's chain meets the machine).
static bool is_hardware_root(const std::string& id) { return starts_with_ci(id, "PCI\\") || starts_with_ci(id, "ACPI\\"); }

static std::string transport_of(const Endpoint& ep) {
    // Walk up from the audio adapter. A physical device reaches PCI\ or ACPI\ (its host controller) first; a virtual
    // one reaches a root-enumerated (ROOT\) or software (SWD\) node first, even when an emulated USB node sits in
    // between (Elgato Wave Link: USB\VID_0FD9&PID_0096 under ROOT\ELGATO_ROOT_DEVICE). Every chain eventually passes
    // ROOT\ACPI_HAL and HTREE\ROOT, so only the first of these nodes counts.
    bool usb = false, bth = false, hda = false;
    for (const auto& a : ep.ancestors) {
        if (starts_with_ci(a, "BTH")) bth = true;  // BTHENUM\, BTHHFENUM\, BTHLEDEVICE\, BTHLE\, BTH\...
        else if (starts_with_ci(a, "USB\\") || starts_with_ci(a, "USBSTOR\\")) usb = true;
        else if (starts_with_ci(a, "HDAUDIO\\") || starts_with_ci(a, "INTELAUDIO\\")) hda = true;
        if (is_hardware_root(a)) {
            if (bth) return "bluetooth";
            if (usb) return "usb";
            return ep.form_factor == 9 /* DigitalAudioDisplayDevice */ ? "hdmi" : "builtin";
        }
        if ((starts_with_ci(a, "ROOT\\") || starts_with_ci(a, "SWD\\") || starts_with_ci(a, "HTREE\\"))) return "virtual";
    }
    if (bth) return "bluetooth";
    if (usb) return "usb";
    if (hda) return ep.form_factor == 9 ? "hdmi" : "builtin";
    if (ep.ancestors.empty()) {
        std::string e = lower(ep.enumerator);
        if (e == "root" || e == "swd" || e == "mmdevapi") return "virtual";
    }
    return "other";
}

static Form form_of(const Endpoint& ep) {
    // PKEY_AudioEndpoint_FormFactor: Speakers 1, LineLevel 2, Headphones 3, Microphone 4, Headset 5, Handset 6,
    // DigitalAudioDisplayDevice 9. Drivers are often vague (the G733's speakers report Speakers, the Wave:3's
    // headphone jack LineLevel), so names refine it: an adapter called "...Headset" is a headset, a jack described
    // as "Headphones"/"Casque" is headphones. The user can still override per device (route `forms`).
    const std::string d = lower(ep.description), a = lower(ep.adapter);
    bool headset_name = a.find("headset") != std::string::npos || d.find("headset") != std::string::npos ||
                        d.find("micro-casque") != std::string::npos || d.find("sur casque") != std::string::npos;
    bool headphone_name = d.find("headphone") != std::string::npos || d == "casque" || d.rfind("casque ", 0) == 0;
    switch (ep.form_factor) {
        case 3: return Form::Headphones;
        case 5: case 6: return Form::Headset;
        case 4: return headset_name ? Form::Headset : Form::Microphone;
        default: break;
    }
    if (headset_name && (ep.info.transport == "usb" || ep.info.transport == "bluetooth")) return Form::Headset;
    if (headphone_name) return ep.render ? Form::Headphones : Form::Unknown;
    if (ep.form_factor == 1 || ep.form_factor == 9) return Form::Speakers;
    return ep.render ? Form::Unknown : Form::Microphone;
}

std::string form_word(Form f) {
    switch (f) {
        case Form::Microphone: return "microphone";
        case Form::Speakers: return "speakers";
        case Form::Headphones: return "headphones";
        case Form::Headset: return "headset";
        default: return "unknown";
    }
}

static void read_devnode_chain(Endpoint& ep) {
    std::string inst = ep.adapter_instance;
    if (inst.empty()) return;
    DEVINST di;
    if (CM_Locate_DevNodeW(&di, (DEVINSTID_W)wide(inst).c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return;
    ep.driver_provider = devnode_string(di, DEVPKEY_Device_DriverProvider);
    bool below_bus = true;  // manufacturer only from the device itself, not its hubs and host controller
    for (int depth = 0; depth < 16; depth++) {
        wchar_t id[MAX_DEVICE_ID_LEN + 1];
        if (CM_Get_Device_IDW(di, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
        ep.ancestors.push_back(utf8(id));
        if (is_hardware_root(ep.ancestors.back()) || starts_with_ci(ep.ancestors.back(), "USB\\ROOT_HUB")) below_bus = false;
        if (ep.manufacturer.empty() && below_bus) {
            std::string m = devnode_string(di, DEVPKEY_Device_Manufacturer);
            if (!m.empty() && m[0] != '(' && lower(m) != "microsoft") ep.manufacturer = m;
        }
        DEVINST p;
        if (CM_Get_Parent(&p, di, 0) != CR_SUCCESS) break;
        di = p;
    }
}

ComPtr<IMMDeviceEnumerator> make_enumerator() {
    ensure_com();
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    if (FAILED(hr)) BLOG_E("MMDeviceEnumerator: %s", hr_text(hr).c_str());
    return e;
}

std::string device_id(IMMDevice* d) {
    LPWSTR id = nullptr;
    std::string s;
    if (d && SUCCEEDED(d->GetId(&id))) { s = utf8(id); CoTaskMemFree(id); }
    return s;
}

Endpoint describe_endpoint(IMMDevice* d, EDataFlow flow, bool read_mix_format) {
    Endpoint ep;
    ep.id = device_id(d);
    ep.render = flow == eRender;
    d->GetState(&ep.state);
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        ep.name = prop_string(ps.Get(), PKEY_Device_FriendlyName);
        ep.description = prop_string(ps.Get(), PKEY_Device_DeviceDesc);
        ep.adapter = prop_string(ps.Get(), kAdapterName);
        if (ep.adapter.empty()) ep.adapter = prop_string(ps.Get(), PKEY_DeviceInterface_FriendlyName);
        ep.enumerator = prop_string(ps.Get(), PKEY_Device_EnumeratorName);
        ep.adapter_instance = prop_string(ps.Get(), kAdapterInstance);
        if (ep.adapter_instance.rfind("{1}.", 0) == 0) ep.adapter_instance = ep.adapter_instance.substr(4);
        ep.form_factor = prop_u32(ps.Get(), PKEY_AudioEndpoint_FormFactor, -1);
        if (read_mix_format) {
            // The engine's device format as a property: no IAudioClient is activated (blocked endpoints stay untouched).
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                auto* f = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
                ep.mix_channels = f->nChannels;
                ep.mix_rate = (int)f->nSamplesPerSec;
                ep.mix_float = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                               (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && v.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) &&
                                reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            }
            PropVariantClear(&v);
        }
    }
    read_devnode_chain(ep);
    DeviceInfo& in = ep.info;
    in.key = ep.id;
    in.name = ep.name;
    in.manufacturer = ep.manufacturer;
    in.input = !ep.render;
    in.output = ep.render;
    in.channels = ep.mix_channels;
    in.nominal_rate = ep.mix_rate;
    in.transport = transport_of(ep);
    in.form = form_of(ep);
    in.role = Role::Normal;
    ep.why = block_reason(ep);
    ep.blocked = !ep.why.empty();
    if (!ep.blocked && is_vbcable(ep)) {
        if (is_vbcable_input(ep)) { in.role = Role::VirtualMicFeed; ep.why = "VB-CABLE: virtual mic sink"; }
        else if (is_vbcable_output(ep)) { in.role = Role::VirtualMic; ep.why = "VB-CABLE: what apps record"; }
        else { ep.hidden = true; ep.why = "VB-CABLE endpoint not used by Beamer"; }
    }
    if (!ep.blocked && ep.state != DEVICE_STATE_ACTIVE) { ep.hidden = true; if (ep.why.empty()) ep.why = "not active"; }
    return ep;
}

std::vector<Endpoint> enumerate_endpoints(IMMDeviceEnumerator* e, DWORD state_mask, bool read_mix_format) {
    std::vector<Endpoint> out;
    if (!e) return out;
    for (EDataFlow flow : {eCapture, eRender}) {
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(e->EnumAudioEndpoints(flow, state_mask, &col))) continue;
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            ComPtr<IMMDevice> d;
            if (SUCCEEDED(col->Item(i, &d))) out.push_back(describe_endpoint(d.Get(), flow, read_mix_format));
        }
    }
    return out;
}

std::string default_endpoint_id(IMMDeviceEnumerator* e, EDataFlow flow) {
    ComPtr<IMMDevice> d;
    if (!e || FAILED(e->GetDefaultAudioEndpoint(flow, eConsole, &d))) return {};
    return device_id(d.Get());
}

ComPtr<IMMDevice> device_by_id(IMMDeviceEnumerator* e, const std::string& id) {
    ComPtr<IMMDevice> d;
    if (!e || id.empty() || FAILED(e->GetDevice(wide(id).c_str(), &d))) return nullptr;
    DWORD st = 0;
    if (FAILED(d->GetState(&st)) || st != DEVICE_STATE_ACTIVE) return nullptr;
    return d;
}

DeviceSnapshot build_snapshot(IMMDeviceEnumerator* e, bool vmic_in_use, std::vector<Endpoint>* all_out) {
    DeviceSnapshot s;
    std::vector<Endpoint> all = enumerate_endpoints(e, DEVICE_STATE_ACTIVE, true);
    std::string din = default_endpoint_id(e, eCapture), dout = default_endpoint_id(e, eRender);
    bool cable_in = false, cable_out = false, any_cable = false;
    for (const auto& ep : all) {
        if (ep.blocked) continue;
        if (is_vbcable(ep)) any_cable = true;
        if (ep.info.role == Role::VirtualMicFeed) cable_in = true;
        if (ep.info.role == Role::VirtualMic) cable_out = true;
        if (ep.hidden) continue;
        s.devices.push_back(ep.info);
        // A default that is not an ordinary device (blocked, VB-CABLE) is reported as none: following it would capture
        // the virtual mic as a mic, or mute Wave Link's virtual output.
        if (ep.id == din && ep.info.role == Role::Normal) s.default_input = din;
        if (ep.id == dout && ep.info.role == Role::Normal) s.default_output = dout;
    }
    s.vmic_kind = any_cable ? "vbcable" : "";
    s.vmic_present = cable_in && cable_out;
    s.vmic_in_use = s.vmic_present && vmic_in_use;
    if (all_out) *all_out = std::move(all);
    return s;
}

}  // namespace beamer::audio::win
