// SPDX-License-Identifier: GPL-3.0-only
#include "util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <timeapi.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winmm.lib")
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/semaphore.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>
#else
#include <pthread.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace beamer::audio {

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void random_bytes(uint8_t* out, size_t n) {
#if defined(_WIN32)
  BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(__APPLE__)
  arc4random_buf(out, n);
#else
  size_t got = 0;
  while (got < n) {
    ssize_t r = getrandom(out + got, n - got, 0);
    if (r > 0) got += static_cast<size_t>(r);
  }
#endif
}

uint32_t random_u32() {
  uint8_t b[4];
  random_bytes(b, 4);
  return get_u32(b);
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const uint8_t* data, size_t n) {
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i < n) {
    uint32_t v = uint32_t(data[i]) << 16;
    if (i + 1 < n) v |= uint32_t(data[i + 1]) << 8;
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

static int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

bool b64_decode(const std::string& text, std::vector<uint8_t>* out) {
  out->clear();
  size_t n = text.size();
  while (n > 0 && text[n - 1] == '=') --n;
  if (text.size() - n > 2) return false;
  if (n % 4 == 1) return false;
  out->reserve(n * 3 / 4);
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; ++i) {
    int v = b64_val(text[i]);
    if (v < 0) return false;
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(uint8_t((acc >> bits) & 0xFF));
    }
  }
  return true;
}

std::string hex_encode(const uint8_t* data, size_t n) {
  static const char h[] = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s += h[data[i] >> 4];
    s += h[data[i] & 15];
  }
  return s;
}

bool hex_decode(const std::string& text, std::vector<uint8_t>* out) {
  out->clear();
  if (text.size() % 2) return false;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < text.size(); i += 2) {
    int a = val(text[i]), b = val(text[i + 1]);
    if (a < 0 || b < 0) return false;
    out->push_back(uint8_t(a * 16 + b));
  }
  return true;
}

static std::atomic<int> g_log_level{static_cast<int>(LogLevel::Info)};
static std::mutex g_log_mutex;

void set_log_level(LogLevel level) { g_log_level.store(static_cast<int>(level)); }

bool parse_log_level(const std::string& text, LogLevel* out) {
  if (text == "debug") *out = LogLevel::Debug;
  else if (text == "info") *out = LogLevel::Info;
  else if (text == "warn") *out = LogLevel::Warn;
  else return false;
  return true;
}

void log_msg(LogLevel level, const char* fmt, ...) {
  if (static_cast<int>(level) < g_log_level.load()) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  for (char* p = buf; *p; ++p)
    if (*p == '\n' || *p == '\r') *p = ' ';
  const char* name = level == LogLevel::Debug ? "debug" : level == LogLevel::Info ? "info" : "warn";
  std::lock_guard<std::mutex> lock(g_log_mutex);
  fprintf(stderr, "%s: %s\n", name, buf);
  fflush(stderr);
}

void set_thread_name(const char* name) {
#if defined(_WIN32)
  wchar_t w[64];
  size_t i = 0;
  for (; name[i] && i < 63; ++i) w[i] = static_cast<wchar_t>(name[i]);
  w[i] = 0;
  SetThreadDescription(GetCurrentThread(), w);
#elif defined(__APPLE__)
  pthread_setname_np(name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}

void set_thread_interactive() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#elif defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void sleep_ns(int64_t ns) {
  if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

double process_cpu_s() {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  auto conv = [](FILETIME f) {
    return static_cast<double>((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7;
  };
  return conv(k) + conv(u);
#else
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
#endif
}

int64_t current_pid() {
#if defined(_WIN32)
  return static_cast<int64_t>(GetCurrentProcessId());
#else
  return static_cast<int64_t>(getpid());
#endif
}

void enable_fine_timers() {
#if defined(_WIN32)
  static std::once_flag once;
  std::call_once(once, [] { timeBeginPeriod(1); });
#endif
}

// ---------------------------------------------------------------- Signal

#if defined(_WIN32)
Signal::Signal() { impl_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
Signal::~Signal() { CloseHandle(static_cast<HANDLE>(impl_)); }
void Signal::signal() { SetEvent(static_cast<HANDLE>(impl_)); }
bool Signal::wait_ms(int ms) {
  return WaitForSingleObject(static_cast<HANDLE>(impl_), static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
}
#elif defined(__APPLE__)
Signal::Signal() {
  semaphore_t s = 0;
  semaphore_create(mach_task_self(), &s, SYNC_POLICY_FIFO, 0);
  impl_ = reinterpret_cast<void*>(static_cast<uintptr_t>(s));
}
Signal::~Signal() {
  semaphore_destroy(mach_task_self(), static_cast<semaphore_t>(reinterpret_cast<uintptr_t>(impl_)));
}
void Signal::signal() { semaphore_signal(static_cast<semaphore_t>(reinterpret_cast<uintptr_t>(impl_))); }
bool Signal::wait_ms(int ms) {
  mach_timespec_t ts;
  ts.tv_sec = static_cast<unsigned>(ms / 1000);
  ts.tv_nsec = (ms % 1000) * 1000000;
  return semaphore_timedwait(static_cast<semaphore_t>(reinterpret_cast<uintptr_t>(impl_)), ts) ==
         KERN_SUCCESS;
}
#else
namespace {
struct CvSignal {
  std::mutex m;
  std::condition_variable cv;
  int count = 0;
};
}  // namespace
Signal::Signal() { impl_ = new CvSignal(); }
Signal::~Signal() { delete static_cast<CvSignal*>(impl_); }
void Signal::signal() {
  auto* s = static_cast<CvSignal*>(impl_);
  {
    std::lock_guard<std::mutex> lock(s->m);
    s->count++;
  }
  s->cv.notify_one();
}
bool Signal::wait_ms(int ms) {
  auto* s = static_cast<CvSignal*>(impl_);
  std::unique_lock<std::mutex> lock(s->m);
  if (!s->cv.wait_for(lock, std::chrono::milliseconds(ms), [s] { return s->count > 0; })) return false;
  s->count--;
  return true;
}
#endif

}  // namespace beamer::audio
