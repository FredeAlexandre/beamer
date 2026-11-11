// SPDX-License-Identifier: GPL-3.0-only
// Null video backend (camera PHASE2 §10.3): a fake encoder and a reference-checking fake decoder, so two
// helpers exercise the whole video protocol on one machine. Options ("k=v;k=v"):
//   cams=2  perm=granted|denied|undetermined|restricted  vcam=ok|missing|outdated|broken|unsupported
//   ip=120000 p=10000 refresh=14000  (AU bytes at 2.5 Mbit/s; scaled with the bitrate; P lognormal, CV 0.34)
//   enc_ms=5 dec_ms=2  native=1920x1440  cam_start_ms=5 (start_encoding() -> first AU)
//   dec_err_ms=0 (inject a decoder error every N ms of decoding; tests)
// Fake AUs carry their frame number (predicted from on_frame's numbering and checked against its return), their
// reference (previous frame, newest acknowledged LTR for a refresh, none for an IDR), the display and coded size
// (1080p is coded 1920x1088 with an 8-row crop, as VideoToolbox does) and a byte pattern. The fake decoder marks
// a picture correct iff its bytes are intact and its reference was decoded correct (an infinite-GOP H.264
// decoder), decodes "ok" silently even when the picture is wrong (as real decoders do), and counts every
// keep=true picture that is not correct and that the engine did not revoke with drop() (`incorrect_kept`, must stay
// 0; it includes any such picture presented). Decoding is asynchronous, so a picture handed with keep=true before an
// earlier frame's decoder error is known is legitimately revoked afterwards (§4.5.2: on_decoded(ok=false) revokes
// every later kept picture): those are counted apart (`kept_wrong_revoked`). It rejects DISPLAY sizes that are odd or
// outside 64x64..1920x1080 (`size_rejected`), never the coded size.
#pragma once

#include <string>

#include "json.h"
#include "video_backend.h"

namespace beamer::audio {

class NullVideoBackend;
NullVideoBackend* as_null_video(VideoBackend* b);  // nullptr for real backends
// Counters and records of both sides (encoder handles and the sink), for tests and `dev null_video report`.
Json null_video_report(NullVideoBackend* nb);
// Dev controls: {"perm":..., "vcam_in_use":bool, "want":{"w","h","fps"}, "install":..., "loaded_version":n,
// "dec_err_ms":n, "decoder_errors":n (fail the next n decodes), "reset":true (zero the counters),
// "mark":true (report.mark.kept_after_ms = time to the next correct kept picture)}.
bool null_video_set(NullVideoBackend* nb, const Json& j, std::string* err);

}  // namespace beamer::audio
