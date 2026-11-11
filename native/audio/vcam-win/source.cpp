// SPDX-License-Identifier: GPL-3.0-only
// BeamerCameraSource.dll: the Media Foundation media source behind the "Beamer Camera" virtual camera
// (PHASE2 §4.6.2-4.6.4). Frame Server (svchost -k Camera, LocalService) loads it through the COM class
// kClsidText; the helper (beamer-audio.exe, user session) feeds it through the shared memory of
// shm_protocol.h. No network, no decode, no crypto.
//
// What it does:
//  - activate -> IMFMediaSourceEx + IMFGetService + IKsControl -> one IMFMediaStream2, shared stream, NV12 at
//    the kOffered types; a delivery thread hands Frame Server a sample for each pending RequestSample;
//  - owns the section and the frame event (Global\BeamerCamera.<InstallGuid>.Shm / .Frame) with a DACL of
//    SYSTEM, LocalService and the owner SID (HKLM kRegKey), refuses objects someone else created first;
//  - publishes Start/Stop and the app's type in the header and sets the helper's .Demand event;
//  - treats every helper-written byte as hostile (§4.6.3): constants for geometry, each field read once and
//    validated, slots copied under the seqlock into a private buffer, scaling only from that buffer;
//  - shows the built-in image when nobody feeds it (beat older than 500 ms), slot 3 in waiting mode, the
//    newest live slot otherwise (§4.6.4);
//  - outside Frame Server (MFCreateVirtualCamera validates the DLL in the calling process) it is inert: valid
//    media types, no kernel objects, no log, no registry access.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl.h>
#include <winternl.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "builtin_image.h"
#include "shm_protocol.h"

using namespace Microsoft::WRL;
namespace vc = beamer::vcam;

namespace {

HMODULE g_module;
std::atomic<long> g_objects{0};

struct ObjCount {
  ObjCount() { g_objects++; }
  ~ObjCount() { g_objects--; }
};

int64_t qpc_freq() {
  static const int64_t f = [] {
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return li.QuadPart;
  }();
  return f;
}
int64_t qpc_now() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return li.QuadPart;
}
int64_t qpc_ms(int64_t ms) { return ms * qpc_freq() / 1000; }

// ------------------------------------------------------------------------------------------ where we run
// Only the Frame Server instance (LocalService) creates objects and logs; any other process gets an inert
// source (MFCreateVirtualCamera loads the DLL once in the helper to validate it, §4.6.3).
bool token_is(WELL_KNOWN_SID_TYPE type) {
  HANDLE tok;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  BYTE buf[256];
  DWORD n = 0;
  bool r = GetTokenInformation(tok, TokenUser, buf, sizeof buf, &n) && IsWellKnownSid(((TOKEN_USER*)buf)->User.Sid, type);
  CloseHandle(tok);
  return r;
}
bool in_frame_server() {
  static const bool v = token_is(WinLocalServiceSid);
  return v;
}

// ------------------------------------------------------------------------------------------------- log
// %ProgramData%\Beamer\Camera\source.log, Frame Server only. Rate: a token bucket of 3 lines refilled at 1
// line/s (so on average at most 1 line/s; lines over the budget are counted and the count is appended to the
// next line written). Rotated to source.log.1 at 1 MB. The folder is created by vcam-install.
//
// Never through a link (R3 #1): this runs as LocalService, and a user who could plant a junction or link in that
// path would get LocalService to create, append to or overwrite a file of their choice. So: Beamer and Camera are
// opened one at a time relative to the ProgramData handle without following reparse points, each must be a real
// folder owned by Administrators or SYSTEM (vcam-install made them so; a non-administrator's folder never is), and
// the log is created and rotated through the Camera folder's handle (rename in place), never by path. The file must
// be a plain file with one link. Anything else: no log line (nothing else changes).
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef FILE_OPEN_IF
#define FILE_OPEN_IF 0x00000003
#endif
#ifndef FILE_DIRECTORY_FILE
#define FILE_DIRECTORY_FILE 0x00000001
#endif
#ifndef FILE_NON_DIRECTORY_FILE
#define FILE_NON_DIRECTORY_FILE 0x00000040
#endif
#ifndef FILE_SYNCHRONOUS_IO_NONALERT
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#endif
#ifndef FILE_OPEN_REPARSE_POINT
#define FILE_OPEN_REPARSE_POINT 0x00200000
#endif
using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG,
                                        ULONG, PVOID, ULONG);

// One path component inside the open folder `dir`; a reparse point there is opened as itself, never followed.
HANDLE open_rel_nofollow(HANDLE dir, const wchar_t* name, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options) {
  static const auto nt_create = reinterpret_cast<NtCreateFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  if (!nt_create) return nullptr;
  UNICODE_STRING us;
  us.Buffer = const_cast<PWSTR>(name);
  us.Length = us.MaximumLength = (USHORT)(wcslen(name) * sizeof(wchar_t));
  OBJECT_ATTRIBUTES oa;
  InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, dir, nullptr);
  IO_STATUS_BLOCK io{};
  HANDLE h = nullptr;
  NTSTATUS st = nt_create(&h, access | SYNCHRONIZE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                          options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  return st < 0 ? nullptr : h;
}

// A real folder (not a link) owned by Administrators or SYSTEM.
bool trusted_folder(HANDLE h) {
  BY_HANDLE_FILE_INFORMATION fi{};
  if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      !(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    return false;
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) return false;
  bool ok = owner && (IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid));
  LocalFree(sd);
  return ok;
}

// A plain file with a single name (no link, no hard link to a file elsewhere).
bool plain_file(HANDLE h, ULONGLONG* size) {
  BY_HANDLE_FILE_INFORMATION fi{};
  if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      fi.nNumberOfLinks != 1)
    return false;
  *size = ((ULONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
  return true;
}

// Rename the open file inside its own folder: NtSetInformationFile(FileRenameInformation) with a bare name and no
// root renames in the handle's folder; no path is resolved (kernel32's SetFileInformationByHandle would resolve a
// bare name under the process's current directory and move the file there).
bool rename_in_place(HANDLE h, const wchar_t* name) {
  using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
  static const auto nt_set = reinterpret_cast<NtSetInformationFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
  if (!nt_set) return false;
  const size_t n = wcslen(name);
  std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + n * sizeof(wchar_t));
  auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
  ri->ReplaceIfExists = TRUE;
  ri->RootDirectory = nullptr;
  ri->FileNameLength = (DWORD)(n * sizeof(wchar_t));
  memcpy(ri->FileName, name, n * sizeof(wchar_t));
  IO_STATUS_BLOCK io{};
  return nt_set(h, &io, ri, (ULONG)buf.size(), 10 /* FileRenameInformation */) >= 0;
}

