// SPDX-License-Identifier: GPL-3.0-only
// Small portable helpers: clocks, randomness, base64/hex, big-endian codecs, logging, threads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace beamer::audio {

// Monotonic clock (steady_clock: mach continuous/uptime on macOS, QPC on Windows), nanoseconds.
int64_t now_ns();
inline double now_s() { return static_cast<double>(now_ns()) * 1e-9; }
// Wall clock, milliseconds since the Unix epoch (for `t` fields).
int64_t wall_ms();

void random_bytes(uint8_t* out, size_t n);
uint32_t random_u32();

std::string b64_encode(const uint8_t* data, size_t n);
inline std::string b64_encode(const std::vector<uint8_t>& v) { return b64_encode(v.data(), v.size()); }
bool b64_decode(const std::string& text, std::vector<uint8_t>* out);  // strict (padding optional)
std::string hex_encode(const uint8_t* data, size_t n);
bool hex_decode(const std::string& text, std::vector<uint8_t>* out);

inline void put_u16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put_u32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline void put_u64(uint8_t* p, uint64_t v) { put_u32(p, uint32_t(v >> 32)); put_u32(p + 4, uint32_t(v)); }
inline uint16_t get_u16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t get_u32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t get_u64(const uint8_t* p) { return (uint64_t(get_u32(p)) << 32) | get_u32(p + 4); }

enum class LogLevel { Debug = 0, Info = 1, Warn = 2 };
void set_log_level(LogLevel level);
bool parse_log_level(const std::string& text, LogLevel* out);
// stderr, one line "level: message". Never call from an IO thread.
void log_msg(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#define BA_DEBUG(...) ::beamer::audio::log_msg(::beamer::audio::LogLevel::Debug, __VA_ARGS__)
#define BA_INFO(...) ::beamer::audio::log_msg(::beamer::audio::LogLevel::Info, __VA_ARGS__)
#define BA_WARN(...) ::beamer::audio::log_msg(::beamer::audio::LogLevel::Warn, __VA_ARGS__)

void set_thread_name(const char* name);
// Latency-sensitive engine threads (network, encoders): macOS QoS user-interactive (performance
// cores), Windows THREAD_PRIORITY_ABOVE_NORMAL. Backend IO threads use their OS's real-time class.
void set_thread_interactive();
void sleep_ns(int64_t ns);
// Process CPU time (user + system) in seconds, for self-measurement.
double process_cpu_s();
int64_t current_pid();
// Raise the system timer resolution where needed (Windows: 1 ms). Idempotent.
void enable_fine_timers();

// Wakes one waiting thread; signal() is safe from a real-time IO thread (no locks, no allocation:
// mach semaphore on macOS, an auto-reset event on Windows).
class Signal {
 public:
  Signal();
  ~Signal();
  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;
  void signal();
  // Returns true if signalled, false on timeout.
  bool wait_ms(int ms);

 private:
  void* impl_;
};

}  // namespace beamer::audio
