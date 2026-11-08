// SPDX-License-Identifier: GPL-3.0-only
// Opus encoder/decoder wrappers and the `codec` object of §4.4.
#pragma once

#include <cstdint>
#include <string>

#include "json.h"

struct OpusEncoder;
struct OpusDecoder;

namespace beamer::audio {

struct CodecConfig {
  std::string name = "opus";
  std::string app = "voip";  // voip | audio | lowdelay
  int channels = 1;
  int bitrate = 32000;
  double frame_ms = 10;
  bool fec = true;

  int frame() const { return static_cast<int>(frame_ms * 48 + 0.5); }  // samples per channel
  static bool parse(const Json& j, CodecConfig* out, std::string* err);
  Json to_json() const;
  static CodecConfig mic_default();
  static CodecConfig system_default();
};

class OpusEnc {
 public:
  OpusEnc() = default;
  ~OpusEnc();
  OpusEnc(const OpusEnc&) = delete;
  OpusEnc& operator=(const OpusEnc&) = delete;
  // Mic: complexity 9, PACKET_LOSS_PERC 10 when fec, DTX off (§4.4).
  bool init(const CodecConfig& cfg, std::string* err);
  // pcm: frame() interleaved samples. Returns bytes written or < 0 on error.
  int encode(const float* pcm, uint8_t* out, int max_bytes);
  void set_bitrate(int bps);
  int bitrate() const { return bitrate_; }

 private:
  OpusEncoder* enc_ = nullptr;
  int frame_ = 480;
  int bitrate_ = 0;
};

class OpusDec {
 public:
  OpusDec() = default;
  ~OpusDec();
  OpusDec(const OpusDec&) = delete;
  OpusDec& operator=(const OpusDec&) = delete;
  bool init(int channels, std::string* err);
  // Returns samples per channel decoded, or < 0 on error. Real-time safe (no allocation).
  int decode(const uint8_t* data, int len, float* out, int max_frames);
  int decode_fec(const uint8_t* next, int len, float* out, int frames);  // LBRR of the next packet
  int plc(float* out, int frames);
  int channels() const { return channels_; }

 private:
  OpusDecoder* dec_ = nullptr;
  int channels_ = 1;
};

bool opus_has_lbrr(const uint8_t* data, int len);
const char* opus_version();

}  // namespace beamer::audio
