// SPDX-License-Identifier: GPL-3.0-only
// Null device backend (§11.3): virtual devices on threads with a given clock error. Options:
//   "<group>=<signal|record:path|null>[,ppm=<+-ppm>][,period=<frames>][,in_use=0|1][,kind=..];..."
// groups: mic (input "null-mic"), system (system-sound capture), out (output "null-out"),
// vmic (virtual mic sink "null-vmic"), ref (the AEC reference capture of null-out: what it "plays",
// default silence); a bare "ppm=<v>" or "period=<n>" applies to every group.
// Signals: sine:<hz>[:<amp>], A, B (E2E signatures), noise[:amp], silence.
// Example: "mic=sine:440,ppm=+80;out=record:/tmp/out.wav".
#pragma once

#include <string>

#include "engine.h"

namespace beamer::audio {

class NullBackend;
// The engine's dev command reaches the null backend through this (nullptr for real backends).
NullBackend* as_null_backend(Backend* b);
// Dev controls: vmic_in_use, remove/add a device ("null-mic", "null-out", "null-vmic").
void null_backend_set_vmic_in_use(NullBackend* nb, bool in_use);
bool null_backend_set_present(NullBackend* nb, const std::string& key, bool present);

}  // namespace beamer::audio
