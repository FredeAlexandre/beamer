// SPDX-License-Identifier: GPL-3.0-only
// The macOS device backend (PHASE2 §2.4): Core Audio HAL IOProcs, process taps, the Beamer virtual
// microphone. backend.h has neither include guard nor includes of its own: include it only through
// this header.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../engine/backend.h"

namespace beamer::audio {

// `options` is reserved (pass ""). Environment, for tuning and tests only:
//   BEAMER_AUDIO_MAC_BUFFER_FRAMES  IO buffer size asked for on every device we open (default 480,
//                                   10 ms at 48 kHz; the HAL reports what it granted in the log).
std::unique_ptr<Backend> make_mac_backend(const std::string& options);

}  // namespace beamer::audio
