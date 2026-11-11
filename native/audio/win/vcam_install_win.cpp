// SPDX-License-Identifier: GPL-3.0-only
// Helper verbs for Beamer Camera's install (camera PHASE2 §3.1, §9.2, CW4), run without --control:
//   beamer-audio vcam-install --owner-sid <SID> --dll-sha256 <hex> [--dll <path>]   (elevated) copy the DLL to
//        %ProgramFiles%\Beamer\Camera\<kDllVersion>\ and check the copy's SHA-256 there (R3 #8: the source folder
//        may be user-writable; the copy is in a folder only administrators can write, so what is checked is what
//        Frame Server loads), make %ProgramData%\Beamer\Camera (SYSTEM, LocalService, Administrators write; Users
//        read), register the COM class in HKLM and write HKLM\SOFTWARE\Beamer\Camera (InstallGuid kept, OwnerSid,
//        DllVersion, DllPath), delete older version folders that are not locked
//   beamer-audio vcam-uninstall                                   (elevated) the reverse; stops FrameServer only
//        when a file is locked
// Folders (R3 #1). Every folder this verb creates or reuses (Program Files\Beamer, \Camera, \<v>; ProgramData\Beamer,
// \Camera) is opened one component at a time, relative to its parent's handle, without following a reparse point.
// It must be a real folder (not a junction or symbolic link) owned by Administrators or SYSTEM; it then gets an
// explicit protected ACL (no user can write). A folder that exists with another owner, or is a link, was made by a
// non-administrator (ProgramData lets any user create folders): the install refuses with `unsafe_folder` and names
// it, rather than take it over, because taking ownership cannot undo what is already inside or what its maker can
// still do with handles opened before. An administrator deletes it and installs again. Uninstall deletes through
// handles the same way and never follows a link: a junction or symbolic link is removed itself, never its target.
//   beamer-audio vcam-remove                                      (unelevated) Remove() this user's camera
//   beamer-audio vcam-status                                      prints the `vcam` event once
// Exit codes: 0 ok, 2 bad args, 3 not elevated, 4 failed. Each verb prints one JSON line.
// Without --dll the DLL is looked up beside the helper: <exe dir>\camera\BeamerCameraSource.dll, then
// <exe dir>\BeamerCameraSource.dll (§9.2; the dev build writes native\audio\build\win\camera\).
// Test seams (video_check only, never used by Beamer): --test-reg <HKLM subkey> puts every registry write under it
// (see WinVideoOptions::reg_base) and --test-dir <dir> puts the two folders under it; with either, the FrameServer
// service is never touched. --test-dir needs --test-reg (R3 #8): the real COM class never points into a test folder.
#include "video_backend_win.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winternl.h>

#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#ifndef FILE_CREATE
#define FILE_CREATE 0x00000002
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
#ifndef FILE_OPEN_FOR_BACKUP_INTENT
#define FILE_OPEN_FOR_BACKUP_INTENT 0x00004000
#endif
#ifndef FILE_CREATED
#define FILE_CREATED 0x00000002
#endif