struct OwnedHandle {
  HANDLE h = nullptr;
  explicit OwnedHandle(HANDLE x) : h(x == INVALID_HANDLE_VALUE ? nullptr : x) {}
  ~OwnedHandle() {
    if (h) CloseHandle(h);
  }
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
};

// Appends `n` bytes to the log as above; false when the path is not safe (nothing written).
bool append_log_nofollow(const std::wstring& program_data, const char* data, DWORD n) {
  OwnedHandle pd(CreateFileW(program_data.c_str(), FILE_TRAVERSE | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!pd.h) return false;
  // no delete share: neither folder can be renamed or replaced while the line is written
  OwnedHandle beamer(open_rel_nofollow(pd.h, L"Beamer", FILE_TRAVERSE | FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       FILE_OPEN, FILE_DIRECTORY_FILE));
  if (!beamer.h || !trusted_folder(beamer.h)) return false;
  OwnedHandle camera(open_rel_nofollow(beamer.h, L"Camera", FILE_TRAVERSE | FILE_ADD_FILE | FILE_READ_ATTRIBUTES | READ_CONTROL,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_DIRECTORY_FILE));
  if (!camera.h || !trusted_folder(camera.h)) return false;
  const DWORD access = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | DELETE;
  const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  ULONGLONG size = 0;
  HANDLE f = open_rel_nofollow(camera.h, L"source.log", access, share, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE);
  if (!f) return false;
  if (!plain_file(f, &size)) {
    CloseHandle(f);
    return false;
  }
  if (size > 1024 * 1024) {  // rotate: source.log -> source.log.1 in the same folder, then a fresh source.log
    const bool renamed = rename_in_place(f, L"source.log.1");
    CloseHandle(f);
    if (!renamed) return false;
    f = open_rel_nofollow(camera.h, L"source.log", access, share, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE);
    if (!f) return false;
    if (!plain_file(f, &size)) {
      CloseHandle(f);
      return false;
    }
  }
  DWORD w = 0;
  const bool ok = WriteFile(f, data, n, &w, nullptr) && w == n;
  CloseHandle(f);
  return ok;
}

class Log {
 public:
  void line(const char* fmt, ...) {
    if (!in_frame_server()) return;
    std::lock_guard<std::mutex> g(mu_);
    ULONGLONG now = GetTickCount64();
    tokens_ += (double)(now - last_ms_) / 1000.0;
    if (tokens_ > 3) tokens_ = 3;
    last_ms_ = now;
    if (tokens_ < 1) {
      suppressed_++;
      return;
    }
    tokens_ -= 1;
    char buf[640];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu v%d ", st.wYear, st.wMonth, st.wDay, st.wHour,
                     st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), vc::kDllVersion);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 48, fmt, ap);
    va_end(ap);
    n += m < 0 ? 0 : (m > (int)(sizeof buf - n - 48) ? (int)(sizeof buf - n - 48) : m);
    if (suppressed_) {
      int k = snprintf(buf + n, sizeof buf - n - 2, " (+%llu lines suppressed)", suppressed_);
      if (k > 0) n += k;
      if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    }
    suppressed_ = 0;
    buf[n++] = '\r';
    buf[n++] = '\n';
    if (path_.empty()) {
      wchar_t pd[MAX_PATH];
      DWORD k = ExpandEnvironmentStringsW(L"%ProgramData%", pd, MAX_PATH);
      path_ = (k > 0 && k < MAX_PATH && pd[0] != L'%') ? pd : L"C:\\ProgramData";
    }
    if (!append_log_nofollow(path_, buf, (DWORD)n)) unsafe_++;  // counted; nothing written
  }

 private:
  std::mutex mu_;
  std::wstring path_;  // %ProgramData%
  unsigned long long unsafe_ = 0;
  double tokens_ = 3;
  ULONGLONG last_ms_ = GetTickCount64();
  unsigned long long suppressed_ = 0;
};
Log g_log;

std::string narrow(const std::wstring& w) {
  char b[512];
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof b, nullptr, nullptr);
  return n > 0 ? std::string(b) : std::string("?");
}

// ------------------------------------------------------------------------------------- install identity
// HKLM\SOFTWARE\Beamer\Camera (written by the elevated vcam-install): InstallGuid names the objects, OwnerSid
// is the only user in their DACL. Both are validated: a GUID (36 hex/dash characters, optionally in braces)
// and a valid SID that is not a broad well-known group.
struct Install {
  bool ok = false;
  std::wstring guid, owner;
  const char* why = "";
};

Install read_install() {
  Install in;
  wchar_t buf[200];
  DWORD sz = sizeof buf;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, vc::kRegKey, L"InstallGuid", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) {
    in.why = "no InstallGuid";
    return in;
  }
  std::wstring g = buf;
  std::wstring core = g.size() == 38 && g.front() == L'{' && g.back() == L'}' ? g.substr(1, 36) : g;
  bool guid_ok = core.size() == 36;
  for (size_t i = 0; guid_ok && i < core.size(); i++) {
    wchar_t c = core[i];
    bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    guid_ok = dash ? c == L'-' : iswxdigit(c) != 0;
  }
  if (!guid_ok) {
    in.why = "bad InstallGuid";
    return in;
  }
  sz = sizeof buf;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, vc::kRegKey, L"OwnerSid", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) {
    in.why = "no OwnerSid";
    return in;
  }
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(buf, &sid) || !IsValidSid(sid)) {
    if (sid) LocalFree(sid);
    in.why = "bad OwnerSid";
    return in;
  }
  static const WELL_KNOWN_SID_TYPE broad[] = {WinWorldSid, WinAuthenticatedUserSid, WinBuiltinUsersSid, WinInteractiveSid, WinAnonymousSid,
                                              WinNetworkSid, WinBuiltinGuestsSid, WinLocalSid, WinBatchSid, WinServiceSid};
  bool is_broad = false;
  for (auto t : broad) is_broad = is_broad || IsWellKnownSid(sid, t);
  LPWSTR canon = nullptr;
  if (!is_broad && ConvertSidToStringSidW(sid, &canon)) {
    in.owner = canon;
    LocalFree(canon);
  }
  LocalFree(sid);
  if (in.owner.empty()) {
    in.why = "OwnerSid not allowed";
    return in;
  }
  in.guid = g;
  in.ok = true;
  return in;
}

