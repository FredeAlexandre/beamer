// SPDX-License-Identifier: GPL-3.0-only
// Synthetic signals: the E2E signatures A/B of proto/scenarios/sig.py (`Injector`: pilot + uneven
// Hann-windowed chirp markers), sines and noise. Rendered by absolute sample index.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace beamer::audio {

class TestSignal {
 public:
  // spec: "A[:<pilot amp>]" | "B[:<pilot amp>]" (signatures), "sine:<hz>[:<amp>]", "silence", "noise[:<amp>]"
  static bool parse(const std::string& spec, TestSignal* out);
  // Mono samples for absolute indices [s0, s0 + n).
  void render(int64_t s0, int n, float* out);

 private:
  enum class Kind { Silence, Sine, Signature, Noise } kind_ = Kind::Silence;
  double freq_ = 440, amp_ = 0.25;
  // signature
  double pilot_amp_ = 0.02, period_s_ = 1.0, start_s_ = 2.0;
  std::vector<float> tpl_;
  int64_t marker_index(int64_t k) const;
  uint64_t rng_ = 0x9E3779B97F4A7C15ull;
};

}  // namespace beamer::audio
