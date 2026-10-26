// SPDX-License-Identifier: GPL-3.0-or-later
// Test helper (not installed): copies a folder with SDE's safe copy engine
// (src/common/safe_copy.h) and measures it against the drive, for
// tools/safe-copy-bench.py and docs/TRANSFERS.md.
//
//   sde-safe-copy-bench SOURCE DESTINATION [--method cache|range|fsync]
//                       [--device loop0] [--sync-after]
//
// Prints one line of key=value pairs: the seconds until the copy said it
// was done (done), until the data was really on the drive (durable: the
// same in the safe modes; --sync-after adds a syncfs () for the cache
// mode), the bytes, the files, and with --device the largest lead of the
// reported progress over what the drive had written (lead, from
// /sys/block/DEVICE/stat; it includes metadata, so a lead <= 0 means the
// progress never ran ahead of the drive) and the largest amount of data
// written but not on the drive (dirty).
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "safe_copy.h"

namespace {

struct Bench {
  SdeWriter writer;
  std::string device;
  long long device_start = 0;
  long long reported = 0;  // bytes the copy said were done
  long long max_lead = -(1LL << 62), max_dirty = 0;
  long long bytes = 0, files = 0;
  std::vector<unsigned char> buffer = std::vector<unsigned char>(1 << 20);
};

long long device_bytes(const std::string &device) {
  if (device.empty()) return 0;
  FILE *f = fopen(("/sys/block/" + device + "/stat").c_str(), "r");
  long long v[11] = {0};
  if (f) {
    for (int i = 0; i < 11; ++i)
      if (fscanf(f, "%lld", &v[i]) != 1) break;
    fclose(f);
  }
  return v[6] * 512;
}

void sample(Bench &b) {
  if (b.device.empty()) return;
  long long on_drive = device_bytes(b.device) - b.device_start;
  b.max_lead = std::max(b.max_lead, b.reported - on_drive);
  b.max_dirty = std::max(b.max_dirty, b.writer.written - on_drive);
}

void confirmed(void *data, long long bytes) {
  auto *b = static_cast<Bench *>(data);
  b->reported += bytes;
  sample(*b);
}

bool copy_tree(Bench &b, const std::string &src, const std::string &dst) {
  struct stat st;
  if (lstat(src.c_str(), &st) != 0) return false;
  if (S_ISDIR(st.st_mode)) {
    if (mkdir(dst.c_str(), 0755) != 0 && errno != EEXIST) return false;
    DIR *d = opendir(src.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (struct dirent *e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto &n : names)
      if (!copy_tree(b, src + "/" + n, dst + "/" + n)) return false;
    return true;
  }
  if (!S_ISREG(st.st_mode)) return true;
  int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
  int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (in < 0 || out < 0) return false;
  int source_error = 0;
  int error = sde_copy_data(&b.writer, in, out, st.st_size, b.buffer.data(), b.buffer.size(), nullptr, &source_error);
  close(in);
  close(out);
  if (error) {
    fprintf(stderr, "%s: %s\n", dst.c_str(), strerror(error));
    return false;
  }
  b.bytes += st.st_size;
  b.files++;
  sample(b);
  return sde_writer_file_done(&b.writer) == 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s SOURCE DESTINATION [--method cache|range|fsync] [--device NAME] [--sync-after]\n",
            argv[0]);
    return 2;
  }
  std::string src = argv[1], dst = argv[2], method = "range";
  bool sync_after = false;
  Bench b;
  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--method" && i + 1 < argc) method = argv[++i];
    else if (a == "--device" && i + 1 < argc) b.device = argv[++i];
    else if (a == "--sync-after") sync_after = true;
  }
  SdeWriterMethod m = method == "cache" ? SDE_WRITER_CACHE : method == "fsync" ? SDE_WRITER_FSYNC : SDE_WRITER_RANGE;
  std::string parent = dst.substr(0, dst.find_last_of('/'));
  int dir_fd = open(parent.empty() ? "/" : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  sde_writer_init(&b.writer, m, dir_fd, nullptr, nullptr);
  b.writer.confirmed_cb = confirmed;
  b.writer.confirmed_data = &b;
  b.device_start = device_bytes(b.device);
  double start = sde_writer_clock(nullptr);
  bool ok = copy_tree(b, src, dst);
  int error = sde_writer_finish(&b.writer);
  double done = sde_writer_clock(nullptr) - start;
  sample(b);
  double durable = done;
  if (sync_after) {
    int fd = open(parent.empty() ? "/" : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
      syncfs(fd);
      fsync(fd);
      close(fd);
    }
    durable = sde_writer_clock(nullptr) - start;
  }
  printf("method=%s done=%.3f durable=%.3f bytes=%lld files=%lld", method.c_str(), done, durable, b.bytes, b.files);
  if (!b.device.empty())
    printf(" lead=%lld dirty=%lld drive=%lld", b.max_lead, b.max_dirty, device_bytes(b.device) - b.device_start);
  printf(" window=%lld rate=%.0f error=%d\n", b.writer.window, b.writer.rate, error);
  return ok && !error ? 0 : 1;
}
