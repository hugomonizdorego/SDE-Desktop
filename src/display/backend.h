// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <sys/types.h>

#include "profile.h"
namespace sde {
class Config;
std::vector<Output> parse_xrandr(const std::string &text);
std::vector<Output> query_outputs(bool detect = false);
std::string display_signature(const std::vector<Output> &outputs);
std::string display_topology(const std::vector<Output> &outputs);
std::string edid_name(const std::vector<unsigned char> &edid);
int output_width(const Output &o);
int output_height(const Output &o);
std::string validate_display_layout(const std::vector<Output> &wanted,
                                    const std::vector<Output> &hardware);
std::vector<std::string> xrandr_args(const std::vector<Output> &outputs);
bool run_display_layout(const std::vector<Output> &outputs);
void save_display_layout(Config &cfg, const std::vector<Output> &outputs);
bool apply_display_layout(const Config &cfg);
bool import_legacy_display_layout(const std::string &text, const std::vector<Output> &hardware,
                                  std::vector<Output> &outputs);
std::vector<std::string> display_profiles();
bool save_display_profile(const DisplayProfile &profile);
bool load_display_profile(const std::string &name, DisplayProfile &profile,
                          const std::vector<Output> &hardware);
bool delete_display_profile(const std::string &name);
// A separate worker restores on deadline or parent death, even if the UI stalls.
class DisplayPreview {
 public:
  explicit DisplayPreview(const std::vector<Output> &previous, int seconds = 25);
  ~DisplayPreview();
  bool ready() const { return pid_ > 0; }
  bool keep();
  void revert();

 private:
  int fd_ = -1;
  pid_t pid_ = -1;
  bool finish(char command);
};
}  // namespace sde
