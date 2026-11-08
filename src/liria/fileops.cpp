// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "fileops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "desktop_model.h"
#include "file_chooser_model.h"
#include "i18n.h"
#include "location.h"
#include "util.h"

namespace sde {
namespace liria {

namespace {

std::string join(const std::string &dir, const std::string &name) {
  return dir == "/" ? "/" + name : dir + "/" + name;
}

struct Remembered {
  bool have_dir = false, have_other = false;
  Answer dir = Answer::Skip, other = Answer::Skip;
};

desk::FileOp::Kind op_kind(Transfer t) {
  return t == Transfer::Move ? desk::FileOp::MOVE : t == Transfer::Link ? desk::FileOp::LINK : desk::FileOp::COPY;
}

void add_op(Plan &plan, Transfer kind, const std::string &src, const std::string &dst, bool replace, bool top) {
  desk::FileOp op;
  op.kind = op_kind(kind);
  op.src = src;
  op.dst = dst;
  op.replace = replace;
  plan.ops.push_back(op);
  if (top) plan.results.push_back(dst);
  plan.moved.push_back({dst, kind == Transfer::Move ? src : std::string()});
}

void plan_item(const std::string &src, const std::string &dest_dir, Transfer kind, const Resolver &resolve, Plan &plan,
               Remembered &mem, bool top) {
  std::string name = basename_of(src);
  struct stat ss {};
  if (lstat(src.c_str(), &ss) != 0) {
    plan.errors.push_back(format(_("“%s” cannot be found."), name.c_str()));
    return;
  }
  bool src_dir = S_ISDIR(ss.st_mode);
  if (src_dir && kind != Transfer::Link && desk::path_inside(dest_dir, src)) {
    plan.errors.push_back(kind == Transfer::Move ? format(_("“%s” cannot be moved into itself."), name.c_str())
                                                 : format(_("“%s” cannot be copied into itself."), name.c_str()));
    return;
  }
  std::string dst = join(dest_dir, name);
  if (dst == src) {
    if (kind == Transfer::Move) return;  // already there
    // A copy (or link) in its own folder: "name (2)".
    add_op(plan, kind, src, join(dest_dir, free_name(dest_dir, name, src_dir)), false, top);
    return;
  }
  struct stat ds {};
  if (lstat(dst.c_str(), &ds) != 0) {
    add_op(plan, kind, src, dst, false, top);
    return;
  }
  bool dst_dir = S_ISDIR(ds.st_mode);
  if (ss.st_dev == ds.st_dev && ss.st_ino == ds.st_ino) {
    // The same item under another name (a link to the folder): moving it
    // there changes nothing, a copy gets a new name.
    if (kind == Transfer::Move) return;
    add_op(plan, kind, src, join(dest_dir, free_name(dest_dir, name, src_dir)), false, top);
    return;
  }
  Clash c;
  c.src = src;
  c.dst = dst;
  c.src_dir = src_dir;
  c.dst_dir = dst_dir;
  c.src_size = S_ISREG(ss.st_mode) ? (long long)ss.st_size : 0;
  c.dst_size = S_ISREG(ds.st_mode) ? (long long)ds.st_size : 0;
  c.src_mtime = ss.st_mtime;
  c.dst_mtime = ds.st_mtime;
  bool folders = src_dir && dst_dir;
  Answer a;
  if (folders && mem.have_dir) {
    a = mem.dir;
  } else if (!folders && mem.have_other) {
    a = mem.other;
  } else {
    bool all = false;
    a = resolve ? resolve(c, all) : Answer::Skip;
    if (all) {
      if (folders) {
        mem.have_dir = true;
        mem.dir = a;
      } else {
        mem.have_other = true;
        mem.other = a;
      }
    }
  }
  if (a == Answer::Merge && (!folders || kind == Transfer::Link)) a = kind == Transfer::Link ? Answer::KeepBoth : Answer::Replace;
  switch (a) {
    case Answer::Cancel:
      plan.cancelled = true;
      return;
    case Answer::Skip:
      return;
    case Answer::KeepBoth:
      add_op(plan, kind, src, join(dest_dir, free_name(dest_dir, name, src_dir)), false, top);
      return;
    case Answer::Replace:
      add_op(plan, kind, src, dst, true, top);
      return;
    case Answer::Merge: {
      // The folder's items go into the existing folder.
      std::vector<std::string> children;
      if (DIR *d = opendir(src.c_str())) {
        while (struct dirent *e = readdir(d))
          if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) children.push_back(join(src, e->d_name));
        closedir(d);
      }
      std::sort(children.begin(), children.end());
      for (const auto &child : children) {
        plan_item(child, dst, kind, resolve, plan, mem, false);
        if (plan.cancelled) return;
      }
      if (kind == Transfer::Move) plan.remove_after.push_back(src);
      if (top) plan.results.push_back(dst);
      return;
    }
  }
}

void add_errors(std::vector<std::string> *errors, const std::string &e) {
  if (errors && !e.empty()) errors->push_back(e);
}

// remove_tree() with progress and Cancel.
bool remove_counted(const std::string &path, desk::FileProgress *p, std::string *error) {
  if (p && !p->wait_while_paused()) {
    if (error) *error = "Cancelled";
    return false;
  }
  struct stat st {};
  if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  if (p) p->set_current(path);
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path.c_str());
    if (!d) {
      if (error) *error = format(_("Cannot delete “%s”: %s"), basename_of(path).c_str(), strerror(errno));
      return false;
    }
    std::vector<std::string> children;
    while (struct dirent *e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) children.push_back(join(path, e->d_name));
    closedir(d);
    bool ok = true;
    for (const auto &c : children) {
      if (!remove_counted(c, p, error)) {
        ok = false;
        if (p && p->cancel) return false;
      }
    }
    if (rmdir(path.c_str()) != 0) {
      if (ok && error) *error = format(_("Cannot delete “%s”: %s"), basename_of(path).c_str(), strerror(errno));
      ok = false;
    }
    if (p) p->files_done++;
    return ok;
  }
  if (unlink(path.c_str()) != 0) {
    if (error) *error = format(_("Cannot delete “%s”: %s"), basename_of(path).c_str(), strerror(errno));
    return false;
  }
  if (p) {
    p->files_done++;
    p->done += S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
  }
  return true;
}

