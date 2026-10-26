// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// Gallery's plugins that work on many photos: batch rename, format
// conversion, printing (and PDF), sending by email, the slideshow
// settings, cameras and phones, finding duplicates, and the Places view.
#include <FL/Fl.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <cairo-pdf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include "config.h"
#include "duplicates.h"
#include "edit.h"
#include "exif.h"
#include "forms.h"
#include "gallery.h"
#include "i18n.h"
#include "menubar.h"
#include "places.h"
#include "process.h"
#include "save.h"
#include "spawn.h"
#include "util.h"

using namespace sde;

namespace gallery {

namespace {

std::string choose_folder_for(const char *title, const std::string &start) {
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

bool path_exists(const std::string &p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}

struct OutputFormat {
  photo::Format format;
  const char *name;
  const char *ext;
};

std::vector<OutputFormat> writable_formats() {
  std::vector<OutputFormat> out;
  for (auto f : {OutputFormat{photo::Format::Jpeg, "JPEG", "jpg"}, OutputFormat{photo::Format::Png, "PNG", "png"},
                 OutputFormat{photo::Format::Webp, "WebP", "webp"}, OutputFormat{photo::Format::Tiff, "TIFF", "tiff"}})
    if (photo::can_write(f.format)) out.push_back(f);
  return out;
}

// Writes |src| as |dest| in |f|: resized to |max_edge| (0 keeps the size),
// with the camera details of a JPEG kept when |keep_exif|.
bool convert_file(const std::string &src, const std::string &dest, photo::Format f, int quality, int max_edge,
                  bool keep_exif, std::string *err) {
  cairo_surface_t *img = decode_image(src, 0);
  if (!img) {
    if (err) *err = _("the image could not be read");
    return false;
  }
  cairo_surface_t *out = photo::downscale(img, max_edge);
  cairo_surface_destroy(img);
  bool jpeg_exif = keep_exif && photo::sniff_format(src) == photo::Format::Jpeg && f == photo::Format::Jpeg;
  bool ok = photo::write_image(out, dest, f, quality, jpeg_exif ? src : "", err);
  cairo_surface_destroy(out);
  return ok;
}

// The size for printing a photo |dpi| dots per inch into |w| x |h| points.
int print_pixels(double w, double h, double dpi) { return int(std::ceil(std::max(w, h) / 72.0 * dpi)); }

}  // namespace

// ----------------------------------------------------------------- slideshow

void GalleryWindow::slideshow_settings() {
  Config cfg;
  cfg.load();
  static const char *keys[] = {"none", "crossfade", "slide", "kenburns"};
  std::string cur = cfg.get("gallery", "slideshow_transition", "crossfade");
  int tr = 1;
  for (int i = 0; i < 4; ++i)
    if (cur == keys[i]) tr = i;
  int seconds = cfg.get_int("gallery", "slideshow_seconds", 5);
  static const int lengths[] = {3, 5, 8, 12};
  int len = 1;
  for (int i = 0; i < 4; ++i)
    if (seconds == lengths[i]) len = i;
  Form f(_("Slideshow"), _("How the photos of a slideshow follow each other."));
  int t = f.choice(_("Transition"), {_("None"), _("Crossfade"), _("Slide"), _("Zoom (Ken Burns)")}, tr);
  int l = f.choice(_("Each photo"), {_("3 seconds"), _("5 seconds"), _("8 seconds"), _("12 seconds")}, len);
  int s = f.check(_("Shuffle the photos"), cfg.get_bool("gallery", "slideshow_shuffle", false));
  int r = f.check(_("Start again after the last photo"), cfg.get_bool("gallery", "slideshow_loop", true));
  if (f.run({_("Cancel"), _("Save")}) != 1) return;
  cfg.load();
  cfg.set("gallery", "slideshow_transition", keys[f.choice_value(t)]);
  cfg.set("gallery", "slideshow_seconds", std::to_string(lengths[f.choice_value(l)]));
  cfg.set("gallery", "slideshow_shuffle", f.check_value(s) ? "true" : "false");
  cfg.set("gallery", "slideshow_loop", f.check_value(r) ? "true" : "false");
  cfg.save();
  toast(_("Slideshow settings saved"));
}

// --------------------------------------------------------------------- email

namespace {
const int kEmailSizes[] = {0, 2048, 1280, 800};
}

void GalleryWindow::email_settings() {
  Config cfg;
  cfg.load();
  int cur = cfg.get_int("gallery", "email_size", 1280), sel = 2;
  for (int i = 0; i < 4; ++i)
    if (kEmailSizes[i] == cur) sel = i;
  Form f(_("Send by Email"), _("Photos are attached to a new message in your email application."));
  int size = f.choice(_("Size of the photos"),
                      {_("As they are"), _("Large (2048 px)"), _("Medium (1280 px)"), _("Small (800 px)")}, sel);
  if (f.run({_("Cancel"), _("Save")}) != 1) return;
  cfg.load();
  cfg.set("gallery", "email_size", std::to_string(kEmailSizes[f.choice_value(size)]));
  cfg.save();
}

void GalleryWindow::share_files(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  if (find_program("xdg-email").empty()) {
    toast(_("Sharing by email needs xdg-email (xdg-utils)"));
    return;
  }
  Config cfg;
  cfg.load();
  int cur = cfg.get_int("gallery", "email_size", 1280), sel = 2;
  for (int i = 0; i < 4; ++i)
    if (kEmailSizes[i] == cur) sel = i;
  long long total = 0;
  for (const auto &p : paths) {
    struct stat st {};
    if (stat(p.c_str(), &st) == 0) total += st.st_size;
  }
  Form f(_("Send by Email"), paths.size() == 1 ? format(_("“%s”, %s"), basename_of(paths[0]).c_str(), human_size(total).c_str())
                                               : format(_("%zu photos, %s"), paths.size(), human_size(total).c_str()));
  int size = f.choice(_("Size of the photos"),
                      {_("As they are"), _("Large (2048 px)"), _("Medium (1280 px)"), _("Small (800 px)")}, sel);
  f.note(_("Smaller copies are quicker to send; the photos themselves do not change."));
  if (f.run({_("Cancel"), _("Send")}) != 1) return;
  int edge = kEmailSizes[f.choice_value(size)];
  cfg.load();
  cfg.set("gallery", "email_size", std::to_string(edge));
  cfg.save();
  std::string subject = paths.size() == 1 ? basename_of(paths[0]) : format(_("%zu photos"), paths.size());
  auto send = [this, subject](const std::vector<std::string> &files) {
    std::vector<std::string> argv{"xdg-email", "--subject", subject};
    for (const auto &p : files) {
      argv.push_back("--attach");
      argv.push_back(p);
    }
    if (!spawn_detached(argv)) toast(_("No email application is available"));
  };
  if (edge == 0) {
    send(paths);
    return;
  }
  // Smaller JPEG copies in a private folder, then the message.
  std::string dir = runtime_dir() + "/sde-gallery/email-" + std::to_string(getpid()) + "-" + std::to_string(time(nullptr));
  make_dirs(dir);
  std::vector<std::function<bool(std::string *)>> steps;
  std::vector<std::string> copies, planned;
  for (const auto &p : paths) {
    std::string dest = unique_destination(dir, basename_of(p), nullptr, planned, "jpg");
    planned.push_back(dest);
    copies.push_back(dest);
    steps.push_back([p, dest, edge](std::string *err) {
      return convert_file(p, dest, photo::Format::Jpeg, 85, edge, true, err);
    });
  }
  run_batch(_("Making smaller copies…"), std::move(steps),
            [this, copies, send](size_t done, size_t failed, const std::string &error) {
              if (failed) toast(format(_("%zu photos could not be made smaller (%s)"), failed, error.c_str()));
              std::vector<std::string> ok;
              for (const auto &c : copies)
                if (path_exists(c)) ok.push_back(c);
              if (!ok.empty()) send(ok);
              (void)done;
            });
}

// -------------------------------------------------------------------- rename

void GalleryWindow::rename_files(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  if (batch_running_ || importing_) {
    toast(_("Please wait: photos are still being processed"));
    return;
  }
  std::vector<RenameSource> sources;
  for (const auto &p : paths) {
    RenameSource s;
    s.path = p;
    PhotoInfo info;
    if (read_photo_info(p, info)) {
      s.taken = info.taken;
      s.camera = info.model;
    }
    if (!s.taken) {
      struct stat st {};
      if (stat(p.c_str(), &st) == 0) s.taken = st.st_mtime;
    }
    sources.push_back(s);
  }
  // Photos taken first come first.
  std::stable_sort(sources.begin(), sources.end(),
                   [](const RenameSource &a, const RenameSource &b) { return a.taken < b.taken; });
  Config cfg;
  cfg.load();
  Form f(_("Rename Photos"), paths.size() == 1 ? format(_("“%s”"), basename_of(paths[0]).c_str())
                                               : format(_("%zu photos, in the order they were taken"), paths.size()));
  int pattern = f.text(_("New names"), cfg.get("gallery", "rename_pattern", "{date} {n}"), "{date} {n}");
  f.note(_("{name} the name now · {n} a number · {date} and {time} when it was taken · {camera} · {folder}"));
  int start = f.slider(_("First number"), 0, 100, 1, 1, [](double v) { return format("%.0f", v); });
  auto plan = [&](std::vector<RenameItem> &items, std::string *err) {
    return plan_rename(sources, trim(f.text_value(pattern)), int(f.slider_value(start)), path_exists, items, err);
  };
  f.preview(_("Preview"), [&] {
    std::vector<RenameItem> items;
    std::string err;
    std::vector<std::string> lines;
    if (!plan(items, &err)) {
      lines.push_back(err == "slash" ? _("A name cannot contain “/”") : _("Enter a name"));
      return lines;
    }
    if (items.empty()) lines.push_back(_("The names stay as they are"));
    for (size_t i = 0; i < items.size() && i < 4; ++i)
      lines.push_back(basename_of(items[i].from) + "  →  " + basename_of(items[i].to));
    if (items.size() > 4) lines.push_back(format(_("…and %zu more"), items.size() - 4));
    return lines;
  }, 5);
  if (f.run({_("Cancel"), _("Rename")}) != 1) return;
  std::vector<RenameItem> items;
  std::string err;
  if (!plan(items, &err) || items.empty()) return;
  cfg.load();
  cfg.set("gallery", "rename_pattern", trim(f.text_value(pattern)));
  cfg.save();
  size_t done = apply_rename(items, &err);
  // Albums, favorites and the lists follow the new names.
  bool fav = false;
  for (const auto &i : items) {
    if (path_exists(i.from) || !path_exists(i.to)) continue;  // not renamed
    album_path_changed(albums_dir(), i.from, i.to);
    if (favorites_.erase(i.from)) {
      favorites_.insert(i.to);
      fav = true;
    }
    forget_thumbnails(i.from);
    for (auto *list : {&all_, &ext_photos_, &album_photos_})
      for (auto &ph : *list)
        if (ph.path == i.from) {
          ph.path = i.to;
          ph.name = basename_of(i.to);
        }
  }
  if (fav) save_settings();
  grid_->clear_selection();
  reload_albums();
  rebuild_view(true);
  if (done < items.size()) toast(format(_("Renamed %zu photos; %zu could not be renamed (%s)"), done, items.size() - done,
                                        err.c_str()));
  else toast(format(done == 1 ? _("Renamed %zu photo") : _("Renamed %zu photos"), done));
}

// ------------------------------------------------------------------- convert

void GalleryWindow::convert_files(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  auto formats = writable_formats();
  std::vector<std::string> names;
  for (const auto &f : formats) names.push_back(f.name);
  Config cfg;
  cfg.load();
  std::string last = cfg.get("gallery", "convert_format", "jpg");
  int sel = 0;
  for (size_t i = 0; i < formats.size(); ++i)
    if (last == formats[i].ext) sel = int(i);
  Form f(_("Convert Photos"), paths.size() == 1 ? format(_("“%s”"), basename_of(paths[0]).c_str())
                                                : format(_("%zu photos"), paths.size()));
  int fmt = f.choice(_("Format"), names, sel);
  int quality = f.slider(_("Quality"), 30, 100, 1, cfg.get_int("gallery", "convert_quality", 90),
                         [](double v) { return format("%.0f", v); });
  int size = f.choice(_("Size"), {_("As it is"), "3840 px", "2048 px", "1280 px", "640 px"}, 0);
  int dest = f.choice(_("Save the copies"), {_("Next to the photos"), _("In a folder…")}, 0);
  int exif = f.check(_("Keep the camera details (JPEG)"), true);
  f.note(_("The photos themselves do not change: converted copies are saved beside them or in the folder you choose."));
  f.on_change([&](Form &form) {
    photo::Format x = formats[size_t(form.choice_value(fmt))].format;
    form.set_enabled(quality, x == photo::Format::Jpeg || x == photo::Format::Webp);
    form.set_enabled(exif, x == photo::Format::Jpeg);
  });
  if (f.run({_("Cancel"), _("Convert")}) != 1) return;
  const OutputFormat out = formats[size_t(f.choice_value(fmt))];
  int q = int(f.slider_value(quality)), edge = std::vector<int>{0, 3840, 2048, 1280, 640}[size_t(f.choice_value(size))];
  bool keep = f.check_value(exif);
  std::string folder;
  if (f.choice_value(dest) == 1) {
    folder = choose_folder_for(_("Convert to Folder"), pictures_);
    if (folder.empty()) return;
  }
  cfg.load();
  cfg.set("gallery", "convert_format", out.ext);
  cfg.set("gallery", "convert_quality", std::to_string(q));
  cfg.save();
  std::vector<std::function<bool(std::string *)>> steps;
  std::vector<std::string> planned;
  size_t same = 0;
  for (const auto &p : paths) {
    std::string dir = folder.empty() ? p.substr(0, p.rfind('/')) : folder;
    if (folder.empty() && edge == 0 && photo::sniff_format(p) == out.format) {
      ++same;  // a copy of the same photo next to it is no use
      continue;
    }
    std::string d = unique_destination(dir, basename_of(p), path_exists, planned, out.ext);
    planned.push_back(d);
    photo::Format ff = out.format;
    steps.push_back([p, d, ff, q, edge, keep](std::string *err) { return convert_file(p, d, ff, q, edge, keep, err); });
  }
  if (steps.empty()) {
    toast(format(_("The photos are %s files already"), out.name));
    return;
  }
  size_t count = steps.size();
  run_batch(format(_("Converting %zu photos…"), count), std::move(steps),
            [this, planned, same, out](size_t done, size_t failed, const std::string &error) {
              std::vector<GalleryEvent> events;
              for (const auto &p : planned)
                if (path_exists(p)) events.push_back({"added", p});
              file_events(events);
              if (failed) toast(format(_("Converted %zu photos; %zu failed (%s)"), done, failed, error.c_str()));
              else if (same) toast(format(_("Converted %zu photos to %s; %zu were %s already"), done, out.name, same, out.name));
              else toast(format(done == 1 ? _("Converted %zu photo to %s") : _("Converted %zu photos to %s"), done, out.name));
            });
}

// --------------------------------------------------------------------- print

void GalleryWindow::print_files(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  Config cfg;
  cfg.load();
  // Letter in the Americas, A4 elsewhere, unless chosen before.
  std::string lang = env("LC_PAPER", env("LANG"));
  bool letter_default = lang.find("_US") != std::string::npos || lang.find("_CA") != std::string::npos ||
                        lang.find("_MX") != std::string::npos;
  Form f(_("Print Photos"), paths.size() == 1 ? format(_("“%s”"), basename_of(paths[0]).c_str())
                                              : format(_("%zu photos"), paths.size()));
  int layout = f.choice(_("Layout"), {_("One per page"), _("Two per page"), _("Four per page"), _("Contact sheet")},
                        cfg.get_int("gallery", "print_layout", 0));
  int paper = f.choice(_("Paper"), {"A4", _("Letter")}, cfg.get("gallery", "print_paper", letter_default ? "letter" : "a4") == "letter");
  int fit = f.choice(_("Photos"), {_("Whole photo"), _("Fill the space")}, cfg.get_int("gallery", "print_fill", 0));
  int copies = f.slider(_("Copies"), 1, 20, 1, 1, [](double v) { return format("%.0f", v); });
  bool can_print = !find_program("lp").empty();
  if (!can_print) f.note(_("Printing needs the CUPS client (lp); the PDF can still be saved."));
  int r = f.run(can_print ? std::vector<std::string>{_("Cancel"), _("Save as PDF…"), _("Print")}
                          : std::vector<std::string>{_("Cancel"), _("Save as PDF…")});
  if (r == 0) return;
  int lay = f.choice_value(layout), n_copies = int(f.slider_value(copies));
  bool letter = f.choice_value(paper) == 1, fill = f.choice_value(fit) == 1;
  cfg.load();
  cfg.set("gallery", "print_layout", std::to_string(lay));
  cfg.set("gallery", "print_paper", letter ? "letter" : "a4");
  cfg.set("gallery", "print_fill", fill ? "1" : "0");
  cfg.save();
  std::string pdf;
  bool to_printer = r == 2;
  if (!to_printer) {
    Fl_Native_File_Chooser chooser;
    chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    chooser.title(_("Save as PDF"));
    chooser.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM | Fl_Native_File_Chooser::NEW_FOLDER);
    chooser.directory(user_dir("DOCUMENTS").empty() ? home_dir().c_str() : user_dir("DOCUMENTS").c_str());
    chooser.preset_file((paths.size() == 1 ? basename_of(paths[0]) + ".pdf" : std::string(_("Photos")) + ".pdf").c_str());
    chooser.filter("PDF\t*.pdf");
    if (chooser.show() != 0 || !chooser.filename()) return;
    pdf = chooser.filename();
    if (!ends_with(lower(pdf), ".pdf")) pdf += ".pdf";
  } else {
    pdf = runtime_dir() + "/sde-gallery/print-" + std::to_string(getpid()) + "-" + std::to_string(time(nullptr)) + ".pdf";
    make_dirs(pdf.substr(0, pdf.rfind('/')));
  }
  // One step makes the whole PDF (a page at a time) on the worker thread.
  std::vector<std::function<bool(std::string *)>> steps;
  steps.push_back([paths, pdf, lay, letter, fill](std::string *err) {
    double pw = letter ? 612 : 595.28, ph = letter ? 792 : 841.89, margin = 36;
    int cols = lay == 0 ? 1 : lay == 1 ? 1 : lay == 2 ? 2 : 3;
    int rows = lay == 0 ? 1 : lay == 1 ? 2 : lay == 2 ? 2 : 4;
    double gap = lay == 3 ? 14 : 18, caption = lay == 3 ? 14 : 0;
    double cw = (pw - 2 * margin - (cols - 1) * gap) / cols;
    double ch = (ph - 2 * margin - (rows - 1) * gap) / rows - caption;
    cairo_surface_t *surface = cairo_pdf_surface_create(pdf.c_str(), pw, ph);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      if (err) *err = _("the PDF could not be written");
      cairo_surface_destroy(surface);
      return false;
    }
    cairo_t *cr = cairo_create(surface);
    int per = cols * rows;
    for (size_t i = 0; i < paths.size(); ++i) {
      int slot = int(i % size_t(per));
      if (slot == 0 && i > 0) cairo_show_page(cr);
      double x = margin + (slot % cols) * (cw + gap), y = margin + (slot / cols) * (ch + caption + gap);
      cairo_surface_t *img = decode_image(paths[i], print_pixels(cw, ch, 220));
      if (img) {
        double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        // Landscape photos on portrait spaces are turned (and the other way).
        bool turn = lay < 2 && ((iw > ih * 1.1 && ch > cw * 1.1) || (ih > iw * 1.1 && cw > ch * 1.1));
        double sw = turn ? ih : iw, sh = turn ? iw : ih;
        double k = fill ? std::max(cw / sw, ch / sh) : std::min(cw / sw, ch / sh);
        cairo_save(cr);
        cairo_rectangle(cr, x, y, cw, ch);
        cairo_clip(cr);
        cairo_translate(cr, x + cw / 2, y + ch / 2);
        if (turn) cairo_rotate(cr, M_PI / 2);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, img, -iw / 2, -ih / 2);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(img);
      }
      if (caption > 0) {
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 7.5);
        std::string name = basename_of(paths[i]);
        cairo_text_extents_t te;
        cairo_text_extents(cr, name.c_str(), &te);
        while (te.x_advance > cw && name.size() > 4) {
          name = name.substr(0, name.size() - 4) + "…";
          cairo_text_extents(cr, name.c_str(), &te);
        }
        cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
        cairo_move_to(cr, x + (cw - te.x_advance) / 2, y + ch + 10);
        cairo_show_text(cr, name.c_str());
      }
    }
    cairo_show_page(cr);
    cairo_destroy(cr);
    cairo_surface_finish(surface);
    bool ok = cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(surface);
    if (!ok && err) *err = _("the PDF could not be written");
    return ok;
  });
  std::string title = paths.size() == 1 ? basename_of(paths[0]) : format(_("%zu photos"), paths.size());
  run_batch(to_printer ? _("Preparing the pages…") : _("Saving the PDF…"), std::move(steps),
            [this, pdf, to_printer, n_copies, letter, title](size_t done, size_t, const std::string &error) {
              if (!done) {
                toast(format(_("The pages could not be made: %s"), error.c_str()));
                return;
              }
              if (!to_printer) {
                toast(format(_("Saved “%s”"), basename_of(pdf).c_str()));
                return;
              }
              std::vector<std::string> argv{"lp", "-n", std::to_string(n_copies), "-t", title, "-o",
                                            letter ? "media=Letter" : "media=A4", "--", pdf};
              run_async(argv, [this, pdf](const std::string &, int status) {
                unlink(pdf.c_str());
                if (instance_ != this) return;
                toast(status == 0 ? _("Sent to the printer") : _("Printing failed; check the printer in Settings"));
              });
            });
}