namespace beamer::audio {

namespace win {
namespace {

struct VerbArgs {
    std::wstring owner_sid, dll, dll_sha256, test_reg, test_dir;
    bool bad = false;
};

// ------------------------------------------------------------------ no-follow file system access (R3 #1)
// An owned HANDLE (INVALID_HANDLE_VALUE / nullptr = none).
struct H {
    HANDLE h = nullptr;
    H() = default;
    explicit H(HANDLE x) : h(x == INVALID_HANDLE_VALUE ? nullptr : x) {}
    H(H&& o) noexcept : h(o.h) { o.h = nullptr; }
    H& operator=(H&& o) noexcept {
        if (this != &o) {
            reset();
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    ~H() { reset(); }
    void reset() {
        if (h) CloseHandle(h);
        h = nullptr;
    }
    explicit operator bool() const { return h != nullptr; }
};

using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG,
                                        ULONG, PVOID, ULONG);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NTSTATUS);

// Opens `name` (one path component) inside the folder `dir`, relative to its handle, and never follows a reparse
// point: a junction or symbolic link there is opened as the link itself (FILE_OPEN_REPARSE_POINT). `*created` tells
// whether this call made it. Win32 error in *err.
H open_rel(HANDLE dir, const std::wstring& name, ACCESS_MASK access, ULONG share, ULONG disposition, ULONG options,
           PSECURITY_DESCRIPTOR sd, DWORD* err, bool* created = nullptr) {
    static const auto nt_create = reinterpret_cast<NtCreateFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
    static const auto to_dos = reinterpret_cast<RtlNtStatusToDosErrorFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
    if (created) *created = false;
    if (!nt_create || !to_dos) {
        *err = ERROR_PROC_NOT_FOUND;
        return H();
    }
    if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"\\/:") != std::wstring::npos) {
        *err = ERROR_INVALID_NAME;
        return H();
    }
    UNICODE_STRING us;
    us.Buffer = const_cast<PWSTR>(name.c_str());
    us.Length = us.MaximumLength = USHORT(name.size() * sizeof(wchar_t));
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE, dir, sd);
    IO_STATUS_BLOCK io{};
    HANDLE h = nullptr;
    NTSTATUS st = nt_create(&h, access | SYNCHRONIZE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                            options | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_FOR_BACKUP_INTENT, nullptr, 0);
    if (st < 0) {
        *err = to_dos(st);
        return H();
    }
    *err = ERROR_SUCCESS;
    if (created) *created = io.Information == FILE_CREATED;
    return H(h);
}

// A folder opened by path the normal way (the trusted base: %ProgramFiles%, %ProgramData%, or a test folder).
H open_base(const std::wstring& path, DWORD* err) {
    HANDLE h = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_SUBDIRECTORY | READ_CONTROL | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    *err = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    return H(h);
}

DWORD attributes_of(HANDLE h, DWORD* links = nullptr) {
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi)) return INVALID_FILE_ATTRIBUTES;
    if (links) *links = fi.nNumberOfLinks;
    return fi.dwFileAttributes;
}

std::string sid_text(PSID sid) {
    wchar_t name[256], dom[256];
    DWORD n = 256, d = 256;
    SID_NAME_USE use;
    if (LookupAccountSidW(nullptr, sid, name, &n, dom, &d, &use)) return utf8(std::wstring(dom) + (d ? L"\\" : L"") + name);
    LPWSTR str = nullptr;
    std::string r = "?";
    if (ConvertSidToStringSidW(sid, &str)) {
        r = utf8(str);
        LocalFree(str);
    }
    return r;
}

// Owner of an open file or folder: Administrators or SYSTEM is trusted. *who = the owner's name.
bool owner_trusted(HANDLE h, std::string* who) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS || !owner) {
        if (sd) LocalFree(sd);
        *who = "unknown";
        return false;
    }
    const bool ok = IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid);
    *who = sid_text(owner);
    LocalFree(sd);
    return ok;
}