bool rename_noreplace(const std::string &src, const std::string &dst) {
  int r = renameat2(AT_FDCWD, src.c_str(), AT_FDCWD, dst.c_str(), RENAME_NOREPLACE);
  if (r != 0 && (errno == EINVAL || errno == ENOSYS)) {
    struct stat st;
    if (lstat(dst.c_str(), &st) == 0) {
      errno = EEXIST;
      return false;
    }
    r = rename(src.c_str(), dst.c_str());
  }
  return r == 0;
}

}  // namespace

Plan plan_transfer(const std::vector<std::string> &sources, const std::string &dest_dir, Transfer kind,
                   const Resolver &resolve) {
  Plan plan;
  plan.kind = kind;
  Remembered mem;
  for (const auto &s : sources) {
    std::string src = chooser::normalize_path(s);
    if (src.empty() || src == "/") continue;
    plan_item(src, chooser::normalize_path(dest_dir), kind, resolve, plan, mem, true);
    if (plan.cancelled) {
      plan.ops.clear();
      plan.results.clear();
      plan.moved.clear();
      plan.remove_after.clear();
      break;
    }
  }
  return plan;
}

int run_plan(const Plan &plan, desk::FileProgress *progress, std::vector<std::string> *errors) {
  int failed = desk::run_file_ops(plan.ops, progress, errors);
  for (const auto &dir : plan.remove_after) rmdir(dir.c_str());  // only when it is empty now
  return failed;
}

int trash_paths(const std::vector<std::string> &paths, desk::FileProgress *progress, std::vector<TrashItem> *trashed,
                std::vector<std::string> *errors) {
  if (progress) {
    progress->files_total = long(paths.size());
    progress->total = 0;
  }
  int failed = 0;
  for (const auto &p : paths) {
    if (progress && !progress->wait_while_paused()) break;
    if (progress) progress->set_current(p);
    TrashItem item;
    std::string error;
    if (move_to_trash(p, &item, &error)) {
      if (trashed) trashed->push_back(item);
    } else {
      ++failed;
      add_errors(errors, error);
    }
    if (progress) progress->files_done++;
  }
  if (progress) progress->phase = desk::FileProgress::DONE;
  return failed;
}