// --------------------------------------------------------- cameras and phones

void GalleryWindow::import_camera() {
  if (importing_) {
    toast(_("An import is already running"));
    return;
  }
  refresh_devices();
  for (const auto &d : devices_)
    if (d.camera()) {
      import_from(d);
      return;
    }
  auto nothing = [this] {
    ask(_("Import from Camera or Phone"),
        _("No camera or phone was found.\nConnect it with a USB cable, unlock the phone and choose “File transfer” "
          "(or “PTP”) on it, then try again."),
        {_("Close")});
  };
  auto try_gphoto2 = [this, nothing] {
    if (find_program("gphoto2").empty()) {
      nothing();
      return;
    }
    run_async({"gphoto2", "--auto-detect"}, [this, nothing](const std::string &out, int) {
      if (instance_ != this) return;
      auto cams = parse_gphoto2_cameras(out);
      if (cams.empty()) {
        nothing();
        return;
      }
      std::string dest = import_root(pictures_);
      if (ask(_("Import from Camera"),
              format(_("Copy all photos from “%s” to %s?\nPhotos that were imported before are skipped."),
                     cams[0].first.c_str(), dest.c_str()),
              {_("Cancel"), _("Import")}) != 1)
        return;
      make_dirs(dest);
      importing_ = true;
      toast(format(_("Importing from “%s”…"), cams[0].first.c_str()));
      std::string model = cams[0].first;
      run_async({"gphoto2", "--port", cams[0].second, "--get-all-files", "--skip-existing", "--filename",
                 dest + "/%Y-%m-%d/%f.%C"},
                [this, model](const std::string &, int status) {
                  if (instance_ != this) return;
                  importing_ = false;
                  toast(status == 0 ? format(_("Imported the photos of “%s”"), model.c_str())
                                    : format(_("The import from “%s” stopped with an error"), model.c_str()));
                  start_scan();
                });
    });
  };
  if (find_program("gio").empty()) {
    try_gphoto2();
    return;
  }
  // A phone or camera that GIO knows but has not mounted: mount it.
  run_async({"gio", "mount", "-li"}, [this, try_gphoto2](const std::string &out, int) {
    if (instance_ != this) return;
    for (const auto &v : parse_gio_volumes(out)) {
      if (v.mounted) continue;
      toast(format(_("Connecting to “%s”…"), v.name.c_str()));
      std::string name = v.name;
      run_async({"gio", "mount", v.root}, [this, name](const std::string &, int status) {
        if (instance_ != this) return;
        refresh_devices();
        for (const auto &d : devices_)
          if (d.camera()) {
            import_from(d);
            return;
          }
        toast(status == 0 ? format(_("“%s” has no photos to import"), name.c_str())
                          : format(_("“%s” could not be opened; unlock it and allow access"), name.c_str()));
      });
      return;
    }
    try_gphoto2();
  });
}