// Owner of a kernel object we got ERROR_ALREADY_EXISTS for: only LocalService or SYSTEM may have made it.
bool owner_is_service(HANDLE h) {
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) return false;
  bool ok = owner && (IsWellKnownSid(owner, WinLocalServiceSid) || IsWellKnownSid(owner, WinLocalSystemSid));
  LocalFree(sd);
  return ok;
}

// --------------------------------------------------------------------------------------- shared memory
// One per process (Frame Server may hold several source objects at once and re-creates them on every
// close): created by the first source object, kept while any lives (§4.6.2 rules).
class Shared {
 public:
  void acquire() {
    std::lock_guard<std::mutex> g(mu_);
    if (refs_++ > 0) return;
    open_locked();
  }
  void release() {
    std::lock_guard<std::mutex> g(mu_);
    if (refs_ <= 0 || --refs_ > 0) return;
    if (hdr_) {
      running_ = 0;
      publish_locked();
      UnmapViewOfFile(hdr_);
    }
    if (map_) CloseHandle(map_);
    if (frame_) CloseHandle(frame_);
    hdr_ = nullptr;
    map_ = frame_ = nullptr;
    g_log.line("ipc: closed");
  }
  vc::Header* hdr() const { return hdr_; }   // stable while the caller holds a reference
  HANDLE frame_event() const { return frame_; }
  void stream_started(uint32_t w, uint32_t h, uint32_t fps) {
    std::lock_guard<std::mutex> g(mu_);
    running_++;
    if (!hdr_) return;
    hdr_->want_w = w;
    hdr_->want_h = h;
    hdr_->want_fps = fps;
    publish_locked();
  }
  // Every Stop/Shutdown publishes (state_seq + 1), so repeated stops are visible and harmless.
  void stream_stopped(bool was_running) {
    std::lock_guard<std::mutex> g(mu_);
    if (was_running && running_ > 0) running_--;
    if (hdr_) publish_locked();
  }

 private:
  void publish_locked() {
    hdr_->streaming = running_ > 0 ? 1 : 0;
    hdr_->state_qpc = (uint64_t)qpc_now();
    std::atomic_thread_fence(std::memory_order_release);
    hdr_->state_seq.fetch_add(1, std::memory_order_acq_rel);
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, demand_name_.c_str());
    if (ev) {
      SetEvent(ev);
      CloseHandle(ev);
    }
  }
  void open_locked() {
    if (!in_frame_server()) return;
    Install in = read_install();
    if (!in.ok) {
      g_log.line("ipc: not installed (%s): built-in image only", in.why);
      return;
    }
    std::wstring base = L"Global\\BeamerCamera." + in.guid;
    demand_name_ = base + L".Demand";
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;" + in.owner + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
      g_log.line("ipc: security descriptor failed %lu", GetLastError());
      return;
    }
    SECURITY_ATTRIBUTES sa = {sizeof sa, sd, FALSE};
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, (DWORD)(vc::kSectionBytes >> 32), (DWORD)vc::kSectionBytes,
                                    (base + L".Shm").c_str());
    DWORD map_err = GetLastError();
    HANDLE frame = map ? CreateEventW(&sa, FALSE, FALSE, (base + L".Frame").c_str()) : nullptr;
    DWORD frame_err = GetLastError();
    LocalFree(sd);
    const char* refused = nullptr;
    if (!map) refused = "section create failed";
    else if (map_err == ERROR_ALREADY_EXISTS && !owner_is_service(map)) refused = "squatted section";
    else if (!frame) refused = "frame event create failed";
    else if (frame_err == ERROR_ALREADY_EXISTS && !owner_is_service(frame)) refused = "squatted frame event";
    vc::Header* hdr = nullptr;
    if (!refused) {
      hdr = (vc::Header*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, (SIZE_T)vc::kSectionBytes);
      if (!hdr) refused = "map failed";
    }
    if (refused) {
      g_log.line("ipc: %s (err %lu/%lu): refusing the objects, built-in image only", refused, map ? frame_err : map_err, frame_err);
      if (map) CloseHandle(map);
      if (frame) CloseHandle(frame);
      return;
    }
    map_ = map;
    frame_ = frame;
    hdr_ = hdr;
    // Written at creation, never read back (§4.6.3 rule 1).
    hdr_->magic = vc::kMagic;
    hdr_->version = vc::kVersion;
    hdr_->header_bytes = (uint32_t)sizeof(vc::Header);
    hdr_->slot_count = vc::kSlotCount;
    hdr_->dll_version = (uint32_t)vc::kDllVersion;
    hdr_->source_pid = GetCurrentProcessId();
    running_ = 0;
    publish_locked();
    g_log.line("ipc: section %s, frame event %s (%ls)", map_err == ERROR_ALREADY_EXISTS ? "reopened" : "created",
               frame_err == ERROR_ALREADY_EXISTS ? "reopened" : "created", base.c_str());
  }

  std::mutex mu_;
  int refs_ = 0, running_ = 0;
  HANDLE map_ = nullptr, frame_ = nullptr;
  vc::Header* hdr_ = nullptr;
  std::wstring demand_name_;
};
Shared g_shared;

// ------------------------------------------------------------------------------------- built-in cache
// The built-in image per output size, rendered once per process on first use (WIC, ~10-30 ms).
const std::vector<uint8_t>& builtin_for(uint32_t w, uint32_t h) {
  static std::mutex mu;
  static std::map<uint64_t, std::vector<uint8_t>> cache;
  std::lock_guard<std::mutex> g(mu);
  auto& v = cache[((uint64_t)w << 32) | h];
  if (v.empty() && !vc::builtin::render_nv12(w, h, v)) g_log.line("builtin: WIC render failed, plain ground");
  return v;
}

// --------------------------------------------------------------------------------------- media types
HRESULT make_type(uint32_t w, uint32_t h, uint32_t fps, IMFMediaType** out) {
  ComPtr<IMFMediaType> t;
  HRESULT hr = MFCreateMediaType(&t);
  if (FAILED(hr)) return hr;
  t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
  MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, fps, 1);
  MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
  t->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
  t->SetUINT32(MF_MT_SAMPLE_SIZE, w * h * 3 / 2);
  t->SetUINT32(MF_MT_AVG_BITRATE, w * h * 3 / 2 * 8 * fps);
  *out = t.Detach();
  return S_OK;
}