int delete_paths(const std::vector<std::string> &paths, desk::FileProgress *progress, std::vector<std::string> *errors) {
  if (progress) {
    long items = 0;
    long long bytes = 0;
    for (const auto &p : paths) {
      items += desk::count_items(p);
      bytes += desk::tree_size(p);
    }
    progress->files_total = items;
    progress->total = bytes;
  }
  int failed = 0;
  for (const auto &p : paths) {
    std::string error;
    if (!remove_counted(p, progress, &error)) {
      if (progress && progress->cancel) break;
      ++failed;
      add_errors(errors, error);
    }
  }
  if (progress) progress->phase = desk::FileProgress::DONE;
  return failed;
}

int restore_items(const std::vector<TrashItem> &items, desk::FileProgress *progress, std::vector<std::string> *restored,
                  std::vector<std::string> *errors) {
  if (progress) progress->files_total = long(items.size());
  int failed = 0;
  for (const auto &it : items) {
    if (progress && !progress->wait_while_paused()) break;
    if (progress) progress->set_current(it.files_path());
    std::string to = it.original, error;
    if (!to.empty() && desk::path_exists(to)) {
      // Something new has the name: the item comes back as "name (2)".
      std::string dir = chooser::parent_dir(to);
      struct stat st {};
      bool is_dir = lstat(it.files_path().c_str(), &st) == 0 && S_ISDIR(st.st_mode);
      to = join(dir, free_name(dir, basename_of(to), is_dir));
    }
    if (restore_from_trash(it, to, &error)) {
      if (restored) restored->push_back(to);
    } else {
      ++failed;
      add_errors(errors, error);
    }
    if (progress) progress->files_done++;
  }
  if (progress) progress->phase = desk::FileProgress::DONE;
  return failed;
}

int empty_all_trash(desk::FileProgress *progress, std::vector<std::string> *errors) {
  std::vector<TrashItem> items = list_trash();
  if (progress) {
    long n = 0;
    for (const auto &it : items) n += desk::count_items(it.files_path());
    progress->files_total = n;
  }
  int failed = 0;
  for (const auto &it : items) {
    std::string error;
    if (!remove_counted(it.files_path(), progress, &error)) {
      if (progress && progress->cancel) break;
      ++failed;
      add_errors(errors, error);
      continue;
    }
    unlink(it.info_path().c_str());
  }
  if (progress) progress->phase = desk::FileProgress::DONE;
  return failed;
}

bool rename_path(const std::string &path, const std::string &name_in, std::string *new_path, std::string *error) {
  std::string name = name_in;
  std::string problem = name_problem(name);
  if (!problem.empty()) {
    if (error) *error = problem;
    return false;
  }
  std::string dir = chooser::parent_dir(path);
  if (dir.empty()) {
    if (error) *error = _("This item cannot be renamed.");
    return false;
  }
  std::string dst = join(dir, name);
  if (new_path) *new_path = dst;
  if (dst == path) return true;
  struct stat a {}, b {};
  if (lstat(path.c_str(), &a) != 0) {
    if (error) *error = format(_("“%s” cannot be found."), basename_of(path).c_str());
    return false;
  }
  if (lstat(dst.c_str(), &b) == 0) {
    if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
      // Only the case changes on a file system that ignores it (FAT, exFAT):
      // through a temporary name.
      std::string tmp = join(dir, "." + name + ".sde-rename");
      if (rename_noreplace(path, tmp) && rename_noreplace(tmp, dst)) return true;
      rename(tmp.c_str(), path.c_str());
    }
    if (error) *error = format(_("There is already an item named “%s” here."), name.c_str());
    return false;
  }
  if (!rename_noreplace(path, dst)) {
    int err = errno;
    if (error)
      *error = err == EEXIST ? format(_("There is already an item named “%s” here."), name.c_str())
                             : format(_("Cannot rename “%s”: %s"), basename_of(path).c_str(), strerror(err));
    return false;
  }
  return true;
}

std::string free_name(const std::string &dir, const std::string &name, bool is_dir) {
  struct Ctx {
    std::string dir;
  } ctx{dir};
  auto taken = [](const std::string &n, void *d) { return desk::path_exists(join(static_cast<Ctx *>(d)->dir, n)); };
  if (!taken(name, &ctx)) return name;
  return desk::keep_both_name(name, is_dir, taken, &ctx);
}