// ----------------------------------------------------------------- duplicates

void GalleryWindow::find_duplicates() {
  if (finding_duplicates_) {
    toast(_("Looking for duplicates already…"));
    return;
  }
  if (all_.size() < 2) {
    toast(_("There are no photos to compare"));
    return;
  }
  int r = ask(_("Find Duplicates"),
              format(_("Compare the %zu photos of your library?\nIdentical copies are found, and with “Similar Too” "
                       "also photos that were resized or saved again."),
                     all_.size()),
              {_("Cancel"), _("Identical Only"), _("Similar Too")});
  if (r == 0) return;
  bool similar = r == 2;
  finding_duplicates_ = true;
  struct Job {
    GalleryWindow *self;
    std::vector<Photo> photos;
    bool similar;
    std::vector<std::vector<std::string>> groups;
    std::vector<std::string> labels;
    std::atomic<size_t> done{0};
  };
  auto job = std::make_shared<Job>();
  job->self = this;
  job->photos = all_;
  job->similar = similar;
  toast(_("Looking for duplicates…"));
  // Progress in the toast while the worker runs.
  struct Tick {
    static void cb(void *p) {
      auto *job = static_cast<std::shared_ptr<Job> *>(p);
      GalleryWindow *self = (*job)->self;
      if (instance_ != self || !self->finding_duplicates_) {
        delete job;
        return;
      }
      self->toast(format(_("Comparing photos… %zu of %zu"), (*job)->done.load(), (*job)->photos.size()));
      Fl::repeat_timeout(0.5, cb, p);
    }
  };
  Fl::add_timeout(0.5, Tick::cb, new std::shared_ptr<Job>(job));
  std::thread([job] {
    const auto &ph = job->photos;
    std::vector<long long> sizes;
    for (const auto &p : ph) sizes.push_back(p.size);
    auto identical = identical_groups(sizes, [&](size_t i) { return file_sha256(ph[i].path); });
    std::vector<std::vector<size_t>> similar;
    if (job->similar) {
      std::vector<uint64_t> hashes(ph.size(), 0);
      std::vector<double> aspects(ph.size(), 0);
      for (size_t i = 0; i < ph.size(); ++i) {
        int w = 0, h = 0;
        cairo_surface_t *s = load_cached_thumbnail(ph[i].path, ph[i].mtime, false, &w, &h);
        if (!s) s = decode_image(ph[i].path, 96, &w, &h);
        if (s) {
          hashes[i] = difference_hash(s);
          if (w <= 0 || h <= 0) {
            w = cairo_image_surface_get_width(s);
            h = cairo_image_surface_get_height(s);
          }
          aspects[i] = h > 0 ? double(w) / h : 0;
          cairo_surface_destroy(s);
        }
        job->done = i + 1;
      }
      similar = similar_groups(hashes, aspects);
    }
    job->done = ph.size();
    for (const auto &g : merge_groups(ph.size(), identical, similar)) {
      // The copy to keep first: the largest file, then the oldest.
      std::vector<size_t> order = g;
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (ph[a].size != ph[b].size) return ph[a].size > ph[b].size;
        return ph[a].mtime < ph[b].mtime;
      });
      std::vector<std::string> paths;
      for (size_t i : order) paths.push_back(ph[i].path);
      bool all_identical = false;
      for (const auto &ig : identical)
        if (ig.size() == g.size() && std::is_permutation(ig.begin(), ig.end(), g.begin())) all_identical = true;
      job->groups.push_back(paths);
      job->labels.push_back(all_identical ? format(_("%zu identical copies"), paths.size())
                                          : format(_("%zu similar photos"), paths.size()));
    }
    Fl::awake([](void *p) {
      auto *holder = static_cast<std::shared_ptr<Job> *>(p);
      auto job = *holder;
      delete holder;
      GalleryWindow *self = job->self;
      if (instance_ != self) return;
      self->finding_duplicates_ = false;
      self->duplicates_ = job->groups;
      self->duplicate_labels_ = job->labels;
      self->sidebar_->redraw();
      if (self->duplicates_.empty()) {
        self->toast(_("No duplicates found"));
        if (self->view_kind_.kind == ViewKind::Duplicates) self->set_view(View{});
        return;
      }
      size_t n = 0;
      for (const auto &g : self->duplicates_) n += g.size();
      self->set_view(View{ViewKind::Duplicates, ""});
      self->toast(format(_("%zu photos in %zu groups · the first of each group is the one to keep"), n,
                         self->duplicates_.size()));
    }, new std::shared_ptr<Job>(job));
  }).detach();
}