// Creates `name` inside `parent` (owner Administrators, protected ACL `sddl`) or validates the one that is there:
// a real folder owned by Administrators or SYSTEM, whose ACL is then set to `sddl`. Anything else: refused
// (`unsafe_folder`). The returned handle cannot be renamed or deleted by anyone while it is open (no delete share),
// so what is created inside it next is created in this very folder.
H secure_dir(HANDLE parent, const std::wstring& name, const wchar_t* sddl, const std::wstring& shown, std::string* err) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) {
        *err = "internal: sddl";
        return H();
    }
    DWORD e = 0;
    bool created = false;
    H d = open_rel(parent, name,
                   FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | FILE_READ_ATTRIBUTES |
                       READ_CONTROL | WRITE_DAC,
                   FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, FILE_DIRECTORY_FILE, sd, &e, &created);
    if (!d) {
        LocalFree(sd);
        if (e == ERROR_DIRECTORY)
            *err = "unsafe_folder: " + utf8(shown) + " exists and is not a folder; delete it (as administrator) and install again";
        else if (e == ERROR_ACCESS_DENIED)  // it exists (creating it would not be denied to an administrator) and shuts us out
            *err = "unsafe_folder: " + utf8(shown) + " exists and administrators cannot open it (not made by an installer); delete it "
                   "(as administrator) and install again";
        else
            *err = "create " + utf8(shown) + ": error " + std::to_string(e);
        return H();
    }
    std::string who;
    const DWORD attrs = attributes_of(d.h);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        LocalFree(sd);
        *err = "unsafe_folder: " + utf8(shown) + " is a link (junction or symbolic link), not a folder; delete it (as administrator) and install again";
        return H();
    }
    if (!owner_trusted(d.h, &who)) {
        LocalFree(sd);
        *err = "unsafe_folder: " + utf8(shown) + " already exists and is owned by " + who +
               ", not by Administrators or SYSTEM: a non-administrator made it, so it cannot be trusted; delete it (as administrator) and install again";
        return H();
    }
    if (!created) {  // an earlier install's folder (owner Administrators): enforce today's protected ACL
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
        DWORD r = SetSecurityInfo(d.h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        if (r != ERROR_SUCCESS) {
            LocalFree(sd);
            *err = "folder ACL " + utf8(shown) + ": error " + std::to_string(r);
            return H();
        }
    }
    LocalFree(sd);
    return d;
}

// Program Files\Beamer\Camera\<v>: administrators and SYSTEM write; users (and LocalService) read and run the DLL.
constexpr wchar_t kSddlProgram[] = L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;LS)(A;OICI;0x1200a9;;;AC)";
// ProgramData\Beamer: administrators and SYSTEM write; everyone else only reads and traverses.
constexpr wchar_t kSddlDataParent[] = L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;LS)";
// ProgramData\Beamer\Camera (§4.6.3): SYSTEM, LocalService (the DLL's log) and Administrators write; Users read.
constexpr wchar_t kSddlData[] = L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;LS)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";

bool set_delete(HANDLE h) {
    FILE_DISPOSITION_INFO di{};
    di.DeleteFile = TRUE;
    return SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di) != 0;
}

// Renames the open file to `name` in its own folder: NtSetInformationFile(FileRenameInformation) with a bare name and
// no root is a rename inside the folder the handle is in; no path is resolved. (Not SetFileInformationByHandle:
// kernel32 turns a bare name into a path under the process's current directory, which moves the file there.)
bool rename_in_place(HANDLE h, const std::wstring& name, bool replace) {
    using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    static const auto nt_set = reinterpret_cast<NtSetInformationFileFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
    if (!nt_set || name.empty() || name.find_first_of(L"\\/:") != std::wstring::npos) return false;
    std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    ri->ReplaceIfExists = replace ? TRUE : FALSE;
    ri->RootDirectory = nullptr;
    ri->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
    memcpy(ri->FileName, name.data(), name.size() * sizeof(wchar_t));
    IO_STATUS_BLOCK io{};
    const NTSTATUS st = nt_set(h, &io, ri, ULONG(buf.size()), 10 /* FileRenameInformation */);
    if (st < 0) SetLastError(ERROR_ACCESS_DENIED);
    return st >= 0;
}

// Deletes everything inside the open folder `dir`, through handles, never following a link: a junction or symbolic
// link is deleted itself. `shown` = the folder's path for messages; entries that cannot go (a DLL Frame Server has
// loaded) are appended to *left as paths.
void delete_contents(HANDLE dir, const std::wstring& shown, std::vector<std::wstring>* left) {
    std::vector<std::wstring> names;
    std::vector<uint8_t> buf(64 * 1024);
    FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(dir, cls, buf.data(), DWORD(buf.size()))) break;  // ERROR_NO_MORE_FILES
        cls = FileIdBothDirectoryInfo;
        for (size_t off = 0;;) {
            auto* fi = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buf.data() + off);
            std::wstring n(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
            if (n != L"." && n != L"..") names.push_back(n);
            if (!fi->NextEntryOffset) break;
            off += fi->NextEntryOffset;
        }
    }
    for (const auto& n : names) {
        DWORD e = 0;
        // Opened without following: if it is a link, this is the link.
        H c = open_rel(dir, n, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, 0, nullptr, &e);
        if (!c) {
            if (e != ERROR_FILE_NOT_FOUND) left->push_back(shown + L"\\" + n);
            continue;
        }
        const DWORD a = attributes_of(c.h);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT))
            delete_contents(c.h, shown + L"\\" + n, left);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
            FILE_BASIC_INFO bi{};
            bi.FileAttributes = (a & ~FILE_ATTRIBUTE_READONLY) | FILE_ATTRIBUTE_NORMAL;
            SetFileInformationByHandle(c.h, FileBasicInfo, &bi, sizeof bi);
        }
        if (!set_delete(c.h)) left->push_back(shown + L"\\" + n);
    }
}