uint32_t rd32(const uint32_t* p) { return *(const volatile uint32_t*)p; }   // one read of a hostile field

// Nearest-neighbour NV12 scale from the private buffer (sw x sh, stride ss) into the sample.
void scale_nv12(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t ss, uint8_t* dst, size_t dpitch, uint32_t dw, uint32_t dh) {
  for (uint32_t y = 0; y < dh; y++) {
    const uint8_t* s = src + (size_t)(y * (uint64_t)sh / dh) * ss;
    uint8_t* d = dst + (size_t)y * dpitch;
    for (uint32_t x = 0; x < dw; x++) d[x] = s[(size_t)x * sw / dw];
  }
  const uint8_t* suv = src + (size_t)ss * sh;
  uint8_t* duv = dst + dpitch * dh;
  for (uint32_t y = 0; y < dh / 2; y++) {
    const uint8_t* s = suv + (size_t)(y * (uint64_t)(sh / 2) / (dh / 2)) * ss;
    uint8_t* d = duv + (size_t)y * dpitch;
    for (uint32_t x = 0; x < dw / 2; x++) {
      size_t sx = (size_t)x * (sw / 2) / (dw / 2) * 2;
      d[2 * x] = s[sx];
      d[2 * x + 1] = s[sx + 1];
    }
  }
}

void copy_nv12(const uint8_t* src, uint32_t w, uint32_t h, uint32_t ss, uint8_t* dst, size_t dpitch) {
  for (uint32_t y = 0; y < h; y++) memcpy(dst + (size_t)y * dpitch, src + (size_t)y * ss, w);
  for (uint32_t y = 0; y < h / 2; y++) memcpy(dst + dpitch * h + (size_t)y * dpitch, src + (size_t)ss * h + (size_t)y * ss, w);
}

class Source;

// R3 #12: Stream::parent_ is a raw pointer (the source owns the stream; a reference back would be a cycle). The
// source's last Release and its destructor run under this lock, and the destructor clears parent_ under it, so
// GetMediaSource (same lock) either AddRefs a live source before its count can reach 0, or sees nullptr. Taken
// before any Stream::mu_.
std::recursive_mutex g_parent_mu;

// ------------------------------------------------------------------------------------------------ stream
class Stream : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
                                   IKsControl, FtmBase> {
  ObjCount count_;

 public:
  HRESULT Initialize(Source* parent, bool live);
  void StartStreaming(IMFMediaType* type);
  void StopStreaming();
  void Shutdown();
  void DetachParent() {  // g_parent_mu held (~Source)
    std::lock_guard<std::recursive_mutex> g(mu_);
    parent_ = nullptr;
  }
  ComPtr<IMFAttributes> attrs;

  // IMFMediaEventGenerator
  IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override {
    auto q = queue();
    return q ? q->BeginGetEvent(cb, st) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
    auto q = queue();
    return q ? q->EndGetEvent(r, e) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override {
    auto q = queue();
    return q ? q->GetEvent(f, e) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override {
    auto q = queue();
    return q ? q->QueueEventParamVar(t, g, s, v) : MF_E_SHUTDOWN;
  }
  // IMFMediaStream
  IFACEMETHODIMP GetMediaSource(IMFMediaSource** out) override;
  IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** out) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!out) return E_POINTER;
    if (!sd_) return MF_E_SHUTDOWN;
    return sd_.CopyTo(out);
  }
  IFACEMETHODIMP RequestSample(IUnknown* token) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    if (pending_.size() >= 64) return MF_E_SAMPLEALLOCATOR_EMPTY;
    pending_.push_back(ComPtr<IUnknown>(token));
    SetEvent(request_ev_);
    return S_OK;
  }
  // IMFMediaStream2
  IFACEMETHODIMP SetStreamState(MF_STREAM_STATE v) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_) return MF_E_SHUTDOWN;
    bool was = state_ == MF_STREAM_STATE_RUNNING;
    if (v == MF_STREAM_STATE_RUNNING && !was) {
      state_ = v;
      g_shared.stream_started(w_, h_, fps_);
      g_log.line("stream: running %ux%u@%u (SetStreamState)", w_, h_, fps_);
    } else if (v != MF_STREAM_STATE_RUNNING) {
      state_ = v;
      pending_.clear();
      g_shared.stream_stopped(was);
      if (was) g_log.line("stream: state %d", (int)v);
    }
    return S_OK;
  }
  IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* v) override {
    if (!v) return E_POINTER;
    std::lock_guard<std::recursive_mutex> g(mu_);
    *v = state_;
    return S_OK;
  }
  // IKsControl: no properties
  IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
  IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
  IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

  ~Stream() {
    Shutdown();
    if (stop_ev_) CloseHandle(stop_ev_);
    if (request_ev_) CloseHandle(request_ev_);
    if (timer_) CloseHandle(timer_);
  }

 private:
  enum class Kind { kNone, kBuiltin, kLive, kWaiting };
  enum class Copy { kOk, kTorn, kInvalid };
  struct Pick {
    Kind kind;
    bool fresh;   // a picture not delivered before
  };

  ComPtr<IMFMediaEventQueue> queue() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return q_;
  }
  void run();
  void tick();
  Pick pick(int64_t now);
  Copy copy_slot(vc::Header* hd, uint32_t idx, uint32_t* seq_out);
  void reject(const char* what, uint64_t a, uint64_t b);
  bool deliver(Kind kind);
  // R3 #12: the delivery thread holds a COM reference to its stream (taken in Initialize) and a reference to this
  // DLL, so neither can go away under it even if Shutdown's bounded wait gives up; it drops both on its way out.
  static DWORD WINAPI thread_main(void* p) {
    auto* s = (Stream*)p;
    s->run();
    HMODULE self = s->module_;
    s->Release();
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
  }
  HMODULE module_ = nullptr;

  std::recursive_mutex mu_;
  Source* parent_ = nullptr;   // raw: the source owns the stream; Shutdown breaks the link
  bool live_ = false;          // false = inert instance (not in Frame Server)
  ComPtr<IMFMediaEventQueue> q_;
  ComPtr<IMFStreamDescriptor> sd_;
  ComPtr<IMFVideoSampleAllocatorEx> alloc_;
  MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
  std::deque<ComPtr<IUnknown>> pending_;
  HANDLE stop_ev_ = nullptr, request_ev_ = nullptr, timer_ = nullptr, thread_ = nullptr;
  uint32_t w_ = vc::kOffered[0].w, h_ = vc::kOffered[0].h, fps_ = vc::kOffered[0].fps;

  // Private copies (§4.6.3 rule 3-4): staging_ receives a slot, good_ is the last picture that passed.
  std::vector<uint8_t> staging_, good_;
  uint32_t good_w_ = 0, good_h_ = 0, good_stride_ = 0;
  bool have_good_ = false;
  // Change detection for the slot last copied (helper-written values, used only to compare).
  uint32_t last_idx_ = UINT32_MAX, last_seq_ = 0;
  uint64_t last_written_ = 0;
  bool last_invalid_ = false;
  Kind shown_ = Kind::kNone;
  int64_t next_due_ = 0, last_wake_ = 0;
  uint64_t n_delivered_ = 0, n_placeholder_ = 0, n_repeat_ = 0, n_rejected_ = 0, n_empty_ = 0;
};

