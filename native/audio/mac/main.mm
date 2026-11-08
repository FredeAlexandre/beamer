// SPDX-License-Identifier: GPL-3.0-only
// beamer-audio for macOS: the Mac backend under the engine's helper main loop (PHASE2 §2.4, §3).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend_mac.h"

int main(int argc, char** argv) {
  auto backend = beamer::audio::make_mac_backend("");
  return beamer::audio::run_helper(*backend, argc, argv);
}
