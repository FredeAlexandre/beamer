// SPDX-License-Identifier: GPL-3.0-only
// The helper's control loop (§3): JSON-lines commands in, events out. run_helper() (backend.h)
// parses the command line and calls run_control() with stdin/stdout; tests call it in-process.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine.h"
#include "flow.h"
#include "video_backend.h"

namespace beamer::audio {

struct HelperOptions {
  bool dev = false;
  int64_t parent_pid = -1;
  std::string state_dir;
  std::vector<int64_t> exclude_pids;
  FlowTiming timing;
  int stats_period_ms = 1000;
  VideoBackend* video = nullptr;  // camera PHASE2: null = no video (the helper behaves exactly as before)
};

struct HelperIo {
  std::function<bool(std::string*)> read_line;     // blocking; false on EOF
  std::function<void(const std::string&)> write_line;  // one event line, without "\n"
  bool detach_reader = false;  // stdin: the reader thread may stay blocked at exit
};

int run_control(Backend& backend, const HelperOptions& opt, const HelperIo& io);

}  // namespace beamer::audio
