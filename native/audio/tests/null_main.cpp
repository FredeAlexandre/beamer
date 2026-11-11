// SPDX-License-Identifier: GPL-3.0-only
// beamer-audio-null: the real helper with the null device backend (§11.3) and the null video backend (camera
// PHASE2 §10.3). Options come from `--backend null:<options>` / `--video null:<options>` (handled by
// run_helper); without them the null defaults apply. `--no-video`: no video backend.
#include "engine.h"
#include "video_backend.h"

int main(int argc, char** argv) {
  auto backend = beamer::audio::make_null_backend("");
  auto video = beamer::audio::make_null_video_backend("");
  return beamer::audio::run_helper(*backend, video.get(), argc, argv);
}
