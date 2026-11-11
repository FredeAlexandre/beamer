// SPDX-License-Identifier: GPL-3.0-only
// Helper stand-in for the Beamer Camera source (port of C2's proto/win/feeder.cpp to shm_protocol.h). Runs in
// the user's session like beamer-audio.exe; JSON lines on stdout (t = unix µs).
//
//   feeder run [--size WxH] [--follow] [--seconds N] [--always] [--fps F] [--mode live|waiting]
//              [--waiting-at S] [--live-at S] [--beat-stop-at S] [--switch-every S]
//        Creates .Demand (owner check), opens the section on demand (or by polling every 1 s), beats from the
//        presenting loop, and while an app streams writes stamped NV12 pictures (C2 stamp, origin 3) into the
//        live slots (mode live) or a stamped uniform picture (origin 2) into slot 3 (mode waiting). Times in
//        --*-at are seconds after the first streaming start; --beat-stop-at simulates a wedged helper (no beat,
//        no writes, process alive).
//   feeder vcam create|remove        MFCreateVirtualCamera(System, CurrentUser, "Beamer Camera") + Start / Remove
//   feeder inspect                   object names, owners, DACLs, header fields and counters
//   feeder squat [--seconds N]       pre-creates the .Frame event with a default DACL (refusal test)
//   feeder loadtest DLL              loads the DLL in this process (as MFCreateVirtualCamera's validation does):
//                                    media types offered, and no kernel object / no log created
//   feeder cpu PID SECONDS           CPU of another process (% of one core: cycles and times)
#include "tool_common.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <cmath>

using Microsoft::WRL::ComPtr;
using namespace tools;

namespace {

bool offered(uint32_t w, uint32_t h) {
  for (const auto& t : vc::kOffered)
    if (t.w == w && t.h == h) return true;
  return false;
}

HANDLE create_demand(const Names& n, bool* squatted) {
  std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;" + my_sid() + L")";
  PSECURITY_DESCRIPTOR sd = nullptr;
  ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
  SECURITY_ATTRIBUTES sa = {sizeof sa, sd, FALSE};
  HANDLE ev = CreateEventExW(&sa, n.demand.c_str(), 0, SYNCHRONIZE | EVENT_MODIFY_STATE | READ_CONTROL);
  DWORD err = GetLastError();
  LocalFree(sd);
  *squatted = false;
  if (ev && err == ERROR_ALREADY_EXISTS) {
    std::string owner;
    object_sddl(ev, &owner);
    if (owner != narrow(my_sid())) {
      *squatted = true;
      CloseHandle(ev);
      ev = nullptr;
    }
  }
  printf("{\"event\":\"demand_event\",\"ok\":%s,\"existed\":%s,\"squatted\":%s,\"err\":%lu}\n", ev ? "true" : "false",
         err == ERROR_ALREADY_EXISTS ? "true" : "false", *squatted ? "true" : "false", ev ? 0 : err);
  return ev;
}

struct RunOpts {
  uint32_t w = 1280, h = 720;
  bool follow = false, always = false, waiting = false;
  double seconds = 60, fps = 30, waiting_at = -1, live_at = -1, beat_stop_at = -1, switch_every = 0;
};