// Deletes `name` inside the open folder `parent` (its contents first unless it is a link), never following a link.
// Returns false when it was there and could not be deleted completely.
bool delete_rel(HANDLE parent, const std::wstring& name, const std::wstring& shown, std::vector<std::wstring>* left) {
    DWORD e = 0;
    H c = open_rel(parent, name, DELETE | FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY | FILE_TRAVERSE,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN, 0, nullptr, &e);
    if (!c) return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    const DWORD a = attributes_of(c.h);
    const size_t before = left->size();
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) delete_contents(c.h, shown, left);
    if (left->size() != before) return false;  // not empty: the folder stays (its locked files are listed)
    if (!set_delete(c.h)) {
        left->push_back(shown);
        return false;
    }
    return true;
}

bool sha256_hex(const std::vector<uint8_t>& data, std::string* hex) {
    uint8_t out[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), out, sizeof out) < 0) return false;
    static const char* d = "0123456789abcdef";
    hex->clear();
    for (uint8_t b : out) {
        hex->push_back(d[b >> 4]);
        hex->push_back(d[b & 15]);
    }
    return true;
}

bool read_all(HANDLE h, std::vector<uint8_t>* out) {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 64 * 1024 * 1024) return false;
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return false;
    out->resize(size_t(sz.QuadPart));
    DWORD n = 0;
    return out->empty() || (ReadFile(h, out->data(), DWORD(out->size()), &n, nullptr) && n == out->size());
}

bool hex64_ok(const std::wstring& h) {
    if (h.size() != 64) return false;
    for (wchar_t c : h)
        if (!iswxdigit(c)) return false;
    return true;
}
std::string lower(std::string x) {
    for (char& c : x) c = char(tolower((unsigned char)c));
    return x;
}

bool elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring exe_dir() {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, p, DWORD(std::size(p)));
    std::wstring s(p, n);
    size_t k = s.find_last_of(L'\\');
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring full_path(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), DWORD(std::size(buf)), buf, nullptr);
    return n ? std::wstring(buf, n) : p;
}

// §9.2 lookup rule.
std::wstring find_dll() {
    for (const wchar_t* rel : {L"\\camera\\BeamerCameraSource.dll", L"\\BeamerCameraSource.dll"}) {
        std::wstring p = exe_dir() + rel;
        if (file_exists(p)) return p;
    }
    return L"";
}

// The trusted bases the two folder chains start from (Beamer\Camera[\<v>] below each).
std::wstring program_files_base(const VerbArgs& a) {
    return a.test_dir.empty() ? known_folder(FOLDERID_ProgramFiles) : a.test_dir + L"\\pf";
}
std::wstring program_data_base(const VerbArgs& a) {
    return a.test_dir.empty() ? known_folder(FOLDERID_ProgramData) : a.test_dir + L"\\pd";
}
bool testing(const VerbArgs& a) { return !a.test_reg.empty() || !a.test_dir.empty(); }