std::vector<Photo> GalleryWindow::duplicates_view(std::vector<Group> &groups) {
  groups.clear();
  std::vector<Photo> out;
  for (size_t g = 0; g < duplicates_.size(); ++g) {
    auto photos = photos_for_paths(duplicates_[g]);
    if (photos.size() < 2) continue;
    Group grp;
    grp.key = "dup" + std::to_string(g);
    grp.label = g < duplicate_labels_.size() ? duplicate_labels_[g] : std::string();
    grp.first = out.size();
    grp.count = photos.size();
    groups.push_back(grp);
    out.insert(out.end(), photos.begin(), photos.end());
  }
  return out;
}

void GalleryWindow::select_extra_copies() {
  if (view_kind_.kind != ViewKind::Duplicates) {
    toast(_("Find duplicates first (Tools menu)"));
    return;
  }
  grid_->clear_selection();
  size_t n = 0;
  for (const auto &g : groups_)
    for (size_t i = 1; i < g.count; ++i, ++n) grid_->select_index(int(g.first + i));
  Fl::focus(grid_);
  layout();
  toast(format(_("%zu extra copies selected · Move to Trash removes them"), n));
}

// -------------------------------------------------------------------- places

void GalleryWindow::start_locating() {
  if (locating_) return;
  locating_ = true;
  if (locate_thread_.joinable()) locate_thread_.join();
  std::vector<Photo> photos = all_;
  locate_thread_ = std::thread([this, photos] {
    auto cache = parse_location_cache([&] {
      std::string text;
      read_file(location_cache_path(), text);
      return text;
    }());
    bool dirty = false;
    std::map<std::string, CachedLocation> now;
    for (const auto &p : photos) {
      auto it = cache.find(p.path);
      if (it != cache.end() && it->second.mtime == p.mtime) {
        now[p.path] = it->second;
        continue;
      }
      CachedLocation c;
      c.mtime = p.mtime;
      PhotoInfo info;
      if (read_photo_info(p.path, info) && info.has_gps && (info.latitude != 0 || info.longitude != 0)) {
        c.has_gps = true;
        c.lat = info.latitude;
        c.lon = info.longitude;
      }
      now[p.path] = c;
      dirty = true;
    }
    if (dirty || now.size() != cache.size()) {
      make_dirs(location_cache_path().substr(0, location_cache_path().rfind('/')));
      write_file_atomic(location_cache_path(), format_location_cache(now));
    }
    {
      std::lock_guard<std::mutex> lock(scan_mu_);
      locations_.clear();
      no_location_.clear();
      for (const auto &kv : now) {
        if (kv.second.has_gps) locations_[kv.first] = {kv.second.mtime, {kv.second.lat, kv.second.lon}};
        else no_location_.insert(kv.first);
      }
    }
    Fl::awake(locate_awake_cb, this);
  });
}