int run(const RunOpts& o) {
  Names n = names();
  if (!n.ok) {
    printf("{\"ok\":false,\"error\":\"not installed (no InstallGuid)\"}\n");
    return 1;
  }
  bool squatted = false;
  HANDLE demand = create_demand(n, &squatted);
  timeBeginPeriod(1);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  Section sec;
  std::vector<uint8_t> pic((size_t)vc::kSlotBytes);
  int64_t t_end = unix_us() + (int64_t)(o.seconds * 1e6), t_start = 0, next_poll = 0;
  uint32_t seen_seq = UINT32_MAX, cw = o.w, ch = o.h, latest = 0, picture = 0;
  bool streaming = false, waiting = o.waiting, beat_stopped = false, did_wait = false, did_live = false;
  int64_t produced = 0, t_switch = 0;
  double write_ms = 0;
  int64_t due = qpc_now();
  const int64_t period = (int64_t)(qpc_freq() / o.fps);
  while (unix_us() < t_end) {
    int64_t now_u = unix_us();
    if (!sec.hdr && now_u >= next_poll) {
      sec.open(n);
      next_poll = now_u + 1000000;
      if (sec.hdr) sec.hdr->helper_pid = GetCurrentProcessId();
    }
    if (sec.hdr) {
      uint32_t q = sec.hdr->state_seq.load(std::memory_order_acquire);
      if (q != seen_seq) {
        seen_seq = q;
        bool s = sec.hdr->streaming == 1;
        uint32_t ww = sec.hdr->want_w, wh = sec.hdr->want_h;
        if (!offered(ww, wh)) ww = vc::kOffered[0].w, wh = vc::kOffered[0].h;   // §4.6.3, other direction
        printf("{\"event\":\"demand\",\"t\":%lld,\"streaming\":%s,\"want\":\"%ux%u\",\"state_seq\":%u,\"signal_ms\":%.3f}\n", (long long)now_u,
               s ? "true" : "false", ww, wh, q, qpc_to_ms(qpc_now() - (int64_t)sec.hdr->state_qpc));
        if (s && !streaming && !t_start) {
          t_start = now_u;
          t_switch = now_u + (int64_t)(o.switch_every * 1e6);
        }
        if (!s && streaming)
          printf("{\"event\":\"stats\",\"t\":%lld,\"produced\":%lld,\"write_ms_mean\":%.3f,\"delivered\":%llu,\"placeholders\":%llu,\"repeats\":%llu,"
                 "\"rejected\":%llu}\n",
                 (long long)now_u, produced, produced ? write_ms / produced : 0.0, (unsigned long long)sec.hdr->delivered.load(),
                 (unsigned long long)sec.hdr->placeholders.load(), (unsigned long long)sec.hdr->repeats.load(),
                 (unsigned long long)sec.hdr->rejected.load());
        streaming = s;
        if (o.follow && s) cw = ww, ch = wh;
      }
    }
    double since = t_start ? (now_u - t_start) / 1e6 : -1;
    if (o.beat_stop_at >= 0 && since >= o.beat_stop_at && !beat_stopped) {
      beat_stopped = true;
      printf("{\"event\":\"beat_stop\",\"t\":%lld}\n", (long long)now_u);
    }
    if (o.waiting_at >= 0 && since >= o.waiting_at && !did_wait) {
      did_wait = true;
      waiting = true;
      printf("{\"event\":\"mode\",\"mode\":\"waiting\",\"t\":%lld}\n", (long long)now_u);
    }
    if (o.live_at >= 0 && since >= o.live_at && !did_live) {
      did_live = true;
      waiting = false;
      printf("{\"event\":\"mode\",\"mode\":\"live\",\"t\":%lld}\n", (long long)now_u);
    }
    if (o.switch_every > 0 && t_start && now_u >= t_switch) {
      bool big = cw == 1920;
      cw = big ? 1280 : 1920;
      ch = big ? 720 : 1080;
      t_switch += (int64_t)(o.switch_every * 1e6);
      printf("{\"event\":\"switch\",\"t\":%lld,\"size\":\"%ux%u\"}\n", (long long)now_u, cw, ch);
    }
    // the presenting loop: beat + present in the same iteration (§4.6.4)
    if (sec.hdr && !beat_stopped) {
      if (streaming || o.always) {
        int64_t w0 = qpc_now();
        uint32_t pn = picture++;
        if (waiting) {
          sec.write_slot(vc::kLiveSlots, cw, ch, cw, pn, [&](uint8_t* p) {
            uniform_nv12(p, cw, ch, 0x60);
            stamp_nv12(p, cw, cw, ch, pn, (uint32_t)unix_us(), kOriginPlaceholder);
          });
          sec.hdr->mode.store(vc::kModeWaiting, std::memory_order_release);
        } else {
          pattern_nv12(pic.data(), cw, cw, ch, pn);
          uint32_t next = (latest + 1) % vc::kLiveSlots;
          sec.write_slot(next, cw, ch, cw, pn, [&](uint8_t* p) {
            stamp_nv12(pic.data(), cw, cw, ch, pn, (uint32_t)unix_us(), kOriginHelper);
            memcpy(p, pic.data(), (size_t)cw * ch * 3 / 2);
          });
          latest = next;
          sec.hdr->latest.store(next, std::memory_order_release);
          sec.hdr->mode.store(vc::kModeLive, std::memory_order_release);
        }
        sec.hdr->beat_qpc.store((uint64_t)qpc_now(), std::memory_order_release);
        if (sec.frame) SetEvent(sec.frame);
        write_ms += qpc_to_ms(qpc_now() - w0);
        produced++;
      } else {
        sec.hdr->beat_qpc.store((uint64_t)qpc_now(), std::memory_order_release);   // idle loop beats too
      }
    }
    // next iteration: the frame clock, or a demand change
    due += period;
    int64_t now = qpc_now();
    if (due < now - period * 3) due = now + period;
    if (due > now) {
      LARGE_INTEGER li;
      li.QuadPart = -(int64_t)((double)(due - now) * 1e7 / qpc_freq());
      SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
      HANDLE hs[2] = {timer, demand};
      WaitForMultipleObjects(demand ? 2 : 1, hs, FALSE, INFINITE);
    }
  }
  printf("{\"event\":\"exit\",\"t\":%lld,\"produced\":%lld}\n", (long long)unix_us(), produced);
  return 0;
}

