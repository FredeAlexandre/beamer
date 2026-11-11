// SPDX-License-Identifier: GPL-3.0-only
// beamer-audio for macOS: the Mac backend under the engine's helper main loop (PHASE2 §2.4, §3), with the
// macOS video backend beside it (camera PHASE2 §2.4; --video null:... / --no-video replace it, helper.cpp).
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend_mac.h"
#include "video_backend_mac.h"

int main(int argc, char** argv) {
  bool dev = false, other_video = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--dev")) dev = true;
    if (!std::strcmp(argv[i], "--no-video") || !std::strcmp(argv[i], "--video")) other_video = true;
  }
  auto backend = beamer::audio::make_mac_backend("");
  std::unique_ptr<beamer::audio::VideoBackend> video;
  if (!other_video) video = beamer::audio::make_mac_video_backend(dev ? "dev" : "");
  return beamer::audio::run_helper(*backend, video.get(), argc, argv);
}