// ------------------------------------------------------------------------------------------------ source
class Source : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                                   IMFGetService, IKsControl, FtmBase> {
  ObjCount count_;

 public:
  HRESULT Initialize(IMFAttributes* activate_attrs) {
    live_ = in_frame_server();
    if (live_) {
      g_shared.acquire();
      holds_shared_ = true;
    }
    HRESULT hr = MFCreateAttributes(&attrs_, 10);
    if (SUCCEEDED(hr) && activate_attrs) activate_attrs->CopyAllItems(attrs_.Get());
    if (SUCCEEDED(hr)) hr = MFCreateEventQueue(&q_);
    if (SUCCEEDED(hr)) {
      stream_ = Make<Stream>();
      hr = stream_ ? stream_->Initialize(this, live_) : E_OUTOFMEMORY;
    }
    ComPtr<IMFStreamDescriptor> sd;
    if (SUCCEEDED(hr)) hr = stream_->GetStreamDescriptor(&sd);
    IMFStreamDescriptor* sds[] = {sd.Get()};
    if (SUCCEEDED(hr)) hr = MFCreatePresentationDescriptor(1, sds, &pd_);
    if (SUCCEEDED(hr)) hr = pd_->SelectStream(0);
    if (FAILED(hr)) g_log.line("source: Initialize failed 0x%08lx", (unsigned long)hr);
    return hr;
  }
  // IMFMediaEventGenerator
  IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override {
    auto q = queue();
    return q ? q->BeginGetEvent(cb, st) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
    auto q = queue();
    return q ? q->EndGetEvent(r, e) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override {
    auto q = queue();
    return q ? q->GetEvent(f, e) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override {
    auto q = queue();
    return q ? q->QueueEventParamVar(t, g, s, v) : MF_E_SHUTDOWN;
  }
  // IMFMediaSource
  IFACEMETHODIMP GetCharacteristics(DWORD* c) override {
    if (!c) return E_POINTER;
    if (!queue()) return MF_E_SHUTDOWN;
    *c = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
  }
  IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!out) return E_POINTER;
    if (!q_) return MF_E_SHUTDOWN;
    return pd_->Clone(out);
  }
  IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* fmt, const PROPVARIANT* pos) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!pd || !pos) return E_INVALIDARG;
    if (!q_) return MF_E_SHUTDOWN;
    if (fmt && *fmt != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    DWORD n = 0;
    HRESULT hr = pd->GetStreamDescriptorCount(&n);
    if (FAILED(hr)) return hr;
    for (DWORD i = 0; i < n; i++) {
      BOOL selected = FALSE;
      ComPtr<IMFStreamDescriptor> sd;
      ComPtr<IMFMediaTypeHandler> h;
      ComPtr<IMFMediaType> type;
      if (FAILED(hr = pd->GetStreamDescriptorByIndex(i, &selected, &sd))) return hr;
      if (!selected) continue;
      if (FAILED(hr = sd->GetMediaTypeHandler(&h)) || FAILED(hr = h->GetCurrentMediaType(&type))) return hr;
      if (FAILED(hr = q_->QueueEventParamUnk(started_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, (IMFMediaStream*)stream_.Get())))
        return hr;
      stream_->StartStreaming(type.Get());
    }
    started_ = true;
    return q_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pos);
  }
  IFACEMETHODIMP Stop() override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_) return MF_E_SHUTDOWN;
    stream_->StopStreaming();
    return q_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
  }
  IFACEMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
  IFACEMETHODIMP Shutdown() override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_) {
      if (stream_) stream_->Shutdown();   // repeated Shutdown: publishes once more, harmless
      return MF_E_SHUTDOWN;
    }
    if (stream_) stream_->Shutdown();
    q_->Shutdown();
    q_.Reset();
    drop_shared();
    return S_OK;
  }
  // IMFMediaSourceEx
  IFACEMETHODIMP GetSourceAttributes(IMFAttributes** out) override {
    if (!out) return E_POINTER;
    return attrs_.CopyTo(out);
  }
  IFACEMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** out) override {
    if (!out) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    return stream_->attrs.CopyTo(out);
  }
  IFACEMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }   // system-memory samples
  // IMFGetService
  IFACEMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }
  // IKsControl
  IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
  IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
  IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

  ~Source() {
    {
      std::lock_guard<std::recursive_mutex> pg(g_parent_mu);  // held already when reached through Release()
      if (stream_) stream_->DetachParent();
    }
    if (stream_) stream_->Shutdown();
    drop_shared();
  }
  // The last Release (and so the destructor) runs under g_parent_mu: see Stream::GetMediaSource (R3 #12).
  STDMETHOD_(ULONG, Release)() override {
    std::lock_guard<std::recursive_mutex> pg(g_parent_mu);
    return RuntimeClass::Release();
  }

 private:
  ComPtr<IMFMediaEventQueue> queue() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return q_;
  }
  void drop_shared() {
    if (holds_shared_) {
      holds_shared_ = false;
      g_shared.release();
    }
  }
  std::recursive_mutex mu_;
  ComPtr<IMFAttributes> attrs_;
  ComPtr<IMFMediaEventQueue> q_;
  ComPtr<IMFPresentationDescriptor> pd_;
  ComPtr<Stream> stream_;
  bool started_ = false, live_ = false, holds_shared_ = false;
};

