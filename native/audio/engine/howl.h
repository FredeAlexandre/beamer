// SPDX-License-Identifier: GPL-3.0-only
// Howl guard for system-sound send streams (PHASE2 §3.3 `howl`).
//
// Acoustic feedback through two machines: M's sound plays on T's speakers, a microphone on M hears
// them, and something on M (Elgato Wave Link's monitor, Windows "Listen to this device") plays that
// microphone into M's own sound, which Beamer captures and sends to T again. Beamer cannot leave that
// process out of the capture (Windows excludes one process tree only: Beamer's own), so the guard
// watches what is about to be sent and ducks it when feedback builds up.
//
// What feedback looks like, and what music or a voice does not: one narrow spectral line from 800 Hz
// up (most of the band's energy within +-2 bins of a 1024-point FFT, no partials at f/2, 2f or 3f, no
// other line within 15 dB) whose level keeps rising over a few hundred ms to a few seconds at a
// stable frequency. A held note at a constant level, a chord, a sweep or an attack (a step, then flat) are not. On
// detection the stream is ducked by 20 dB (a loop gain above 20 dB is not plausible between two
// machines), held for 2 s (doubled for each repeat within a minute, up to 30 s), then released over
// 1 s once the tone has decayed. A tone that does not decay at all while ducked is the source's own
// sound (the duck broke no loop): the duck is released after 4 s and that frequency is ignored for
// 30 s. A pure tone that fades in slowly is the one sound that looks like feedback; it gets that
// 4 s duck once.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace beamer::audio {

struct HowlConfig {
  double duck_db = -20;       // first duck
  double trigger_dbfs = -45;  // a tone quieter than this (RMS, dBFS) never triggers
  double rise_db = 6;         // least rise over a detection window
  double dominance = 0.4;     // tone power / band power (150 Hz..16 kHz) over the window's last quarter
  double hold_s = 2;          // least time a duck holds; doubled per repeat within a minute
  double max_hold_s = 30;
  double release_s = 1;       // ramp back to 0 dB
  double false_alarm_s = 4;   // a ducked tone that has not decayed by then is the source's own sound
  double ignore_s = 30;       // ...and its frequency is ignored this long
};

struct HowlReport {
  bool active = false;   // ducking or releasing
  double freq_hz = 0;    // the tone's frequency (last detection)
  double duck_db = 0;    // the duck in force (0 when inactive)
  uint32_t episodes = 0;
};

class HowlGuard {
 public:
  static constexpr int kFft = 1024;
  static constexpr int kHop = 512;

  explicit HowlGuard(int channels, HowlConfig cfg = HowlConfig());
  // Encoder thread: analyse `frames` interleaved frames and apply the duck in place. Leaves the
  // samples untouched (bit for bit) while no duck is in force.
  void process(float* x, int frames);
  // Control thread: the current state; true when it differs from the previous poll's.
  bool poll(HowlReport* out);

  // Any thread.
  bool active() const { return active_.load(std::memory_order_relaxed); }
  double freq_hz() const { return freq_.load(std::memory_order_relaxed); }
  uint32_t episodes() const { return episodes_.load(std::memory_order_relaxed); }
  uint32_t false_alarms() const { return false_alarms_.load(std::memory_order_relaxed); }
  // Encoder thread (tests): the gain applied to the last sample, dB.
  double gain_db() const { return gain_db_; }

 private:
  enum class State { Idle, Ducked, Releasing };
  struct Frame {
    float level_db;   // tracked tone, RMS dBFS
    float dominance;  // tracked tone / band
    bool lines;       // other spectral lines: partials, a chord, music under the tone
  };

  void analyse();
  bool other_lines(int bin, double freq_hz, double tone_power) const;
  bool rising() const;
  bool ignored(int bin) const;
  void enter_duck(double db);
  double now_s() const { return double(samples_) / 48000.0; }

  int channels_;
  HowlConfig cfg_;
  // analysis (encoder thread)
  std::vector<float> buf_, win_, re_, im_, cos_, sin_, power_;
  std::vector<int> rev_;
  int fill_ = 0;
  int64_t samples_ = 0;
  int track_bin_ = -1, misses_ = 0;
  double track_freq_ = 0;
  std::vector<Frame> hist_;  // ring of the tracked tone, newest at hist_pos_-1
  int hist_pos_ = 0, hist_len_ = 0;
  // duck state (encoder thread)
  State state_ = State::Idle;
  double gain_db_ = 0, target_db_ = 0, step_down_, step_up_ = 0;
  double duck_at_ = 0, hold_ = 0, trigger_db_ = 0, decayed_at_ = -1;
  bool decayed_ever_ = false;
  std::vector<double> recent_;              // trigger times within the last minute
  std::vector<std::pair<int, double>> ignore_;  // (bin, until)
  // published
  std::atomic<bool> active_{false};
  std::atomic<double> freq_{0}, duck_{0};
  std::atomic<uint32_t> episodes_{0}, false_alarms_{0};
  // control thread
  HowlReport reported_;
};

}  // namespace beamer::audio