int vcam(const std::wstring& what) {
  ComPtr<IMFVirtualCamera> cam;
  int64_t t0 = unix_us();
  HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_CurrentUser,
                                     vc::kFriendlyName, vc::kClsidText, nullptr, 0, &cam);
  int64_t t1 = unix_us();
  if (FAILED(hr)) {
    printf("{\"ok\":false,\"what\":\"MFCreateVirtualCamera\",\"hr\":\"0x%08lx\"}\n", (unsigned long)hr);
    return 1;
  }
  if (what == L"remove") {
    hr = cam->Remove();
    printf("{\"ok\":%s,\"removed\":true,\"hr\":\"0x%08lx\"}\n", SUCCEEDED(hr) ? "true" : "false", (unsigned long)hr);
    return SUCCEEDED(hr) ? 0 : 1;
  }
  hr = cam->Start(nullptr);
  int64_t t2 = unix_us();
  printf("{\"ok\":%s,\"hr\":\"0x%08lx\",\"create_ms\":%.1f,\"start_ms\":%.1f}\n", SUCCEEDED(hr) ? "true" : "false", (unsigned long)hr, (t1 - t0) / 1000.0,
         (t2 - t1) / 1000.0);
  return SUCCEEDED(hr) ? 0 : 1;
}

void dump_object(const char* label, HANDLE h, DWORD err) {
  if (!h) {
    printf("\"%s\":{\"exists\":false,\"err\":%lu}", label, err);
    return;
  }
  std::string owner, sddl = object_sddl(h, &owner);
  printf("\"%s\":{\"exists\":true,\"owner\":\"%s\",\"sddl\":\"%s\"}", label, owner.c_str(), sddl.c_str());
}

int inspect() {
  Names n = names();
  printf("{\"guid\":\"%s\",\"owner_sid\":\"%s\",\"me\":\"%s\",", narrow(n.guid).c_str(), narrow(n.owner).c_str(), narrow(my_sid()).c_str());
  if (!n.ok) {
    printf("\"ok\":false}\n");
    return 1;
  }
  HANDLE m = OpenFileMappingW(FILE_MAP_READ | READ_CONTROL, FALSE, n.shm.c_str());
  DWORD em = GetLastError();
  HANDLE f = OpenEventW(SYNCHRONIZE | READ_CONTROL, FALSE, n.frame.c_str());
  DWORD ef = GetLastError();
  HANDLE d = OpenEventW(SYNCHRONIZE | READ_CONTROL, FALSE, n.demand.c_str());
  DWORD ed = GetLastError();
  dump_object("shm", m, em);
  printf(",");
  dump_object("frame", f, ef);
  printf(",");
  dump_object("demand", d, ed);
  if (m) {
    auto* h = (const vc::Header*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(vc::Header));
    if (h) {
      printf(",\"header\":{\"magic\":\"0x%08x\",\"version\":%u,\"header_bytes\":%u,\"slot_count\":%u,\"state_seq\":%u,\"streaming\":%u,\"want\":\"%ux%u@%u\","
             "\"dll_version\":%u,\"source_pid\":%u,\"delivered\":%llu,\"placeholders\":%llu,\"repeats\":%llu,\"rejected\":%llu,\"helper_pid\":%u,"
             "\"mode\":%u,\"latest\":%u,\"beat_age_ms\":%.1f}",
             h->magic, h->version, h->header_bytes, h->slot_count, h->state_seq.load(), h->streaming, h->want_w, h->want_h, h->want_fps, h->dll_version,
             h->source_pid, (unsigned long long)h->delivered.load(), (unsigned long long)h->placeholders.load(),
             (unsigned long long)h->repeats.load(), (unsigned long long)h->rejected.load(), h->helper_pid, h->mode.load(), h->latest.load(),
             h->beat_qpc ? qpc_to_ms(qpc_now() - (int64_t)h->beat_qpc.load()) : -1.0);
      UnmapViewOfFile(h);
    }
  }
  printf(",\"ok\":true}\n");
  for (HANDLE h : {m, f, d})
    if (h) CloseHandle(h);
  return 0;
}

