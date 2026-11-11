// SPDX-License-Identifier: GPL-3.0-only
// Helper verbs for Beamer Camera's install (camera PHASE2 §3.1, §9.2, CW4), run without --control:
//   beamer-audio vcam-install --owner-sid <SID> [--dll <path>]   (elevated) copy the DLL to
//        %ProgramFiles%\Beamer\Camera\<kDllVersion>\, make %ProgramData%\Beamer\Camera (SYSTEM, LocalService,
//        Administrators write; Users read), register the COM class in HKLM and write HKLM\SOFTWARE\Beamer\Camera
//        (InstallGuid kept, OwnerSid, DllVersion, DllPath), delete older version folders that are not locked
//   beamer-audio vcam-uninstall                                   (elevated) the reverse; stops FrameServer only
//        when a file is locked
//   beamer-audio vcam-remove                                      (unelevated) Remove() this user's camera
//   beamer-audio vcam-status                                      prints the `vcam` event once
// Exit codes: 0 ok, 2 bad args, 3 not elevated, 4 failed. Each verb prints one JSON line.
// Without --dll the DLL is looked up beside the helper: <exe dir>\camera\BeamerCameraSource.dll, then
// <exe dir>\BeamerCameraSource.dll (§9.2; the dev build writes native\audio\build\win\camera\).
// Test seams (video_check only, never used by Beamer): --test-reg <HKLM subkey> puts every registry write under it
// (see WinVideoOptions::reg_base) and --test-dir <dir> puts the two folders under it; with either, the FrameServer
// service is never touched.
#include "video_backend_win.h"

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>

namespace beamer::audio {

namespace win {
namespace {

struct VerbArgs {
    std::wstring owner_sid, dll, test_reg, test_dir;
    bool bad = false;
};

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

std::wstring program_files_root(const VerbArgs& a) {
    return a.test_dir.empty() ? known_folder(FOLDERID_ProgramFiles) + L"\\Beamer\\Camera" : a.test_dir + L"\\pf\\Beamer\\Camera";
}
std::wstring program_data_root(const VerbArgs& a) {
    return a.test_dir.empty() ? known_folder(FOLDERID_ProgramData) + L"\\Beamer\\Camera" : a.test_dir + L"\\pd\\Beamer\\Camera";
}
bool testing(const VerbArgs& a) { return !a.test_reg.empty() || !a.test_dir.empty(); }

bool files_equal(const std::wstring& a, const std::wstring& b) {
    auto read = [](const std::wstring& p, std::vector<uint8_t>* out) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        out->resize(size_t(sz.QuadPart));
        DWORD n = 0;
        bool ok = out->empty() || (ReadFile(h, out->data(), DWORD(out->size()), &n, nullptr) && n == out->size());
        CloseHandle(h);
        return ok;
    };
    std::vector<uint8_t> x, y;
    return read(a, &x) && read(b, &y) && x == y;
}

// Deletes a folder tree; returns the paths that could not be deleted (locked).
void delete_tree(const std::wstring& dir, std::vector<std::wstring>* left) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p, left);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(p.c_str())) left->push_back(p);
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES && !RemoveDirectoryW(dir.c_str()) && left->empty())
        left->push_back(dir);
}

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

// The ProgramData folder: SYSTEM, LocalService and Administrators full control, Users read; not inherited (§4.6.3).
bool make_data_dir(const std::wstring& dir, std::string* err) {
    const wchar_t* sddl = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;LS)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) {
        *err = "sddl";
        return false;
    }
    size_t k = dir.find_last_of(L'\\');
    if (k != std::wstring::npos) SHCreateDirectoryExW(nullptr, dir.substr(0, k).c_str(), nullptr);
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    bool ok = CreateDirectoryW(dir.c_str(), &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
    if (ok) {  // existing folder: enforce the ACL
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
        DWORD r = SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
                                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        if (r != ERROR_SUCCESS) {
            ok = false;
            *err = "folder ACL: error " + std::to_string(r);
        }
    } else {
        *err = "create " + utf8(dir) + ": error " + std::to_string(GetLastError());
    }
    LocalFree(sd);
    return ok;
}

int vcam_install(const VerbArgs& a) {
    if (a.owner_sid.empty()) return fail(2, "bad_args: --owner-sid is required");
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(a.owner_sid.c_str(), &sid)) return fail(2, "bad_args: --owner-sid is not a SID");
    LocalFree(sid);
    if (!elevated()) return fail(3, "not_elevated");
    std::wstring src = a.dll.empty() ? find_dll() : full_path(a.dll);
    if (src.empty() || !file_exists(src)) return fail(4, "DLL not found" + (src.empty() ? std::string() : ": " + utf8(src)));
    const std::wstring root = program_files_root(a);
    const std::wstring ver = std::to_wstring(vcam::kDllVersion);
    const std::wstring dir = root + L"\\" + ver;
    const std::wstring dst = dir + L"\\BeamerCameraSource.dll";
    // The folder inherits Program Files' ACL (Users read, so LocalService can load the DLL).
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) return fail(4, "create " + utf8(dir) + ": error " + std::to_string(r));
    if (_wcsicmp(full_path(src).c_str(), full_path(dst).c_str()) != 0 && !files_equal(src, dst)) {
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            DWORD e = GetLastError();
            // Loaded by Frame Server (same version number, different bytes: a dev rebuild): a loaded DLL can still be
            // renamed; the old copy goes at the next reboot.
            std::wstring aside = dst + L".old-" + std::to_wstring(GetTickCount64());
            if (!MoveFileExW(dst.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING) || !CopyFileW(src.c_str(), dst.c_str(), FALSE))
                return fail(4, "copy DLL: error " + std::to_string(e));
            MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    std::string err;
    if (!make_data_dir(program_data_root(a), &err)) return fail(4, err);
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
    // action retries, §9.2).
    std::vector<std::wstring> left;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == L"." || n == L".." || n == ver) continue;
            delete_tree(root + L"\\" + n, &left);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::string j = "{\"ok\":true,\"dll\":" + json_str(utf8(dst)) + ",\"install_guid\":" + json_str(utf8(g)) +
                    ",\"dll_version\":" + std::to_string(vcam::kDllVersion) + ",\"stale_left\":" + std::to_string(left.size()) + "}";
    return out(0, j);
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
    std::vector<std::wstring> left;
    bool stopped = false;
    for (int pass = 0; pass < 2; pass++) {
        left.clear();
        delete_tree(program_files_root(a), &left);
        delete_tree(program_data_root(a), &left);
        if (left.empty() || testing(a) || stopped) break;
        stopped = stop_frame_server();  // the DLL is loaded: every camera pauses ~1 s (warned in the page)
    }
    for (const auto& l : left) MoveFileExW(l.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
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
        else if (a[i] == L"--test-reg") next(&v.test_reg);
        else if (a[i] == L"--test-dir") next(&v.test_dir);
        else if (a[i] == L"--log-level") { std::wstring ignored; next(&ignored); }
        else v.bad = true;
    }
    if (v.bad) {
        *rc = win::fail(2, "bad_args: " + win::utf8(verb) + " [--owner-sid SID] [--dll PATH]");
        return true;
    }
    if (verb == L"vcam-install") *rc = win::vcam_install(v);
    else if (verb == L"vcam-uninstall") *rc = win::vcam_uninstall(v);
    else if (verb == L"vcam-remove") *rc = win::vcam_remove(v);
    else *rc = win::vcam_status(v);
    return true;
}

}  // namespace beamer::audio
