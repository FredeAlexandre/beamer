// SPDX-License-Identifier: GPL-3.0-only
// Umbrella header for backends: the standard headers backend.h relies on, then the contract itself.
// backend.h is the verbatim PHASE2 §2.4 block and has no includes of its own, so include this file
// (or these headers first) instead of backend.h directly.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "backend.h"

namespace beamer::audio {

constexpr const char* kEngineVersion = "0.1.0";
constexpr int kControlProto = 1;

#if defined(_WIN32)
constexpr const char* kPlatform = "windows";
#elif defined(__APPLE__)
constexpr const char* kPlatform = "macos";
#else
constexpr const char* kPlatform = "linux";
#endif

}  // namespace beamer::audio
