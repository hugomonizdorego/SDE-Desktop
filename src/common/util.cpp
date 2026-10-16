// SPDX-License-Identifier: GPL-3.0-or-later
#include "util.h"

#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sde {

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

std::vector<std::string> split(const std::string &s, char sep, bool keep_empty) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      if (keep_empty || !cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (keep_empty || !cur.empty()) out.push_back(cur);
  return out;
}

bool starts_with(const std::string &s, const std::string &p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string &s, const std::string &p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string join_path(const std::string &a, const std::string &b) {
  if (a.empty()) return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string basename_of(const std::string &path) {
  size_t p = path.find_last_of('/');
  return p == std::string::npos ? path : path.substr(p + 1);
}

std::string env(const char *name, const std::string &fallback) {
  const char *v = getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

std::string home_dir() {
  std::string h = env("HOME");
  if (!h.empty()) return h;
  struct passwd *pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "/";
}

std::string user_name() {
  struct passwd *pw = getpwuid(getuid());
  if (!pw) return env("USER", "user");
  std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
  std::string full = trim(split(gecos, ',', true).empty() ? "" : split(gecos, ',', true)[0]);
  return full.empty() ? std::string(pw->pw_name) : full;
}

std::string config_home() { return env("XDG_CONFIG_HOME", home_dir() + "/.config"); }
std::string data_home() { return env("XDG_DATA_HOME", home_dir() + "/.local/share"); }
std::string state_home() { return env("XDG_STATE_HOME", home_dir() + "/.local/state"); }

std::string runtime_dir() {
  std::string r = env("XDG_RUNTIME_DIR");
  if (!r.empty()) return r;
  r = "/tmp/sde-" + std::to_string(getuid());
  mkdir(r.c_str(), 0700);
  return r;
}

std::vector<std::string> data_dirs() {
  std::vector<std::string> out{data_home()};
  for (auto &d : split(env("XDG_DATA_DIRS", "/usr/local/share:/usr/share"), ':'))
    if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
  return out;
}

std::vector<std::string> config_dirs() { return split(env("XDG_CONFIG_DIRS", "/etc/xdg"), ':'); }

std::string user_dir(const std::string &name) {
  std::string content;
  std::string home = home_dir();
  if (read_file(config_home() + "/user-dirs.dirs", content)) {
    std::istringstream in(content);
    std::string line, key = "XDG_" + name + "_DIR=";
    while (std::getline(in, line)) {
      line = trim(line);
      if (!starts_with(line, key)) continue;
      std::string v = line.substr(key.size());
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      if (starts_with(v, "$HOME")) v = home + v.substr(5);
      return v;
    }
  }
  if (name == "DESKTOP") return home + "/Desktop";
  return home;
}

bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dir_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_executable(const std::string &path) {
  return file_exists(path) && access(path.c_str(), X_OK) == 0;
}

bool read_file(const std::string &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool write_file_atomic(const std::string &path, const std::string &content) {
  size_t slash = path.find_last_of('/');
  if (slash != std::string::npos) make_dirs(path.substr(0, slash));
  std::string tmp = path + ".tmp." + std::to_string(getpid());
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
  ok = (fflush(f) == 0) && ok;
  ok = (fsync(fileno(f)) == 0) && ok;
  fclose(f);
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

bool make_dirs(const std::string &path) {
  if (path.empty()) return false;
  std::string cur;
  for (auto &part : split(path, '/')) {
    cur += "/" + part;
    if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  return dir_exists(path);
}

std::vector<std::string> list_dir(const std::string &path) {
  std::vector<std::string> out;
  DIR *d = opendir(path.c_str());
  if (!d) return out;
  while (struct dirent *e = readdir(d)) {
    std::string n = e->d_name;
    if (n != "." && n != "..") out.push_back(n);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

std::string find_program(const std::string &name) {
  if (name.empty()) return "";
  if (name.find('/') != std::string::npos) return is_executable(name) ? name : "";
  for (auto &dir : split(env("PATH", "/usr/local/bin:/usr/bin:/bin"), ':')) {
    std::string p = join_path(dir, name);
    if (is_executable(p)) return p;
  }
  return "";
}

std::string os_release(const std::string &key) {
  std::string content;
  if (!read_file("/etc/os-release", content) && !read_file("/usr/lib/os-release", content)) return "";
  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    if (!starts_with(line, key + "=")) continue;
    std::string v = line.substr(key.size() + 1);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
    return v;
  }
  return "";
}

}  // namespace sde