void GalleryWindow::locate_awake_cb(void *p) {
  auto *self = static_cast<GalleryWindow *>(p);
  if (instance_ != self) return;
  self->locating_ = false;
  self->located_ = true;
  {
    std::lock_guard<std::mutex> lock(self->scan_mu_);
    self->places_count_ = self->locations_.size();
  }
  self->sidebar_->redraw();
  if (self->view_kind_.kind == ViewKind::Places) self->rebuild_view(true);
}

std::vector<Photo> GalleryWindow::places_view(std::vector<Group> &groups) {
  groups.clear();
  std::vector<Photo> out;
  if (!located_) {
    start_locating();
    return out;
  }
  if (gazetteer_.empty()) {
    std::string text;
    if (read_file(gazetteer_path(), text)) gazetteer_ = parse_gazetteer(text);
  }
  std::string query = search_ ? search_->value() : "";
  std::vector<const Photo *> located;
  std::vector<LocatedPhoto> spots;
  {
    std::lock_guard<std::mutex> lock(scan_mu_);
    for (const auto &p : all_) {
      auto it = locations_.find(p.path);
      if (it == locations_.end() || it->second.first != p.mtime) continue;
      LocatedPhoto lp;
      lp.index = located.size();
      lp.lat = it->second.second.first;
      lp.lon = it->second.second.second;
      lp.time = p.mtime;
      located.push_back(&p);
      spots.push_back(lp);
    }
  }
  for (const auto &g : group_by_place(spots, gazetteer_)) {
    // The search finds places by name, and photos by name.
    bool place_match = matches_search(g.label, query);
    Group grp;
    grp.key = g.label;
    grp.label = g.label;
    grp.first = out.size();
    for (size_t i : g.photos) {
      const Photo &p = *located[spots[i].index];
      if (place_match || matches_search(p.name, query)) out.push_back(p);
    }
    grp.count = out.size() - grp.first;
    if (grp.count) groups.push_back(grp);
  }
  return out;
}

}  // namespace gallery
