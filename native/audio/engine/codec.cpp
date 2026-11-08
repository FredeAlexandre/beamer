// SPDX-License-Identifier: GPL-3.0-only
#include "codec.h"

#include "opus.h"

namespace beamer::audio {

CodecConfig CodecConfig::mic_default() { return CodecConfig(); }

CodecConfig CodecConfig::system_default() {
  CodecConfig c;
  c.app = "audio";
  c.channels = 2;
  c.bitrate = 96000;
  c.fec = false;
  return c;
}

bool CodecConfig::parse(const Json& j, CodecConfig* out, std::string* err) {
  if (!j.is_object()) {
    *err = "codec must be an object";
    return false;
  }
  CodecConfig c;
  std::string s;
  if (jget_str(j, "name", &s)) c.name = s;
  if (c.name != "opus") {
    *err = "unsupported codec " + c.name;
    return false;
  }
  if (jget_str(j, "app", &s)) c.app = s;
  if (c.app != "voip" && c.app != "audio" && c.app != "lowdelay") {
    *err = "bad codec.app";
    return false;
  }
  int64_t v;
  if (jget_int(j, "channels", &v)) c.channels = static_cast<int>(v);
  if (c.channels != 1 && c.channels != 2) {
    *err = "codec.channels must be 1 or 2";
    return false;
  }
  if (jget_int(j, "bitrate", &v)) c.bitrate = static_cast<int>(v);
  if (c.bitrate < 6000 || c.bitrate > 510000) {
    *err = "bad codec.bitrate";
    return false;
  }
  double f;
  if (jget_num(j, "frame_ms", &f)) c.frame_ms = f;
  if (c.frame_ms != 2.5 && c.frame_ms != 5 && c.frame_ms != 10 && c.frame_ms != 20) {
    *err = "codec.frame_ms must be 2.5, 5, 10 or 20";
    return false;
  }
  bool b;
  if (jget_bool(j, "fec", &b)) c.fec = b;
  if (j.contains("complexity") && !j["complexity"].is_null()) {
    if (!jget_int(j, "complexity", &v) || v < 0 || v > 10) {
      *err = "codec.complexity must be 0..10";
      return false;
    }
    c.complexity = static_cast<int>(v);
  }
  *out = c;
  return true;
}

Json CodecConfig::to_json() const {
  Json j{{"name", name}, {"app", app}, {"channels", channels},
         {"bitrate", bitrate}, {"frame_ms", frame_ms}, {"fec", fec}};
  if (complexity >= 0) j["complexity"] = complexity;
  return j;
}

OpusEnc::~OpusEnc() {
  if (enc_) opus_encoder_destroy(enc_);
}

bool OpusEnc::init(const CodecConfig& cfg, std::string* err) {
  int app = cfg.app == "voip"    ? OPUS_APPLICATION_VOIP
            : cfg.app == "audio" ? OPUS_APPLICATION_AUDIO
                                 : OPUS_APPLICATION_RESTRICTED_LOWDELAY;
  int e = 0;
  enc_ = opus_encoder_create(48000, cfg.channels, app, &e);
  if (!enc_ || e != OPUS_OK) {
    *err = std::string("opus_encoder_create: ") + opus_strerror(e);
    enc_ = nullptr;
    return false;
  }
  frame_ = cfg.frame();
  bitrate_ = cfg.bitrate;
  opus_encoder_ctl(enc_, OPUS_SET_BITRATE(cfg.bitrate));
  opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(cfg.effective_complexity()));
  opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(cfg.fec ? 1 : 0));
  opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(cfg.fec ? 10 : 0));
  opus_encoder_ctl(enc_, OPUS_SET_DTX(0));
  return true;
}

int OpusEnc::encode(const float* pcm, uint8_t* out, int max_bytes) {
  return opus_encode_float(enc_, pcm, frame_, out, max_bytes);
}

void OpusEnc::set_bitrate(int bps) {
  if (bps == bitrate_ || !enc_) return;
  bitrate_ = bps;
  opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bps));
}

OpusDec::~OpusDec() {
  if (dec_) opus_decoder_destroy(dec_);
}

bool OpusDec::init(int channels, std::string* err) {
  int e = 0;
  dec_ = opus_decoder_create(48000, channels, &e);
  if (!dec_ || e != OPUS_OK) {
    *err = std::string("opus_decoder_create: ") + opus_strerror(e);
    dec_ = nullptr;
    return false;
  }
  channels_ = channels;
  return true;
}

int OpusDec::decode(const uint8_t* data, int len, float* out, int max_frames) {
  return opus_decode_float(dec_, data, len, out, max_frames, 0);
}

int OpusDec::decode_fec(const uint8_t* next, int len, float* out, int frames) {
  return opus_decode_float(dec_, next, len, out, frames, 1);
}

int OpusDec::plc(float* out, int frames) { return opus_decode_float(dec_, nullptr, 0, out, frames, 0); }

bool opus_has_lbrr(const uint8_t* data, int len) { return opus_packet_has_lbrr(data, len) > 0; }

const char* opus_version() { return opus_get_version_string(); }

}  // namespace beamer::audio
