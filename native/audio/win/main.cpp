// SPDX-License-Identifier: GPL-3.0-only
// beamer-audio.exe: the Windows audio helper (PHASE2 §3). The engine's run_helper() is the control loop; this file
// only builds the WASAPI backend. Beyond §2.4's "three lines" it (1) runs the mute guard when started as one,
// (2) hands run_helper a UTF-8 argv (main's argv is in the ANSI code page), (3) puts stdio in binary mode so
// JSON lines end in "\n", not "\r\n", (4) keeps backend-only options (--vmic-exclusive) away from the engine.
#include "backend_win.h"

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <cstdio>

int main(int argc, char** argv) {
    int rc = 0;
    if (beamer::audio::run_guard_if_requested(argc, argv, &rc)) return rc;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::vector<std::string> args;
    int wn = 0;
    if (LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &wn)) {
        for (int i = 0; i < wn; i++) {
            std::string a = beamer::audio::win::utf8(w[i]);
            if (a == "--vmic-exclusive") continue;  // backend-only option (WinOptions), unknown to the engine
            args.push_back(a);
        }
        LocalFree(w);
    }
    std::vector<char*> uargv;
    for (auto& a : args) uargv.push_back(a.data());
    uargv.push_back(nullptr);
    auto backend = beamer::audio::make_win_backend(beamer::audio::win_options_from_args(argc, argv));
    return beamer::audio::run_helper(*backend, (int)args.size(), uargv.data());
}