int squat(double seconds) {
  Names n = names();
  if (!n.ok) return 1;
  HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, n.frame.c_str());   // default DACL, owner = this user
  DWORD err = GetLastError();
  std::string owner, sddl = ev ? object_sddl(ev, &owner) : "";
  printf("{\"event\":\"squat\",\"t\":%lld,\"created\":%s,\"existed\":%s,\"err\":%lu,\"owner\":\"%s\"}\n", (long long)unix_us(), ev ? "true" : "false",
         err == ERROR_ALREADY_EXISTS ? "true" : "false", ev ? 0 : err, owner.c_str());
  Sleep((DWORD)(seconds * 1000));
  if (ev) CloseHandle(ev);
  return ev ? 0 : 1;
}

// Loads the DLL here and walks the path MFCreateVirtualCamera's validation takes (class factory -> activate
// -> source -> presentation descriptor -> shutdown). Reports the media types and whether any named object or
// log line appeared (the inert-source rule, §4.6.3).
int loadtest(const std::wstring& dll) {
  Names n = names();
  wchar_t pd[MAX_PATH];
  ExpandEnvironmentStringsW(L"%ProgramData%\\Beamer\\Camera\\source.log", pd, MAX_PATH);
  auto log_size = [&]() -> long long {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    return GetFileAttributesExW(pd, GetFileExInfoStandard, &fa) ? ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow : -1;
  };
  auto exists = [&](bool section, const std::wstring& name) {
    HANDLE h = section ? OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()) : OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (h) CloseHandle(h);
    return h != nullptr;
  };
  bool before_shm = n.ok && exists(true, n.shm), before_frame = n.ok && exists(false, n.frame);
  long long log0 = log_size();
  HMODULE m = LoadLibraryW(dll.c_str());
  if (!m) {
    printf("{\"ok\":false,\"error\":\"LoadLibrary\",\"err\":%lu}\n", GetLastError());
    return 1;
  }
  auto get = (HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**))GetProcAddress(m, "DllGetClassObject");
  CLSID clsid;
  CLSIDFromString(vc::kClsidText, &clsid);
  ComPtr<IClassFactory> cf;
  ComPtr<IMFActivate> act;
  ComPtr<IMFMediaSource> src;
  ComPtr<IMFPresentationDescriptor> pdsc;
  HRESULT hr = get ? get(clsid, IID_PPV_ARGS(&cf)) : E_NOINTERFACE;
  if (SUCCEEDED(hr)) hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&act));
  if (SUCCEEDED(hr)) hr = act->ActivateObject(IID_PPV_ARGS(&src));
  if (SUCCEEDED(hr)) hr = src->CreatePresentationDescriptor(&pdsc);
  std::string types;
  bool exact = SUCCEEDED(hr);
  if (SUCCEEDED(hr)) {
    BOOL sel;
    ComPtr<IMFStreamDescriptor> sd;
    ComPtr<IMFMediaTypeHandler> th;
    DWORD cnt = 0;
    pdsc->GetStreamDescriptorByIndex(0, &sel, &sd);
    sd->GetMediaTypeHandler(&th);
    th->GetMediaTypeCount(&cnt);
    exact = cnt == sizeof(vc::kOffered) / sizeof(vc::kOffered[0]);
    for (DWORD i = 0; i < cnt; i++) {
      ComPtr<IMFMediaType> t;
      th->GetMediaTypeByIndex(i, &t);
      GUID g;
      UINT32 w, h, fn, fd;
      t->GetGUID(MF_MT_SUBTYPE, &g);
      MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
      MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &fn, &fd);
      char b[64];
      snprintf(b, sizeof b, "%s\"%s %ux%u@%u/%u\"", types.empty() ? "" : ",", g == MFVideoFormat_NV12 ? "NV12" : "other", w, h, fn, fd);
      types += b;
      if (i < cnt && (g != MFVideoFormat_NV12 || w != vc::kOffered[i].w || h != vc::kOffered[i].h || fn != vc::kOffered[i].fps * fd)) exact = false;
    }
  }
  bool during_shm = n.ok && exists(true, n.shm), during_frame = n.ok && exists(false, n.frame);
  if (src) src->Shutdown();
  if (act) act->ShutdownObject();
  src.Reset();
  act.Reset();
  cf.Reset();
  long long log1 = log_size();
  printf("{\"ok\":%s,\"hr\":\"0x%08lx\",\"types\":[%s],\"types_exactly_kOffered\":%s,\"objects_before\":%s,\"objects_while_loaded\":%s,"
         "\"log_bytes_before\":%lld,\"log_bytes_after\":%lld}\n",
         SUCCEEDED(hr) ? "true" : "false", (unsigned long)hr, types.c_str(), exact ? "true" : "false", before_shm || before_frame ? "true" : "false",
         during_shm || during_frame ? "true" : "false", log0, log1);
  return SUCCEEDED(hr) ? 0 : 1;
}

