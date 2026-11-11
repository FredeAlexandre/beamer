// SPDX-License-Identifier: GPL-3.0-only
// Which app uses a camera (camera PHASE2 §4.6.6, CW4), best effort. Windows records camera use per app in the
// consent store: HKCU\...\CapabilityAccessManager\ConsentStore\webcam\<package family name> for packaged apps and
// ...\webcam\NonPackaged\<exe path with '#' for '\'> for desktop apps; an entry whose LastUsedTimeStart is later than
// its LastUsedTimeStop is in use now. It covers every camera, so the caller asks only while Beamer Camera itself is
// streaming, and reports a name only when exactly one app is in use (else nothing: never "probably").
#include "video_backend_win.h"

#include <shlwapi.h>

#include <vector>

#pragma comment(lib, "version.lib")

namespace beamer::audio::win {

namespace {

const wchar_t kStore[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
const wchar_t kPackages[] =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages";

std::vector<std::wstring> subkeys(HKEY k) {
    std::vector<std::wstring> out;
    wchar_t name[512];
    for (DWORD i = 0;; i++) {
        DWORD n = 512;
        if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.emplace_back(name, n);
    }
    return out;
}

bool in_use(HKEY parent, const std::wstring& sub) {
    uint64_t start = 0, stop = 0;
    DWORD sz = sizeof start;
    if (RegGetValueW(parent, sub.c_str(), L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &sz) != ERROR_SUCCESS) return false;
    sz = sizeof stop;
    if (RegGetValueW(parent, sub.c_str(), L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &sz) != ERROR_SUCCESS) stop = 0;
    return start > stop;
}

std::wstring file_description(const std::wstring& path) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return L"";
    std::vector<uint8_t> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return L"";
    struct Lang { WORD lang, cp; }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang)) return L"";
    wchar_t q[80];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* d = nullptr;
    if (!VerQueryValueW(buf.data(), q, reinterpret_cast<void**>(&d), &len) || !d || !*d) return L"";
    return d;
}

std::wstring desktop_name(const std::wstring& key) {
    std::wstring path = key;
    for (auto& c : path)
        if (c == L'#') c = L'\\';
    std::wstring d = file_description(path);
    if (!d.empty()) return d;
    size_t s = path.find_last_of(L'\\');
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

std::wstring package_name(const std::wstring& family) {
    size_t us = family.rfind(L'_');
    std::wstring name = us == std::wstring::npos ? family : family.substr(0, us);
    std::wstring pub = us == std::wstring::npos ? L"" : family.substr(us + 1);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPackages, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        for (const auto& full : subkeys(k)) {
            if (full.size() <= name.size() + 1 || full.compare(0, name.size() + 1, name + L"_") != 0) continue;
            if (!pub.empty() && (full.size() < pub.size() + 1 || full.compare(full.size() - pub.size() - 1, std::wstring::npos, L"_" + pub) != 0))
                continue;
            wchar_t v[512];
            DWORD sz = sizeof v;
            if (RegGetValueW(k, full.c_str(), L"DisplayName", RRF_RT_REG_SZ, nullptr, v, &sz) != ERROR_SUCCESS) continue;
            std::wstring dn = v;
            if (dn.rfind(L"@", 0) == 0 || dn.rfind(L"ms-resource:", 0) == 0) {
                std::vector<std::wstring> tries;
                if (dn[0] == L'@') {
                    tries.push_back(dn);
                } else {
                    // "ms-resource:AppStoreName" is short for ms-resource://<name>/Resources/AppStoreName
                    tries.push_back(L"@{" + full + L"?" + dn + L"}");
                    std::wstring res = dn.substr(12);
                    if (res.rfind(L"//", 0) != 0) {
                        if (res.find(L'/') == std::wstring::npos) tries.push_back(L"@{" + full + L"?ms-resource://" + name + L"/Resources/" + res + L"}");
                        else tries.push_back(L"@{" + full + L"?ms-resource://" + name + L"/" + res + L"}");
                    }
                }
                bool resolved = false;
                for (const auto& src : tries) {
                    wchar_t out[512];
                    if (SUCCEEDED(SHLoadIndirectString(src.c_str(), out, 512, nullptr)) && out[0]) {
                        dn = out;
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) continue;
            }
            RegCloseKey(k);
            return dn;
        }
        RegCloseKey(k);
    }
    return name;
}

}  // namespace

std::string camera_app_in_use(const std::wstring& root, int* count_out) {
    HKEY store;
    int count = 0;
    std::wstring found;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, root.empty() ? kStore : root.c_str(), 0, KEY_READ, &store) == ERROR_SUCCESS) {
        for (const auto& sub : subkeys(store)) {
            if (sub == L"NonPackaged") {
                HKEY np;
                if (RegOpenKeyExW(store, L"NonPackaged", 0, KEY_READ, &np) != ERROR_SUCCESS) continue;
                for (const auto& app : subkeys(np))
                    if (in_use(np, app)) {
                        count++;
                        found = L"np:" + app;
                    }
                RegCloseKey(np);
            } else if (in_use(store, sub)) {
                count++;
                found = L"pk:" + sub;
            }
        }
        RegCloseKey(store);
    }
    if (count_out) *count_out = count;
    if (count != 1) return "";
    return utf8(found.rfind(L"np:", 0) == 0 ? desktop_name(found.substr(3)) : package_name(found.substr(3)));
}

}  // namespace beamer::audio::win