bool stop_frame_server() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    bool ok = false;
    for (const wchar_t* name : {L"FrameServer", L"FrameServerMonitor"}) {
        SC_HANDLE s = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!s) continue;
        SERVICE_STATUS st{};
        ControlService(s, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 50; i++) {  // <= 5 s
            if (QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_STOPPED) {
                if (wcscmp(name, L"FrameServer") == 0) ok = true;
                break;
            }
            Sleep(100);
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool reg_set_sz(HKEY k, const wchar_t* name, const std::wstring& v) {
    return RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

int out(int rc, const std::string& json) {
    fprintf(stdout, "%s\n", json.c_str());
    fflush(stdout);
    return rc;
}
int fail(int rc, const std::string& what) { return out(rc, "{\"ok\":false,\"error\":" + json_str(what) + "}"); }

// Copies the DLL into the open version folder `vdir` (admin-only), checks the SHA-256 of the copy there, and puts it
// in place as BeamerCameraSource.dll. A DLL Frame Server has loaded (same version, a dev rebuild) is renamed aside
// (allowed while loaded) and deleted at the next reboot.
bool install_dll(const std::wstring& src, HANDLE vdir, const std::wstring& vdir_path, const std::string& want_hex, std::string* err) {
    const std::wstring kName = L"BeamerCameraSource.dll", kNew = L"BeamerCameraSource.dll.new";
    std::vector<uint8_t> bytes;
    {
        // FILE_SHARE_READ only: nobody can change the file while it is read
        H f(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!f || !read_all(f.h, &bytes) || bytes.empty()) {
            *err = "read DLL " + utf8(src) + ": error " + std::to_string(GetLastError());
            return false;
        }
    }
    std::vector<std::wstring> left;
    delete_rel(vdir, kNew, vdir_path + L"\\" + kNew, &left);  // a leftover of an interrupted install
    DWORD e = 0;
    H n = open_rel(vdir, kNew, GENERIC_READ | GENERIC_WRITE | DELETE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, nullptr, &e);
    if (!n) {
        *err = "copy DLL: error " + std::to_string(e);
        return false;
    }
    DWORD w = 0;
    if (!WriteFile(n.h, bytes.data(), DWORD(bytes.size()), &w, nullptr) || w != bytes.size() || !FlushFileBuffers(n.h)) {
        *err = "copy DLL: write error " + std::to_string(GetLastError());
        set_delete(n.h);
        return false;
    }
    // The check is on the copy, read back from the protected folder through the handle only this process holds.
    std::vector<uint8_t> back;
    std::string got;
    if (!read_all(n.h, &back) || !sha256_hex(back, &got)) {
        *err = "copy DLL: read back error " + std::to_string(GetLastError());
        set_delete(n.h);
        return false;
    }
    if (got != want_hex) {
        set_delete(n.h);
        *err = "dll_hash_mismatch: the DLL copied from " + utf8(src) + " has SHA-256 " + got + ", expected " + want_hex + "; not registered";
        return false;
    }
    // Already in place with the same bytes (a repeated install): keep it, it may be loaded.
    {
        H cur = open_rel(vdir, kName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, nullptr, &e);
        std::vector<uint8_t> cb;
        std::string ch;
        DWORD links = 0;
        const DWORD ca = cur ? attributes_of(cur.h, &links) : INVALID_FILE_ATTRIBUTES;
        if (cur && ca != INVALID_FILE_ATTRIBUTES && !(ca & FILE_ATTRIBUTE_REPARSE_POINT) && links == 1 && read_all(cur.h, &cb) &&
            sha256_hex(cb, &ch) && ch == want_hex) {
            set_delete(n.h);
            return true;
        }
    }
    if (rename_in_place(n.h, kName, true)) return true;
    // Loaded by Frame Server: rename the loaded copy aside (allowed), then retry.
    const std::wstring aside = kName + L".old-" + std::to_wstring(GetTickCount64());
    {
        H cur = open_rel(vdir, kName, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                         FILE_NON_DIRECTORY_FILE, nullptr, &e);
        if (!cur || !rename_in_place(cur.h, aside, false)) {
            *err = "copy DLL: the current DLL cannot be replaced: error " + std::to_string(cur ? GetLastError() : e);
            set_delete(n.h);
            return false;
        }
    }
    MoveFileExW((vdir_path + L"\\" + aside).c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);  // admin-only folder
    if (!rename_in_place(n.h, kName, true)) {
        *err = "copy DLL: rename error " + std::to_string(GetLastError());
        set_delete(n.h);
        return false;
    }
    return true;
}

int vcam_install(const VerbArgs& a) {
    if (a.owner_sid.empty()) return fail(2, "bad_args: --owner-sid is required");
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(a.owner_sid.c_str(), &sid)) return fail(2, "bad_args: --owner-sid is not a SID");
    LocalFree(sid);
    // Required: an install without it would register whatever is in a possibly user-writable folder (R3 #8).
    if (!hex64_ok(a.dll_sha256)) return fail(2, "bad_args: --dll-sha256 <64 hex digits> is required");
    const std::string want_hex = lower(utf8(a.dll_sha256));
    if (!elevated()) return fail(3, "not_elevated");
    std::wstring src = a.dll.empty() ? find_dll() : full_path(a.dll);
    if (src.empty() || !file_exists(src)) return fail(4, "DLL not found" + (src.empty() ? std::string() : ": " + utf8(src)));
    const std::wstring ver = std::to_wstring(vcam::kDllVersion);
    std::string err;
    DWORD e = 0;
    // Program Files\Beamer\Camera\<v>, each folder created or validated through its parent's handle (R3 #1).
    const std::wstring pfb = program_files_base(a);
    if (!a.test_dir.empty()) CreateDirectoryW(pfb.c_str(), nullptr);
    H pf = open_base(pfb, &e);
    if (!pf) return fail(4, "open " + utf8(pfb) + ": error " + std::to_string(e));
    H pf_beamer = secure_dir(pf.h, L"Beamer", kSddlProgram, pfb + L"\\Beamer", &err);
    if (!pf_beamer) return fail(4, err);
    const std::wstring root = pfb + L"\\Beamer\\Camera";
    H pf_camera = secure_dir(pf_beamer.h, L"Camera", kSddlProgram, root, &err);
    if (!pf_camera) return fail(4, err);
    const std::wstring dir = root + L"\\" + ver;
    H vdir = secure_dir(pf_camera.h, ver, kSddlProgram, dir, &err);
    if (!vdir) return fail(4, err);
    const std::wstring dst = dir + L"\\BeamerCameraSource.dll";
    if (!install_dll(src, vdir.h, dir, want_hex, &err)) return fail(4, err);
    // ProgramData\Beamer\Camera (the DLL's log): the same way.
    const std::wstring pdb = program_data_base(a);
    if (!a.test_dir.empty()) CreateDirectoryW(pdb.c_str(), nullptr);
    H pd = open_base(pdb, &e);
    if (!pd) return fail(4, "open " + utf8(pdb) + ": error " + std::to_string(e));
    H pd_beamer = secure_dir(pd.h, L"Beamer", kSddlDataParent, pdb + L"\\Beamer", &err);
    if (!pd_beamer) return fail(4, err);
    H pd_camera = secure_dir(pd_beamer.h, L"Camera", kSddlData, pdb + L"\\Beamer\\Camera", &err);
    if (!pd_camera) return fail(4, err);
    // Registry: COM class (HKLM: Frame Server does not see HKCU classes, C2 Q1) and the install key.
    RegPaths p = reg_paths(a.test_reg);
    HKEY k = nullptr;
    LONG lr = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (p.clsid + L"\\InprocServer32").c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY,
                              nullptr, &k, nullptr);
    if (lr != ERROR_SUCCESS) return fail(4, "registry (COM class): error " + std::to_string(lr));
    bool ok = reg_set_sz(k, nullptr, dst) && reg_set_sz(k, L"ThreadingModel", L"Both");
    RegCloseKey(k);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, p.clsid.c_str(), 0, KEY_WRITE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        reg_set_sz(k, nullptr, vcam::kFriendlyName);
        RegCloseKey(k);
    }
    lr = RegCreateKeyExW(HKEY_LOCAL_MACHINE, p.camera.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
    if (!ok || lr != ERROR_SUCCESS) return fail(4, "registry (camera key): error " + std::to_string(lr));
    wchar_t guid[64] = {};
    DWORD gsz = sizeof guid;
    std::wstring g;
    if (RegGetValueW(k, nullptr, L"InstallGuid", RRF_RT_REG_SZ, nullptr, guid, &gsz) == ERROR_SUCCESS && install_guid_ok(guid)) g = guid;
    if (g.empty()) {
        GUID ng;
        CoCreateGuid(&ng);
        StringFromGUID2(ng, guid, 64);
        g = guid;
    }
    DWORD dv = DWORD(vcam::kDllVersion);
    ok = reg_set_sz(k, L"InstallGuid", g) && reg_set_sz(k, L"OwnerSid", a.owner_sid) && reg_set_sz(k, L"DllPath", dst) &&
         RegSetValueExW(k, L"DllVersion", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dv), sizeof dv) == ERROR_SUCCESS;
    RegCloseKey(k);
    if (!ok) return fail(4, "registry (values)");
    // Older version folders that are not locked (a loaded DLL stays until Frame Server lets it go; every elevated
    // action retries, §9.2). Through the Camera folder's handle, never following a link.
    std::vector<std::wstring> left;
    {
        std::vector<std::wstring> others;
        std::vector<uint8_t> buf(64 * 1024);
        FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
        while (GetFileInformationByHandleEx(pf_camera.h, cls, buf.data(), DWORD(buf.size()))) {
            cls = FileIdBothDirectoryInfo;
            for (size_t off = 0;;) {
                auto* fi = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buf.data() + off);
                std::wstring n(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
                if (n != L"." && n != L".." && _wcsicmp(n.c_str(), ver.c_str()) != 0) others.push_back(n);
                if (!fi->NextEntryOffset) break;
                off += fi->NextEntryOffset;
            }
        }
        for (const auto& n : others) delete_rel(pf_camera.h, n, root + L"\\" + n, &left);
    }
    std::string j = "{\"ok\":true,\"dll\":" + json_str(utf8(dst)) + ",\"install_guid\":" + json_str(utf8(g)) +
                    ",\"dll_version\":" + std::to_string(vcam::kDllVersion) + ",\"stale_left\":" + std::to_string(left.size()) + "}";
    return out(0, j);
}

