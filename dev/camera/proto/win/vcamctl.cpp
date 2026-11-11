// SPDX-License-Identifier: GPL-3.0-only
// C2 prototype registrar: registers the media source COM class and creates / removes the virtual camera.
//   vcamctl supported
//   vcamctl regcom hklm|hkcu <dll>      vcamctl unregcom hklm|hkcu
//   vcamctl create session|system user|all [--hold SECONDS] [--name NAME]   (Start, then hold; exit = release)
//   vcamctl remove session|system user|all [--name NAME]
//   vcamctl setup-dir                   (C:\ProgramData\BeamerCamC2, writable by LocalService)
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <string>
#include "common.h"

using Microsoft::WRL::ComPtr;
using namespace c2;

static int fail(const char* what, HRESULT hr) {
  printf("{\"ok\":false,\"what\":\"%s\",\"hr\":\"0x%08lx\"}\n", what, (unsigned long)hr);
  return 1;
}

static HRESULT regcom(HKEY root, const wchar_t* dll, bool add) {
  wchar_t key[128];
  swprintf_s(key, L"Software\\Classes\\CLSID\\%s", kClsidStr);
  if (!add) {
    LONG r = RegDeleteTreeW(root, key);
    return r == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(r);
  }
  wchar_t sub[160];
  swprintf_s(sub, L"%s\\InprocServer32", key);
  HKEY k;
  LONG r = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
  if (r) return HRESULT_FROM_WIN32(r);
  RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)dll, (DWORD)(wcslen(dll) + 1) * 2);
  RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Both", 10);
  RegCloseKey(k);
  return S_OK;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    printf("usage: see source header\n");
    return 2;
  }
  std::wstring cmd = argv[1];
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  if (cmd == L"supported") {
    BOOL ok = FALSE;
    hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &ok);
    printf("{\"ok\":%s,\"hr\":\"0x%08lx\",\"supported\":%s}\n", SUCCEEDED(hr) ? "true" : "false", (unsigned long)hr, ok ? "true" : "false");
    return 0;
  }
  if (cmd == L"setup-dir") {
    SECURITY_ATTRIBUTES sa;
    make_sa(&sa);
    BOOL ok = CreateDirectoryW(L"C:\\ProgramData\\BeamerCamC2", &sa) || GetLastError() == ERROR_ALREADY_EXISTS;
    printf("{\"ok\":%s}\n", ok ? "true" : "false");
    return ok ? 0 : 1;
  }
  if (cmd == L"regcom" || cmd == L"unregcom") {
    if (argc < 3) return 2;
    HKEY root = _wcsicmp(argv[2], L"hkcu") == 0 ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    wchar_t full[MAX_PATH] = {0};
    if (cmd == L"regcom") {
      if (argc < 4) return 2;
      GetFullPathNameW(argv[3], MAX_PATH, full, nullptr);
    }
    hr = regcom(root, full, cmd == L"regcom");
    if (FAILED(hr)) return fail("regcom", hr);
    printf("{\"ok\":true}\n");
    return 0;
  }
  if (cmd == L"create" || cmd == L"remove") {
    if (argc < 4) return 2;
    auto life = _wcsicmp(argv[2], L"system") == 0 ? MFVirtualCameraLifetime_System : MFVirtualCameraLifetime_Session;
    auto access = _wcsicmp(argv[3], L"all") == 0 ? MFVirtualCameraAccess_AllUsers : MFVirtualCameraAccess_CurrentUser;
    const wchar_t* name = kFriendlyName;
    double hold = 0;
    for (int i = 4; i + 1 < argc; i++) {
      if (!wcscmp(argv[i], L"--name")) name = argv[++i];
      else if (!wcscmp(argv[i], L"--hold")) hold = _wtof(argv[++i]);
    }
    ComPtr<IMFVirtualCamera> cam;
    int64_t t0 = unix_us();
    hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, life, access, name, kClsidStr, nullptr, 0, &cam);
    int64_t t1 = unix_us();
    if (FAILED(hr)) return fail("MFCreateVirtualCamera", hr);
    if (cmd == L"remove") {
      hr = cam->Remove();
      if (FAILED(hr)) return fail("Remove", hr);
      printf("{\"ok\":true,\"removed\":true}\n");
      return 0;
    }
    hr = cam->Start(nullptr);
    int64_t t2 = unix_us();
    if (FAILED(hr)) return fail("Start", hr);
    LPWSTR link = nullptr;
    UINT32 len = 0;
    std::string sym;
    if (SUCCEEDED(cam->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)) && link) {
      char tmp[1024];
      WideCharToMultiByte(CP_UTF8, 0, link, -1, tmp, sizeof tmp, nullptr, nullptr);
      sym = tmp;
      for (auto& ch : sym)
        if (ch == '\\') ch = '/';
      CoTaskMemFree(link);
    }
    printf("{\"ok\":true,\"create_ms\":%.1f,\"start_ms\":%.1f,\"symlink\":\"%s\"}\n", (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, sym.c_str());
    fflush(stdout);
    if (hold < 0) {
      for (;;) Sleep(INFINITE);
    }
    Sleep((DWORD)(hold * 1000));
    printf("{\"released\":true}\n");
    return 0;
  }
  printf("unknown command\n");
  return 2;
}
