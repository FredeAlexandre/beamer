// SPDX-License-Identifier: GPL-3.0-only
// nlohmann/json 3.11.3 (vendored, MIT) with exceptions kept inside the control plane.
#pragma once

#include "nlohmann/json.hpp"

namespace beamer::audio {
using Json = nlohmann::json;

// Typed field access that never throws: false when missing or of the wrong type.
inline bool jget_bool(const Json& j, const char* k, bool* out) {
  auto it = j.find(k);
  if (it == j.end() || !it->is_boolean()) return false;
  *out = it->get<bool>();
  return true;
}
inline bool jget_int(const Json& j, const char* k, int64_t* out) {
  auto it = j.find(k);
  if (it == j.end() || !it->is_number_integer()) return false;
  *out = it->get<int64_t>();
  return true;
}
inline bool jget_num(const Json& j, const char* k, double* out) {
  auto it = j.find(k);
  if (it == j.end() || !it->is_number()) return false;
  *out = it->get<double>();
  return true;
}
inline bool jget_str(const Json& j, const char* k, std::string* out) {
  auto it = j.find(k);
  if (it == j.end() || !it->is_string()) return false;
  *out = it->get<std::string>();
  return true;
}
}  // namespace beamer::audio