double nominal_hz() {
  DWORD mhz = 0, sz = sizeof mhz;
  RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &sz);
  return mhz * 1e6;
}
ULONGLONG proc_times(HANDLE p) {
  FILETIME a, b, k, u;
  GetProcessTimes(p, &a, &b, &k, &u);
  return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}
int cpu_of(DWORD pid, double secs) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) {
    printf("{\"ok\":false,\"err\":%lu}\n", GetLastError());
    return 1;
  }
  ULONG64 c0, c1;
  QueryProcessCycleTime(p, &c0);
  ULONGLONG t0 = proc_times(p);
  int64_t w0 = unix_us();
  Sleep((DWORD)(secs * 1000));
  QueryProcessCycleTime(p, &c1);
  double wall = (unix_us() - w0) / 1e6;
  printf("{\"ok\":true,\"pid\":%lu,\"seconds\":%.1f,\"cpu_pct_cycles\":%.2f,\"cpu_pct_times\":%.2f}\n", pid, wall, (double)(c1 - c0) / nominal_hz() / wall * 100,
         (double)(proc_times(p) - t0) / 1e7 / wall * 100);
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    printf("usage: feeder run|vcam|inspect|squat|loadtest|cpu ... (see the header of feeder.cpp)\n");
    return 2;
  }
  std::wstring cmd = argv[1];
  if (cmd == L"vcam" && argc >= 3) return vcam(argv[2]);
  if (cmd == L"inspect") return inspect();
  if (cmd == L"loadtest" && argc >= 3) return loadtest(argv[2]);
  if (cmd == L"cpu" && argc >= 4) return cpu_of((DWORD)_wtoi(argv[2]), _wtof(argv[3]));
  RunOpts o;
  for (int i = 2; i < argc; i++) {
    std::wstring a = argv[i];
    auto num = [&]() { return i + 1 < argc ? _wtof(argv[++i]) : 0.0; };
    if (a == L"--size" && i + 1 < argc) swscanf_s(argv[++i], L"%ux%u", &o.w, &o.h);
    else if (a == L"--follow") o.follow = true;
    else if (a == L"--always") o.always = true;
    else if (a == L"--seconds") o.seconds = num();
    else if (a == L"--fps") o.fps = num();
    else if (a == L"--mode" && i + 1 < argc) o.waiting = !wcscmp(argv[++i], L"waiting");
    else if (a == L"--waiting-at") o.waiting_at = num();
    else if (a == L"--live-at") o.live_at = num();
    else if (a == L"--beat-stop-at") o.beat_stop_at = num();
    else if (a == L"--switch-every") o.switch_every = num();
  }
  if (cmd == L"squat") return squat(o.seconds);
  if (cmd == L"run") {
    if (o.w > vc::kMaxW || o.h > vc::kMaxH || o.w < vc::kMinW || o.h < vc::kMinH) return 2;
    return run(o);
  }
  return 2;
}