HRESULT Stream::Initialize(Source* parent, bool live) {
  parent_ = parent;
  live_ = live;
  HRESULT hr = MFCreateAttributes(&attrs, 10);
  if (FAILED(hr)) return hr;
  attrs->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
  attrs->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
  attrs->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
  attrs->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
  // Exactly kOffered, in its order; the first is the default (shm_protocol.h).
  constexpr size_t kN = sizeof(vc::kOffered) / sizeof(vc::kOffered[0]);
  ComPtr<IMFMediaType> types[kN];
  IMFMediaType* raw[kN];
  for (size_t i = 0; i < kN; i++) {
    if (FAILED(hr = make_type(vc::kOffered[i].w, vc::kOffered[i].h, vc::kOffered[i].fps, &types[i]))) return hr;
    raw[i] = types[i].Get();
  }
  if (FAILED(hr = MFCreateStreamDescriptor(0, (DWORD)kN, raw, &sd_))) return hr;
  ComPtr<IMFMediaTypeHandler> h;
  if (FAILED(hr = sd_->GetMediaTypeHandler(&h)) || FAILED(hr = h->SetCurrentMediaType(raw[0]))) return hr;
  if (FAILED(hr = attrs->CopyAllItems(sd_.Get()))) return hr;
  if (FAILED(hr = MFCreateEventQueue(&q_))) return hr;
  stop_ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  request_ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  // The delivery clock: a high-resolution timer (Frame Server runs at the default 15.6 ms timer resolution,
  // which would stretch plain wait timeouts to ~21 fps).
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  if (!stop_ev_ || !request_ev_ || !timer_) return HRESULT_FROM_WIN32(GetLastError());
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&thread_main), &module_);
  AddRef();  // the thread's reference (released by thread_main)
  thread_ = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
  if (!thread_) {
    const DWORD e = GetLastError();
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
    Release();
    return HRESULT_FROM_WIN32(e);
  }
  return S_OK;
}

IFACEMETHODIMP Stream::GetMediaSource(IMFMediaSource** out) {
  std::lock_guard<std::recursive_mutex> pg(g_parent_mu);  // the source cannot reach 0 references meanwhile
  std::lock_guard<std::recursive_mutex> g(mu_);
  if (!out) return E_POINTER;
  if (!parent_ || !q_) return MF_E_SHUTDOWN;
  return parent_->QueryInterface(IID_PPV_ARGS(out));
}

void Stream::StartStreaming(IMFMediaType* type) {
  std::lock_guard<std::recursive_mutex> g(mu_);
  if (!q_) return;
  UINT32 w = 0, h = 0, fn = 0, fd = 0;
  MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
  MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &fn, &fd);
  // Only offered types can be current (MF checks them against the descriptor); keep a sane default anyway.
  bool offered = false;
  for (const auto& t : vc::kOffered) offered = offered || (t.w == w && t.h == h);
  if (!offered) {
    w = vc::kOffered[0].w;
    h = vc::kOffered[0].h;
  }
  w_ = w;
  h_ = h;
  fps_ = fd && fn / fd >= 1 && fn / fd <= 60 ? fn / fd : 30;
  alloc_.Reset();
  HRESULT hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&alloc_));
  if (SUCCEEDED(hr)) hr = alloc_->InitializeSampleAllocator(10, type);
  if (FAILED(hr)) g_log.line("stream: sample allocator failed 0x%08lx", (unsigned long)hr);
  if (staging_.size() != vc::kSlotBytes) {
    staging_.assign(vc::kSlotBytes, 0);
    good_.assign(vc::kSlotBytes, 0);
  }
  pending_.clear();
  next_due_ = qpc_now();
  shown_ = Kind::kNone;
  bool was = state_ == MF_STREAM_STATE_RUNNING;
  state_ = MF_STREAM_STATE_RUNNING;
  n_delivered_ = n_placeholder_ = n_repeat_ = n_rejected_ = n_empty_ = 0;
  q_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
  if (was) g_shared.stream_stopped(true);   // a restart with a new type: one stop, one start
  g_shared.stream_started(w_, h_, fps_);
  g_log.line("stream: start %ux%u@%u", w_, h_, fps_);
}

void Stream::StopStreaming() {
  std::lock_guard<std::recursive_mutex> g(mu_);
  bool was = state_ == MF_STREAM_STATE_RUNNING;
  state_ = MF_STREAM_STATE_STOPPED;
  pending_.clear();
  g_shared.stream_stopped(was);
  if (was)
    g_log.line("stream: stop (delivered %llu, placeholders %llu, repeats %llu, rejected %llu, allocator empty %llu)", n_delivered_,
               n_placeholder_, n_repeat_, n_rejected_, n_empty_);
  if (q_) q_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void Stream::Shutdown() {
  HANDLE th = nullptr;
  {
    std::lock_guard<std::recursive_mutex> g(mu_);
    bool was = state_ == MF_STREAM_STATE_RUNNING;
    g_shared.stream_stopped(was);   // every Shutdown publishes (state_seq + 1) while the section is open
    if (was)
      g_log.line("stream: shutdown while running (delivered %llu, placeholders %llu, repeats %llu, rejected %llu)", n_delivered_,
                 n_placeholder_, n_repeat_, n_rejected_);
    state_ = MF_STREAM_STATE_STOPPED;
    if (q_) {
      q_->Shutdown();
      q_.Reset();
    }
    pending_.clear();
    parent_ = nullptr;
    th = thread_;
    thread_ = nullptr;
    if (stop_ev_) SetEvent(stop_ev_);
  }
  if (th) {
    if (GetCurrentThreadId() != GetThreadId(th)) WaitForSingleObject(th, 5000);
    CloseHandle(th);
  }
  std::lock_guard<std::recursive_mutex> g(mu_);
  alloc_.Reset();
}

void Stream::reject(const char* what, uint64_t a, uint64_t b) {
  n_rejected_++;
  if (vc::Header* hd = g_shared.hdr()) hd->rejected.fetch_add(1, std::memory_order_relaxed);
  g_log.line("rejected %s (%llu, %llu); total %llu this stream", what, (unsigned long long)a, (unsigned long long)b, n_rejected_);
}

// §4.6.3 rule 3: one slot under the seqlock into staging_; two attempts.
Stream::Copy Stream::copy_slot(vc::Header* hd, uint32_t idx, uint32_t* seq_out) {
  vc::Slot& s = hd->slots[idx];
  const uint8_t* pixels = (const uint8_t*)hd + vc::kDataOffset + (uint64_t)idx * vc::kSlotBytes;
  for (int attempt = 0; attempt < 2; attempt++) {
    uint32_t s1 = s.seq.load(std::memory_order_acquire);
    if (s1 & 1) continue;
    uint32_t w = rd32(&s.w), h = rd32(&s.h), stride = rd32(&s.stride);
    bool ok = !(w & 1) && !(h & 1) && w >= vc::kMinW && w <= vc::kMaxW && h >= vc::kMinH && h <= vc::kMaxH && stride >= w && stride <= 4096 &&
              (uint64_t)stride * h * 3 / 2 <= vc::kSlotBytes;
    if (!ok) {
      std::atomic_thread_fence(std::memory_order_acquire);
      if (s.seq.load(std::memory_order_relaxed) != s1) continue;   // changed under us: torn, not invalid
      *seq_out = s1;
      reject("slot geometry", ((uint64_t)w << 32) | h, stride);
      return Copy::kInvalid;
    }
    memcpy(staging_.data(), pixels, (size_t)((uint64_t)stride * h * 3 / 2));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) != s1) continue;
    std::swap(staging_, good_);
    good_w_ = w;
    good_h_ = h;
    good_stride_ = stride;
    have_good_ = true;
    *seq_out = s1;
    return Copy::kOk;
  }
  reject("torn slot", idx, 0);
  return Copy::kTorn;
}