// Deletes <base>\Beamer\Camera and then <base>\Beamer when it is empty, through handles, never following a link: a
// junction or symbolic link at Beamer or Camera is removed itself, never what it points to (R3 #1).
void remove_chain(const std::wstring& base, std::vector<std::wstring>* left) {
    DWORD e = 0;
    H b = open_base(base, &e);
    if (!b) return;
    H beamer = open_rel(b.h, L"Beamer", DELETE | FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, nullptr, &e);
    if (!beamer) return;  // not there
    const DWORD a = attributes_of(beamer.h);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!set_delete(beamer.h)) left->push_back(base + L"\\Beamer");  // a link (not ours): the link only
        return;
    }
    delete_rel(beamer.h, L"Camera", base + L"\\Beamer\\Camera", left);
    // the parent too when nothing else is in it (a non-empty folder refuses the delete and stays)
    std::vector<uint8_t> buf(4096);
    bool empty = true;
    if (GetFileInformationByHandleEx(beamer.h, FileIdBothDirectoryRestartInfo, buf.data(), DWORD(buf.size()))) {
        for (size_t off = 0;;) {
            auto* fi = reinterpret_cast<FILE_ID_BOTH_DIR_INFO*>(buf.data() + off);
            std::wstring n(fi->FileName, fi->FileNameLength / sizeof(wchar_t));
            if (n != L"." && n != L"..") empty = false;
            if (!fi->NextEntryOffset) break;
            off += fi->NextEntryOffset;
        }
    }
    if (empty) set_delete(beamer.h);
}

