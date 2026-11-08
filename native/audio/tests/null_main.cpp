// SPDX-License-Identifier: GPL-3.0-only
// beamer-audio-null: the real helper with the null device backend (§11.3). Options come from
// `--backend null:<options>` (handled by run_helper); without it the null defaults apply.
#include "engine.h"

int main(int argc, char** argv) {
  auto backend = beamer::audio::make_null_backend("");
  return beamer::audio::run_helper(*backend, argc, argv);
}
