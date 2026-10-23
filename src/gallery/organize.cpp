// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Gallery's organizing actions: albums the user makes, the photo menu, and
// batch actions on many photos (rotate, resize and export, copy or move to
// a folder, share by email, slideshow of a set). Batch work runs on a
// worker thread and reports back through the event loop. Editing a single
// photo is Photo's job (sde-view), not Gallery's.
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <thread>

#include "edit.h"
#include "gallery.h"
#include "i18n.h"
#include "menu.h"
#include "save.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

namespace gallery {

namespace {

bool rotate_file(const std::string &path, int q, std::string *err) {
  photo::Format f = photo::sniff_format(path);
  photo::Edits e;
  e.orient = Orient{((q % 4) + 4) % 4, false};
  // JPEG: change the EXIF orientation, without loss of quality.
  if (f == photo::Format::Jpeg && photo::format_of(path) == f && photo::lossless_possible(path, e, path))
    return photo::save_edits(path, e, nullptr, path, 92, err) != photo::SaveResult::Failed;
  if (!photo::can_write(f) || photo::format_of(path) != f) {
    if (err) *err = format(_("%s files cannot be rotated"), photo::format_name(f));
    return false;
  }
  cairo_surface_t *src = decode_image(path, 0);
  if (!src) {
    if (err) *err = _("the image could not be read");
    return false;
  }
  cairo_surface_t *out = photo::render(src, e);
  cairo_surface_destroy(src);
  bool ok = photo::save_edits(path, e, out, path, 92, err) != photo::SaveResult::Failed;
  cairo_surface_destroy(out);
  return ok;
}

photo::Format export_format(const std::string &path) {
  photo::Format f = photo::sniff_format(path);
  if (f == photo::Format::Jpeg || f == photo::Format::Png || f == photo::Format::Webp || f == photo::Format::Tiff)
    return photo::can_write(f) ? f : photo::Format::Png;
  return photo::Format::Png;
}

const char *format_ext(photo::Format f) {
  switch (f) {
    case photo::Format::Jpeg: return "jpg";
    case photo::Format::Webp: return "webp";
    case photo::Format::Tiff: return "tiff";
    default: break;
  }
  return "png";
}

bool export_file(const std::string &src, const std::string &dest, int max_edge, photo::Format f, std::string *err) {
  // Same format and size: a plain copy keeps every byte and the metadata.
  if (max_edge <= 0 && photo::sniff_format(src) == f) return copy_file(src, dest, err);
  int w = 0, h = 0;
  cairo_surface_t *img = decode_image(src, 0, &w, &h);
  if (!img) {
    if (err) *err = _("the image could not be read");
    return false;
  }
  cairo_surface_t *out = photo::downscale(img, max_edge);
  cairo_surface_destroy(img);
  bool jpeg = photo::sniff_format(src) == photo::Format::Jpeg && f == photo::Format::Jpeg;
  bool ok = photo::write_image(out, dest, f, 90, jpeg ? src : "", err);
  cairo_surface_destroy(out);
  return ok;
}

bool move_file(const std::string &from, const std::string &to, std::string *err) {
  if (rename(from.c_str(), to.c_str()) == 0) return true;
  if (errno != EXDEV) {
    if (err) *err = strerror(errno);
    return false;
  }
  // Another file system: copy, then remove the original.
  if (!copy_file(from, to, err)) return false;
  if (unlink(from.c_str()) != 0) {
    if (err) *err = strerror(errno);
    return false;
  }
  return true;
}

std::string choose_folder(const char *title, const std::string &start) {
  Fl_Native_File_Chooser chooser;
  chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
  chooser.options(Fl_Native_File_Chooser::NEW_FOLDER);
  chooser.title(title);
  chooser.directory(start.c_str());
  if (chooser.show() != 0 || !chooser.filename()) return "";
  std::string dir = chooser.filename();
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  return dir;
}

}  // namespace

// ------------------------------------------------------------------ albums

void GalleryWindow::reload_albums() {
  albums_ = list_user_albums(albums_dir());
  if (view_kind_.kind == ViewKind::Album) {
    bool exists = false;
    for (const auto &a : albums_) exists |= a.file == view_kind_.folder;
    if (!exists) {
      view_kind_ = View{};
      rebuild_view(false);
    } else {
      album_photos_ = photos_for_paths(read_album_list(view_kind_.folder));
    }
  }
  counts_.clear();
  if (sidebar_) sidebar_->redraw();
}

void GalleryWindow::new_album(const std::vector<std::string> &add) {
  const char *r = fl_input("%s", "", _("Name of the new album:"));
  if (!r) return;
  std::string file, err;
  if (!create_album(albums_dir(), r, &file, &err)) {
    toast(err == "exists" ? format(_("An album named “%s” exists already"), r) : std::string(_("Enter a name for the album")));
    return;
  }
  reload_albums();
  if (!add.empty()) {
    add_to_album(file, add);
  } else {
    set_view(View{ViewKind::Album, file});
    toast(_("Add photos with “Add to Album” in the photo menu"));
  }
}

void GalleryWindow::add_to_album(const std::string &file, const std::vector<std::string> &paths) {
  int n = gallery::add_to_album(file, paths);
  std::string name = view_title(View{ViewKind::Album, file});
  if (n < 0) toast(_("The album could not be saved"));
  else if (n == 0) toast(format(_("Already in “%s”"), name.c_str()));
  else toast(format(n == 1 ? _("Added %d photo to “%s”") : _("Added %d photos to “%s”"), n, name.c_str()));
  reload_albums();
  if (view_kind_.kind == ViewKind::Album && view_kind_.folder == file) rebuild_view(true);
}

void GalleryWindow::remove_from_album(const std::vector<std::string> &paths) {
  if (view_kind_.kind != ViewKind::Album) return;
  int n = gallery::remove_from_album(view_kind_.folder, paths);
  if (n > 0) toast(format(n == 1 ? _("Removed %d photo from the album") : _("Removed %d photos from the album"), n));
  grid_->clear_selection();
  reload_albums();
  rebuild_view(true);
}

void GalleryWindow::rename_album(const std::string &file) {
  std::string old = view_title(View{ViewKind::Album, file});
  const char *r = fl_input("%s", old.c_str(), _("New name of the album:"));
  if (!r) return;
  std::string nf, err;
  if (!gallery::rename_album(file, r, &nf, &err)) {
    toast(err == "exists" ? format(_("An album named “%s” exists already"), r) : std::string(_("The album could not be renamed")));
    return;
  }
  if (view_kind_.kind == ViewKind::Album && view_kind_.folder == file) view_kind_.folder = nf;
  reload_albums();
  redraw();
}

void GalleryWindow::delete_album(const std::string &file) {
  std::string name = view_title(View{ViewKind::Album, file});
  if (ask(_("Delete Album"), format(_("Delete the album “%s”?\nThe photos stay where they are."), name.c_str()),
          {_("Cancel"), _("Delete Album")}) != 1)
    return;
  gallery::delete_album(file);
  reload_albums();
  toast(format(_("Album “%s” deleted"), name.c_str()));
  redraw();
}

ContextMenu GalleryWindow::album_menu(const std::vector<std::string> &paths) {
  ContextMenu m;
  for (const auto &a : albums_) {
    std::string file = a.file;
    m.item(a.name, [this, file, paths] { add_to_album(file, paths); }, {"image"});
  }
  if (!albums_.empty()) m.separator();
  m.item(_("New Album…"), [this, paths] { new_album(paths); }, {"new"});
  return m;
}

// ------------------------------------------------------------- batch work

void GalleryWindow::run_batch(const std::string &busy, std::vector<std::function<bool(std::string *)>> steps,
                              std::function<void(size_t, size_t, const std::string &)> after) {
  if (batch_running_ || importing_) {
    toast(_("Please wait: photos are still being processed"));
    return;
  }
  struct Job {
    GalleryWindow *self;
    std::vector<std::function<bool(std::string *)>> steps;
    std::function<void(size_t, size_t, const std::string &)> after;
    size_t done = 0, failed = 0;
    std::string error;
  };
  auto *job = new Job{this, std::move(steps), std::move(after), 0, 0, std::string()};
  batch_running_ = true;
  toast(busy);
  std::thread([job] {
    for (auto &step : job->steps) {
      std::string err;
      if (step(&err)) ++job->done;
      else {
        ++job->failed;
        job->error = err;
      }
    }
    Fl::awake([](void *p) {
      auto *job = static_cast<Job *>(p);
      if (instance_ == job->self) {
        job->self->batch_running_ = false;
        job->after(job->done, job->failed, job->error);
      }
      delete job;
    }, job);
  }).detach();
}

void GalleryWindow::files_changed(const std::vector<std::string> &paths) {
  for (const auto &p : paths) {
    forget_thumbnails(p);
    struct stat st {};
    bool ok = stat(p.c_str(), &st) == 0;
    for (auto *list : {&all_, &ext_photos_, &album_photos_})
      for (auto &ph : *list)
        if (ph.path == p && ok) {
          ph.mtime = st.st_mtime;
          ph.size = st.st_size;
        }
  }
  rebuild_view(true);
}

void GalleryWindow::rotate_files(const std::vector<std::string> &paths, int q) {
  std::vector<std::function<bool(std::string *)>> steps;
  for (const auto &p : paths) steps.push_back([p, q](std::string *err) { return rotate_file(p, q, err); });
  run_batch(format(_("Rotating %zu photos…"), paths.size()), std::move(steps),
            [this, paths, q](size_t done, size_t failed, const std::string &error) {
              files_changed(paths);
              if (failed) toast(format(_("Rotated %zu photos; %zu could not be rotated (%s)"), done, failed, error.c_str()));
              else toast(done == 1 ? (q < 0 ? _("Rotated left") : _("Rotated right"))
                                   : format(_("Rotated %zu photos"), done));
            });
}

void GalleryWindow::export_files(const std::vector<std::string> &paths, int max_edge) {
  std::string dir = choose_folder(_("Export to Folder"), pictures_);
  if (dir.empty()) return;
  std::vector<std::function<bool(std::string *)>> steps;
  std::vector<std::string> planned;
  for (const auto &p : paths) {
    photo::Format f = export_format(p);
    std::string dest = unique_destination(dir, basename_of(p), nullptr, planned, format_ext(f));
    planned.push_back(dest);
    steps.push_back([p, dest, max_edge, f](std::string *err) { return export_file(p, dest, max_edge, f, err); });
  }
  run_batch(format(_("Exporting %zu photos…"), paths.size()), std::move(steps),
            [this, dir](size_t done, size_t failed, const std::string &error) {
              std::string name = basename_of(dir);
              if (failed) toast(format(_("Exported %zu photos; %zu failed (%s)"), done, failed, error.c_str()));
              else toast(format(done == 1 ? _("Exported %zu photo to “%s”") : _("Exported %zu photos to “%s”"), done,
                                name.c_str()));
              for (const auto &r : library_settings().roots())
                if (dir == r || path_under(dir, r)) start_scan();
            });
}

void GalleryWindow::copy_files_to(const std::vector<std::string> &paths, bool move) {
  std::string dir = choose_folder(move ? _("Move to Folder") : _("Copy to Folder"), pictures_);
  if (dir.empty()) return;
  auto plan = plan_copy(paths, dir, nullptr);
  std::vector<std::function<bool(std::string *)>> steps;
  for (const auto &i : plan) {
    if (i.source.substr(0, i.source.rfind('/')) == dir && move) continue;  // already there
    steps.push_back([i, move](std::string *err) {
      return move ? move_file(i.source, i.dest, err) : copy_file(i.source, i.dest, err);
    });
  }
  if (steps.empty()) {
    toast(_("The photos are in that folder already"));
    return;
  }
  // Counted first: the order of the arguments is unspecified, and GCC moves
  // |steps| into run_batch() before formatting the text.
  size_t count = steps.size();
  run_batch(format(move ? _("Moving %zu photos…") : _("Copying %zu photos…"), count), std::move(steps),
            [this, plan, move, dir](size_t done, size_t failed, const std::string &error) {
              if (move) {
                // Albums and favorites follow the moved photos.
                bool fav = false;
                for (const auto &i : plan) {
                  if (file_exists(i.source)) continue;  // not moved
                  album_path_changed(albums_dir(), i.source, i.dest);
                  if (favorites_.erase(i.source)) {
                    favorites_.insert(i.dest);
                    fav = true;
                  }
                  forget_thumbnails(i.source);
                  for (auto *list : {&all_, &ext_photos_})
                    list->erase(std::remove_if(list->begin(), list->end(),
                                               [&](const Photo &ph) { return ph.path == i.source; }),
                                list->end());
                }
                if (fav) save_settings();
                grid_->clear_selection();
                reload_albums();
                rebuild_view(true);
              }
              std::string name = basename_of(dir);
              if (failed)
                toast(format(_("%zu photos done; %zu failed (%s)"), done, failed, error.c_str()));
              else
                toast(format(move ? _("Moved %zu photos to “%s”") : _("Copied %zu photos to “%s”"), done, name.c_str()));
              start_scan();
            });
}

void GalleryWindow::share_files(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  if (find_program("xdg-email").empty()) {
    toast(_("Sharing by email needs xdg-email (xdg-utils)"));
    return;
  }
  std::vector<std::string> argv{"xdg-email", "--subject",
                                paths.size() == 1 ? basename_of(paths[0]) : format(_("%zu photos"), paths.size())};
  for (const auto &p : paths) {
    argv.push_back("--attach");
    argv.push_back(p);
  }
  if (!spawn_detached(argv)) toast(_("No email application is available"));
}

void GalleryWindow::slideshow(const std::vector<std::string> &paths) {
  if (view_.empty()) {
    toast(_("There are no photos to show"));
    return;
  }
  std::vector<int> subset;
  for (const auto &p : paths) {
    int i = index_of(view_, p);
    if (i >= 0) subset.push_back(i);
  }
  int start = !subset.empty() ? subset[0] : std::max(0, grid_->focus_index());
  if (start >= int(view_.size())) start = 0;
  open_viewer(start);
  viewer_->start_slideshow(subset.size() > 1 ? subset : std::vector<int>());
}

// --------------------------------------------------------------- the menu

void GalleryWindow::photo_menu(int index, const std::vector<std::string> &paths, int x, int y) {
  if (paths.empty()) return;
  if (index < 0) index = index_of(view_, paths[0]);
  bool single = paths.size() == 1;
  std::string first = paths[0];
  bool all_fav = std::all_of(paths.begin(), paths.end(), [this](const std::string &p) { return is_favorite(p); });
  ContextMenu m;
  m.item(_("Open in Photo"), [this, first] { edit_in_photo(first); }, {"image", FL_CTRL + 'e', single});
  m.item(_("Quick Preview"), [this, index] { preview(index); }, {"", FL_CTRL + 'p', single && index >= 0});
  m.item(single ? _("Slideshow") : format(_("Slideshow of %zu Photos"), paths.size()),
         [this, paths, single] { slideshow(single ? std::vector<std::string>() : paths); }, {"", FL_F + 5, true, true});
  m.item(all_fav ? _("Remove from Favorites") : _("Add to Favorites"), [this, paths] { toggle_favorite(paths); },
         {"", FL_CTRL + 'd'});
  m.submenu(_("Add to Album"), album_menu(paths));
  if (view_kind_.kind == ViewKind::Album)
    m.item(_("Remove from Album"), [this, paths] { remove_from_album(paths); });
  m.separator();
  m.item(_("Rotate Left"), [this, paths] { rotate_files(paths, -1); });
  m.item(_("Rotate Right"), [this, paths] { rotate_files(paths, 1); });
  ContextMenu sizes;
  for (int edge : {0, 3840, 2048, 1280, 640})
    sizes.item(edge ? format(_("Longest side %d px"), edge) : std::string(_("Original size")),
               [this, paths, edge] { export_files(paths, edge); });
  m.submenu(_("Resize and Export"), std::move(sizes));
  m.item(_("Copy to Folder…"), [this, paths] { copy_files_to(paths, false); }, {"copy"});
  m.item(_("Move to Folder…"), [this, paths] { copy_files_to(paths, true); }, {"folder"});
  m.item(_("Share by Email…"), [this, paths] { share_files(paths); }, {"share", 0, true, true});
  if (single) {
    m.item(_("Set as Wallpaper"), [this, first] { set_wallpaper(first); });
    m.item(_("Show in Files"), [this, first] { show_in_files(first); }, {"folder"});
    m.item(_("Open With Other Application"), [this, first] { open_with(first); }, {"open", 0, true, true});
  }
  m.item(single ? _("Copy Path") : _("Copy Paths"), [this, paths] { copy_paths(paths); }, {"copy"});
  m.item(_("Move to Trash…"), [this, paths] { trash(paths); }, {"trash"});
  if (x >= 0) m.popup(x, y);
  else m.popup();
  redraw();
}

}  // namespace gallery