int vcam_uninstall(const VerbArgs& a) {
    if (!elevated()) return fail(3, "not_elevated");
    RegPaths p = reg_paths(a.test_reg);
    std::string errs;
    for (const std::wstring& key : {p.clsid, p.camera}) {
        LONG r = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
        if (r == ERROR_SUCCESS) r = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), KEY_WOW64_64KEY, 0);
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) errs += "registry " + utf8(key) + ": error " + std::to_string(r) + "; ";
    }
    // HKLM\SOFTWARE\Beamer once empty (RegDeleteKeyEx refuses a key with subkeys)
    if (a.test_reg.empty()) RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Beamer", KEY_WOW64_64KEY, 0);
    std::vector<std::wstring> left;
    bool stopped = false;
    for (int pass = 0; pass < 2; pass++) {
        left.clear();
        remove_chain(program_files_base(a), &left);
        remove_chain(program_data_base(a), &left);
        if (left.empty() || testing(a) || stopped) break;
        stopped = stop_frame_server();  // the DLL is loaded: every camera pauses ~1 s (warned in the page)
    }
    // Left until reboot: only what is inside Program Files\Beamer (admin-only folders, checked above), never a path
    // under ProgramData.
    const std::wstring pf_prefix = program_files_base(a) + L"\\Beamer\\";
    for (const auto& l : left)
        if (_wcsnicmp(l.c_str(), pf_prefix.c_str(), pf_prefix.size()) == 0) MoveFileExW(l.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!errs.empty()) return fail(4, errs);
    return out(0, "{\"ok\":true,\"frameserver_stopped\":" + std::string(stopped ? "true" : "false") +
                      ",\"left_until_reboot\":" + std::to_string(left.size()) + "}");
}