// What to show now (§4.6.4). Every helper field is read once (rule 2).
Stream::Pick Stream::pick(int64_t now) {
  vc::Header* hd = live_ ? g_shared.hdr() : nullptr;
  if (!hd) return {Kind::kBuiltin, false};
  uint64_t beat = hd->beat_qpc.load(std::memory_order_acquire);
  uint32_t mode = hd->mode.load(std::memory_order_acquire);
  uint32_t latest = hd->latest.load(std::memory_order_acquire);
  if (beat > (uint64_t)(now + qpc_ms(1000))) {
    reject("beat in the future", beat, (uint64_t)now);
    return {Kind::kBuiltin, false};
  }
  if (beat == 0 || (int64_t)beat < now - qpc_ms(500)) return {Kind::kBuiltin, false};   // nobody feeds the camera
  if (mode > vc::kModeWaiting) {
    reject("mode", mode, 0);
    return {Kind::kBuiltin, false};
  }
  if (mode == vc::kModeNone) return {Kind::kBuiltin, false};
  uint32_t idx;
  Kind kind;
  if (mode == vc::kModeWaiting) {
    idx = vc::kLiveSlots;   // slot 3, the helper's placeholder
    kind = Kind::kWaiting;
  } else {
    if (latest >= vc::kLiveSlots) {
      reject("latest", latest, 0);
      return {Kind::kBuiltin, false};
    }
    idx = latest;
    kind = Kind::kLive;
  }
  // Unchanged slot (same index, seq and write time): keep what that slot gave last time.
  vc::Slot& s = hd->slots[idx];
  uint32_t seq = s.seq.load(std::memory_order_acquire);
  uint64_t written = *(volatile uint64_t*)&s.written_qpc;
  if (idx == last_idx_ && seq == last_seq_ && written == last_written_ && !(seq & 1)) {
    if (last_invalid_) return {Kind::kBuiltin, false};
    return {kind, false};
  }
  uint32_t got = 0;
  switch (copy_slot(hd, idx, &got)) {
    case Copy::kOk:
      last_idx_ = idx;
      last_seq_ = got;
      last_written_ = written;
      last_invalid_ = false;
      return {kind, true};
    case Copy::kInvalid:
      last_idx_ = idx;
      last_seq_ = got;
      last_written_ = written;
      last_invalid_ = true;
      return {Kind::kBuiltin, false};
    case Copy::kTorn:
    default:
      // the previous good picture if one is on screen, else the built-in image
      if (have_good_ && (shown_ == Kind::kLive || shown_ == Kind::kWaiting)) return {shown_, false};
      return {Kind::kBuiltin, false};
  }
}

// Fills one sample and queues it. Called with mu_ held and a pending request.
bool Stream::deliver(Kind kind) {
  ComPtr<IMFSample> sample;
  HRESULT hr = alloc_ ? alloc_->AllocateSample(&sample) : E_FAIL;
  if (FAILED(hr)) {
    n_empty_++;
    return false;
  }
  ComPtr<IUnknown> token = pending_.front();
  pending_.pop_front();
  ComPtr<IMFMediaBuffer> buf;
  ComPtr<IMF2DBuffer2> b2;
  BYTE *scan0 = nullptr, *start = nullptr;
  LONG pitch = 0;
  DWORD len = 0;
  if (FAILED(sample->GetBufferByIndex(0, &buf)) || FAILED(buf.As(&b2)) ||
      FAILED(b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len))) {
    g_log.line("stream: sample lock failed");
    return false;
  }
  if (pitch < (LONG)w_ || (uint64_t)pitch * h_ * 3 / 2 > len) {
    b2->Unlock2D();
    g_log.line("stream: unexpected sample layout pitch %ld len %lu", pitch, len);
    return false;
  }
  bool good = kind != Kind::kBuiltin && have_good_;
  if (good) {
    if (good_w_ == w_ && good_h_ == h_)
      copy_nv12(good_.data(), w_, h_, good_stride_, scan0, (size_t)pitch);
    else
      scale_nv12(good_.data(), good_w_, good_h_, good_stride_, scan0, (size_t)pitch, w_, h_);
  } else {
    kind = Kind::kBuiltin;
    const auto& img = builtin_for(w_, h_);
    copy_nv12(img.data(), w_, h_, w_, scan0, (size_t)pitch);
  }
  if (kind != Kind::kLive) vc::builtin::draw_dot(scan0, (size_t)pitch, w_, h_, GetTickCount64());
  b2->Unlock2D();
  buf->SetCurrentLength((DWORD)(w_ * h_ * 3 / 2));
  sample->SetSampleTime(MFGetSystemTime());
  sample->SetSampleDuration(10000000 / fps_);
  if (token) sample->SetUnknown(MFSampleExtension_Token, token.Get());
  hr = q_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
  if (FAILED(hr)) {
    g_log.line("stream: queue sample failed 0x%08lx", (unsigned long)hr);
    return false;
  }
  n_delivered_++;
  if (vc::Header* hd = live_ ? g_shared.hdr() : nullptr) {
    hd->delivered.fetch_add(1, std::memory_order_relaxed);
    if (kind != Kind::kLive) hd->placeholders.fetch_add(1, std::memory_order_relaxed);
  }
  if (kind != Kind::kLive) n_placeholder_++;
  shown_ = kind;
  return true;
}