std::string create_folder(const std::string &dir, const std::string &name_in, std::string *error) {
  std::string name = name_in.empty() ? std::string(_("New folder")) : name_in;
  std::string problem = name_problem(name);
  if (!problem.empty()) {
    if (error) *error = problem;
    return "";
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::string path = join(dir, free_name(dir, name, true));
    if (mkdir(path.c_str(), 0777) == 0) return path;
    if (errno != EEXIST) {
      if (error) *error = format(_("Cannot create the folder: %s"), strerror(errno));
      return "";
    }
  }
  if (error) *error = format(_("Cannot create the folder: %s"), strerror(EEXIST));
  return "";
}

std::string create_document(const std::string &dir, const std::string &name_in, const std::string &template_path,
                            std::string *error) {
  std::string name = name_in;
  if (name.empty()) name = template_path.empty() ? std::string(_("New Text Document.txt")) : basename_of(template_path);
  std::string problem = name_problem(name);
  if (!problem.empty()) {
    if (error) *error = problem;
    return "";
  }
  std::string path = join(dir, free_name(dir, name, false));
  if (!template_path.empty()) {
    std::string copy_error;
    if (!desk::copy_path(template_path, path, nullptr, &copy_error)) {
      if (error) *error = copy_error;
      return "";
    }
    return path;
  }
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (fd < 0) {
    if (error) *error = format(_("Cannot create the document: %s"), strerror(errno));
    return "";
  }
  close(fd);
  return path;
}

std::vector<std::string> templates() {
  std::vector<std::string> out;
  std::string dir = chooser::normalize_path(user_dir("TEMPLATES"));
  if (dir.empty() || dir == home_dir()) return out;
  DIR *d = opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string p = join(dir, e->d_name);
    struct stat st {};
    if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) out.push_back(p);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

// ------------------------------------------------------------------ undo

void UndoStack::push(UndoStep step) {
  if (step.pairs.empty() && step.trashed.empty()) return;
  steps_.push_back(std::move(step));
  if (steps_.size() > 30) steps_.erase(steps_.begin());
}

UndoStep UndoStack::pop() {
  UndoStep s = std::move(steps_.back());
  steps_.pop_back();
  return s;
}

std::string UndoStack::label(const UndoStep &step) {
  switch (step.kind) {
    case UndoStep::Rename: return _("Undo Rename");
    case UndoStep::Move: return _("Undo Move");
    case UndoStep::Copy: return _("Undo Copy");
    case UndoStep::Trash: return _("Undo Delete");
    case UndoStep::Create: return _("Undo New");
  }
  return _("Undo");
}

int undo_step(const UndoStep &step, desk::FileProgress *progress, std::vector<std::string> *errors) {
  switch (step.kind) {
    case UndoStep::Rename: {
      int failed = 0;
      for (const auto &pr : step.pairs) {
        std::string error;
        if (!rename_path(pr.first, basename_of(pr.second), nullptr, &error)) {
          ++failed;
          add_errors(errors, error);
        }
      }
      return failed;
    }
    case UndoStep::Move: {
      std::vector<desk::FileOp> ops;
      for (auto it = step.pairs.rbegin(); it != step.pairs.rend(); ++it) {
        if (it->second.empty() || !desk::path_exists(it->first)) continue;
        if (desk::path_exists(it->second)) {
          add_errors(errors, format(_("“%s” was not moved back: there is something with its name."),
                                    basename_of(it->second).c_str()));
          continue;
        }
        make_dirs(chooser::parent_dir(it->second));
        desk::FileOp op;
        op.kind = desk::FileOp::MOVE;
        op.src = it->first;
        op.dst = it->second;
        ops.push_back(op);
      }
      return desk::run_file_ops(ops, progress, errors);
    }
    case UndoStep::Copy:
    case UndoStep::Create: {
      std::vector<std::string> made;
      for (const auto &pr : step.pairs)
        if (desk::path_exists(pr.first)) made.push_back(pr.first);
      return trash_paths(made, progress, nullptr, errors);
    }
    case UndoStep::Trash:
      return restore_items(step.trashed, progress, nullptr, errors);
  }
  return 0;
}

}  // namespace liria
}  // namespace sde