int vcam_remove(const VerbArgs& a) {
    ComScope com;
    InstallInfo i = read_install(a.test_reg);
    if (i.install == "missing" || i.install == "unsupported") return out(0, "{\"ok\":true,\"removed\":false,\"install\":" + json_str(i.install) + "}");
    if (!testing(a)) {
        HRESULT hr = remove_camera();
        if (FAILED(hr)) return fail(4, "Remove: " + hr_text(hr));
    }
    return out(0, "{\"ok\":true,\"removed\":" + std::string(testing(a) ? "false" : "true") + "}");
}

int vcam_status(const VerbArgs& a) {
    ComScope com;
    WinVideoOptions o;
    o.reg_base = a.test_reg;
    o.create_camera = false;
    Vcam v(o);
    v.poll();
    return out(0, vcam_event_json(v.state(false)));
}

}  // namespace
}  // namespace win

bool run_vcam_verb_if_requested(int* rc) {
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return false;
    std::vector<std::wstring> a(w, w + n);
    LocalFree(w);
    if (a.size() < 2) return false;
    const std::wstring verb = a[1];
    if (verb != L"vcam-install" && verb != L"vcam-uninstall" && verb != L"vcam-remove" && verb != L"vcam-status") return false;
    win::VerbArgs v;
    for (size_t i = 2; i < a.size(); i++) {
        auto next = [&](std::wstring* dst) {
            if (i + 1 < a.size()) *dst = a[++i];
            else v.bad = true;
        };
        if (a[i] == L"--owner-sid") next(&v.owner_sid);
        else if (a[i] == L"--dll") next(&v.dll);
        else if (a[i] == L"--dll-sha256") next(&v.dll_sha256);
        else if (a[i] == L"--test-reg") next(&v.test_reg);
        else if (a[i] == L"--test-dir") next(&v.test_dir);
        else if (a[i] == L"--log-level") { std::wstring ignored; next(&ignored); }
        else v.bad = true;
    }
    if (!v.test_dir.empty() && v.test_reg.empty()) v.bad = true;  // a test folder never gets the real COM class
    if (v.bad) {
        *rc = win::fail(2, "bad_args: " + win::utf8(verb) + " [--owner-sid SID --dll-sha256 HEX] [--dll PATH]");
        return true;
    }
    if (verb == L"vcam-install") *rc = win::vcam_install(v);
    else if (verb == L"vcam-uninstall") *rc = win::vcam_uninstall(v);
    else if (verb == L"vcam-remove") *rc = win::vcam_remove(v);
    else *rc = win::vcam_status(v);
    return true;
}

}  // namespace beamer::audio