void Stream::tick() {
  if (!q_ || state_ != MF_STREAM_STATE_RUNNING || pending_.empty()) return;
  int64_t now = qpc_now();
  Pick p = pick(now);
  bool changed = p.fresh || p.kind != shown_;
  if (!changed && now < next_due_) return;
  bool repeat = !changed && p.kind == Kind::kLive;
  if (!deliver(p.kind)) return;
  if (repeat) {
    n_repeat_++;
    if (vc::Header* hd = live_ ? g_shared.hdr() : nullptr) hd->repeats.fetch_add(1, std::memory_order_relaxed);
  }
  int64_t period = qpc_freq() / fps_;
  // A fresh live picture: repeat it only if the next one is 1.5 frames late (helper jitter is not a repeat).
  next_due_ = now + (p.fresh && p.kind == Kind::kLive ? period * 3 / 2 : period);
}

void Stream::run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // delivery is the latency path
  for (;;) {
    HANDLE frame_ev = nullptr;
    {
      std::lock_guard<std::recursive_mutex> g(mu_);
      int64_t now = qpc_now();
      // wake at the next delivery time, at the latest 10 ms from now (beat staleness is checked on wake)
      int64_t wake = now + qpc_ms(10);
      bool waiting_request = state_ == MF_STREAM_STATE_RUNNING && !pending_.empty();   // else a request wakes us
      if (waiting_request && next_due_ < wake) wake = next_due_ > now ? next_due_ : now;
      // The frame event is helper-controlled: listen to it at most once per 2 ms, so a writer setting it in
      // a loop cannot spin this thread (requests and the delivery clock still wake it).
      HANDLE ev = live_ ? g_shared.frame_event() : nullptr;
      int64_t since = now - last_wake_;
      if (ev && since >= qpc_ms(2)) frame_ev = ev;
      else if (ev && last_wake_ + qpc_ms(2) < wake) wake = last_wake_ + qpc_ms(2);
      LARGE_INTEGER due;
      due.QuadPart = -(int64_t)((double)(wake - now) * 1e7 / (double)qpc_freq());
      if (due.QuadPart >= 0) due.QuadPart = -1;
      SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    }
    HANDLE hs[4] = {stop_ev_, request_ev_, timer_, frame_ev};
    DWORD r = WaitForMultipleObjects(frame_ev ? 4 : 3, hs, FALSE, 1000);
    if (r == WAIT_OBJECT_0) break;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (r == WAIT_OBJECT_0 + 3) last_wake_ = qpc_now();
    tick();
  }
  CoUninitialize();
}

// --------------------------------------------------------------------------------------------- activate
#define FWD(name, params, args) \
  IFACEMETHODIMP name params override { return a_->name args; }
class Activate : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>, FtmBase> {
  ObjCount count_;

 public:
  HRESULT Initialize() { return MFCreateAttributes(&a_, 4); }
  IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
    std::lock_guard<std::mutex> g(mu_);
    if (!src_) {
      auto s = Make<Source>();
      if (!s) return E_OUTOFMEMORY;
      HRESULT hr = s->Initialize(a_.Get());
      if (FAILED(hr)) {
        s->Shutdown();
        return hr;
      }
      src_ = s;
    }
    return src_.CopyTo(riid, ppv);
  }
  IFACEMETHODIMP ShutdownObject() override {
    std::lock_guard<std::mutex> g(mu_);
    if (src_) src_->Shutdown();
    src_.Reset();
    return S_OK;
  }
  IFACEMETHODIMP DetachObject() override {
    std::lock_guard<std::mutex> g(mu_);
    src_.Reset();
    return S_OK;
  }
  FWD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
  FWD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
  FWD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
  FWD(Compare, (IMFAttributes * o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r), (o, t, r))
  FWD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
  FWD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
  FWD(GetDouble, (REFGUID k, double* v), (k, v))
  FWD(GetGUID, (REFGUID k, GUID* v), (k, v))
  FWD(GetStringLength, (REFGUID k, UINT32* v), (k, v))
  FWD(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32* l), (k, v, n, l))
  FWD(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* l), (k, v, l))
  FWD(GetBlobSize, (REFGUID k, UINT32* v), (k, v))
  FWD(GetBlob, (REFGUID k, UINT8* v, UINT32 n, UINT32* l), (k, v, n, l))
  FWD(GetAllocatedBlob, (REFGUID k, UINT8** v, UINT32* l), (k, v, l))
  FWD(GetUnknown, (REFGUID k, REFIID r, LPVOID* v), (k, r, v))
  FWD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
  FWD(DeleteItem, (REFGUID k), (k))
  FWD(DeleteAllItems, (), ())
  FWD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
  FWD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
  FWD(SetDouble, (REFGUID k, double v), (k, v))
  FWD(SetGUID, (REFGUID k, REFGUID v), (k, v))
  FWD(SetString, (REFGUID k, LPCWSTR v), (k, v))
  FWD(SetBlob, (REFGUID k, const UINT8* v, UINT32 n), (k, v, n))
  FWD(SetUnknown, (REFGUID k, IUnknown* v), (k, v))
  FWD(LockStore, (), ())
  FWD(UnlockStore, (), ())
  FWD(GetCount, (UINT32 * n), (n))
  FWD(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
  FWD(CopyAllItems, (IMFAttributes * d), (d))

 private:
  std::mutex mu_;
  ComPtr<IMFAttributes> a_;
  ComPtr<Source> src_;
};

class Factory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory, FtmBase> {
 public:
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto a = Make<Activate>();
    if (!a) return E_OUTOFMEMORY;
    HRESULT hr = a->Initialize();
    if (FAILED(hr)) return hr;
    return a.CopyTo(riid, ppv);
  }
  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock) g_objects++;
    else g_objects--;
    return S_OK;
  }
};

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;
  CLSID ours;
  if (FAILED(CLSIDFromString(vc::kClsidText, &ours)) || clsid != ours) return CLASS_E_CLASSNOTAVAILABLE;
  if (in_frame_server()) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      wchar_t path[MAX_PATH] = {0};
      GetModuleFileNameW(g_module, path, MAX_PATH);
      g_log.line("loaded from %s", narrow(path).c_str());
    }
  }
  auto f = Make<Factory>();
  if (!f) return E_OUTOFMEMORY;
  return f.CopyTo(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }
