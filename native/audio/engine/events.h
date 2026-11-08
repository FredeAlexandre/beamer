// SPDX-License-Identifier: GPL-3.0-only
// Event output of the control protocol (§3.3): one JSON object per line, one writer under a mutex.
#pragma once

#include <functional>
#include <mutex>
#include <string>

#include "json.h"

namespace beamer::audio {

class EventSink {
 public:
  explicit EventSink(std::function<void(const std::string&)> write_line) : write_(std::move(write_line)) {}
  // Thread-safe. Never call from an IO thread.
  void emit(const Json& ev) {
    std::string line = ev.dump(-1, ' ', false, Json::error_handler_t::replace);
    std::lock_guard<std::mutex> lock(mu_);
    write_(line);
  }

 private:
  std::mutex mu_;
  std::function<void(const std::string&)> write_;
};

}  // namespace beamer::audio
