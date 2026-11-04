// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The Home tab, Xanara's start page. Without a file Xanara starts here
// (0.2.0 Alpha 4: like the start page of an office suite, everything is a
// click away without opening a PDF first): open a PDF; make a new one,
// blank, from images or by merging PDFs; the tools that start from a file
// (merge, split, compress, OCR, protect ...), which ask for the PDF and
// then run on it; and the recently opened documents as a grid of first
// page thumbnails, rendered in the background one per idle moment. PDFs
// dropped on the page open, dropped images become a new PDF.
#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "util.h"
#include "xanara.h"

using namespace sde;

namespace xanara {

namespace {

// The first row: open a PDF or make a new one.
struct StartAction {
  const char *id;
  icon::Kind glyph;
  const char *title, *detail;
};

const StartAction kStart[] = {
    {"open", icon::Open, N_("Open…"), N_("A PDF from your files · Ctrl+O")},
    {"blank", icon::Plus, N_("Blank PDF"), N_("A new empty document · Ctrl+N")},
    {"images", icon::ImagesToPdf, N_("From Images"), N_("Scans and photos as pages")},
    {"merge", icon::Merge, N_("Merge PDFs"), N_("Several files into one")},
};
constexpr int kStartCount = int(sizeof(kStart) / sizeof(kStart[0]));

// The tools that start from a file: each asks for the PDF it works on.
struct HomeTool {
  Act act;
  icon::Kind glyph;
  const char *title, *detail;
};

// (Merging is in the first row, with the other ways to a new PDF.)
const HomeTool kTools[] = {
    {Act::PageSplit, icon::Split, N_("Split"), N_("Into files of a few pages")},
    {Act::PageExtract, icon::Extract, N_("Extract Pages"), N_("Some pages as a new PDF")},
    {Act::Compress, icon::Compress, N_("Compress"), N_("Make a file smaller")},
    {Act::Ocr, icon::Ocr, N_("Recognize Text"), N_("Searchable scans (OCR)")},
    {Act::EditContent, icon::EditText, N_("Edit Text"), N_("Change text and pictures")},
    {Act::FillSign, icon::Signature, N_("Fill & Sign"), N_("Fill in a form, sign it")},
    {Act::Encrypt, icon::Lock, N_("Protect"), N_("Add a password")},
    {Act::Decrypt, icon::Unlock, N_("Remove Password"), N_("Unlock a protected PDF")},
    {Act::Redact, icon::Redact, N_("Redact"), N_("Black out text for good")},
    {Act::ToPng, icon::ExportImage, N_("To Images"), N_("Pages as PNG pictures")},
    {Act::Watermark, icon::Watermark, N_("Watermark"), N_("Text across every page")},
    {Act::Compare, icon::Compare, N_("Compare"), N_("Two PDFs side by side")},
};
constexpr int kHomeToolCount = int(sizeof(kTools) / sizeof(kTools[0]));

std::string pretty_dir(const std::string &path) {
  std::string dir = path.substr(0, path.rfind('/'));
  std::string home = home_dir();
  if (starts_with(dir, home)) dir = "~" + dir.substr(home.size());
  return dir;
}

int pad_for(int w) { return std::min(S(40), std::max(S(16), w / 30)); }

}  // namespace

HomeView::HomeView(XanaraWindow *app, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), app_(app) {}

HomeView::~HomeView() {
  Fl::remove_timeout(thumb_cb, this);
  for (auto &t : thumbs_)
    if (t.second) cairo_surface_destroy(t.second);
}

void HomeView::refresh() {
  build();
  redraw();
}

void HomeView::build() {
  cards_.clear();
  int pad = pad_for(w());
  int x0 = x() + pad, y0 = y() + pad - int(scroll_);
  int avail = w() - 2 * pad;
  int gap = S(14);
  // Open and new: four cards in a row, fewer per row on narrow windows.
  int cols = std::clamp((avail + gap) / (S(210) + gap), 1, kStartCount);
  int cw = (avail - (cols - 1) * gap) / cols, ch = S(76);
  int cy = y0 + S(84);
  for (int k = 0; k < kStartCount; ++k)
    cards_.push_back(Card{Start, x0 + (k % cols) * (cw + gap), cy + (k / cols) * (ch + S(12)), cw, ch, k});
  cy += ((kStartCount + cols - 1) / cols) * (ch + S(12)) - S(12);
  // Tools.
  tools_y_ = cy + S(30);
  cy = tools_y_ + S(58);
  int tgap = S(12);
  // As many columns as fit, and rows of the same length (12 tools: 6, 4,
  // 3 or 2 a row).
  int tcols = std::clamp((avail + tgap) / (S(200) + tgap), 2, 6);
  while (tcols > 2 && kHomeToolCount % tcols) --tcols;
  int tw = (avail - (tcols - 1) * tgap) / tcols, th = S(58);
  for (int k = 0; k < kHomeToolCount; ++k)
    cards_.push_back(Card{Tool, x0 + (k % tcols) * (tw + tgap), cy + (k / tcols) * (th + tgap), tw, th, k});
  cy += ((kHomeToolCount + tcols - 1) / tcols) * (th + tgap) - tgap;
  // Recent documents.
  recent_y_ = cy + S(30);
  cy = recent_y_ + S(48);
  auto rec = app_->recent();
  int rw = S(168), rh = S(246), rgap = S(18);
  int rcols = std::max(1, (avail + rgap) / (rw + rgap));
  for (size_t k = 0; k < rec.size(); ++k) {
    int col = int(k) % rcols, row = int(k) / rcols;
    cards_.push_back(Card{Recent, x0 + col * (rw + rgap), cy + row * (rh + rgap), rw, rh, int(k)});
    if (!thumbs_.count(rec[k].path) &&
        std::find(pending_.begin(), pending_.end(), rec[k].path) == pending_.end())
      pending_.push_back(rec[k].path);
  }
  int rows = rec.empty() ? 0 : (int(rec.size()) + rcols - 1) / rcols;
  content_h_ = (cy - y0) + (rows ? rows * (rh + rgap) : S(40)) + 2 * pad;
  scroll_ = std::clamp(scroll_, 0.0, double(std::max(0, content_h_ - h())));
  if (!pending_.empty() && !Fl::has_timeout(thumb_cb, this)) Fl::add_timeout(0.02, thumb_cb, this);
}

void HomeView::thumb_cb(void *p) {
  auto *self = static_cast<HomeView *>(p);
  if (self->pending_.empty()) return;
  std::string path = self->pending_.front();
  self->pending_.erase(self->pending_.begin());
  cairo_surface_t *s = nullptr;
  std::string error;
  bool enc = false;
  if (auto d = Document::open(path, "", error, enc)) {
    RenderParams rp;
    int tw = S(144);
    rp.scale = tw / std::max(1.0, d->width(0));
    int w, h;
    page_pixels(d->width(0), d->height(0), rp, w, h);
    s = d->render(0, rp, 0, 0, w, std::min(h, S(190)));
  }
  self->thumbs_[path] = s;
  self->redraw();
  if (!self->pending_.empty()) Fl::repeat_timeout(0.01, thumb_cb, p);
}

void HomeView::draw() {
  const Theme &t = app_->theme();
  build();
  auto rec = app_->recent();
  int pad = pad_for(w());
  {
    Painter p(window()->w(), window()->h());
    cairo_t *cr = p.cr();
    cairo_save(cr);
    cairo_rectangle(cr, x(), y(), w(), h());
    cairo_clip(cr);
    p.fill_rect(x(), y(), w(), h(), t.background);
    for (int k = 0; k < int(cards_.size()); ++k) {
      const Card &c = cards_[size_t(k)];
      if (c.y > y() + h() || c.y + c.h < y()) continue;
      bool hov = k == hover_;
      if (c.kind == Start) {
        p.fill_rounded(c.x, c.y, c.w, c.h, S(12), hov ? t.hover.mix(t.surface, 0.3) : t.surface);
        p.stroke_rounded(c.x + 0.5, c.y + 0.5, c.w - 1, c.h - 1, S(12), hov ? t.accent : t.border);
        p.fill_rounded(c.x + S(16), c.y + (c.h - S(44)) / 2.0, S(44), S(44), S(12), t.accent.alpha(0.14));
        icon::draw(p, kStart[c.index].glyph, c.x + S(26), c.y + (c.h - S(24)) / 2.0, S(24), t.accent_text);
        continue;
      }
      if (c.kind == Tool) {
        p.fill_rounded(c.x, c.y, c.w, c.h, S(10), hov ? t.hover.mix(t.surface, 0.3) : t.surface);
        p.stroke_rounded(c.x + 0.5, c.y + 0.5, c.w - 1, c.h - 1, S(10), hov ? t.accent : t.border.alpha(0.8));
        icon::draw(p, kTools[c.index].glyph, c.x + S(14), c.y + (c.h - S(22)) / 2.0, S(22),
                   hov ? t.accent_text : t.text_dim);
        continue;
      }
      // A recent document.
      if (hov) p.fill_rounded(c.x - S(6), c.y - S(6), c.w + S(12), c.h + S(12), S(12), t.hover);
      int tw = S(144), th = S(190);
      int tx = c.x + (c.w - tw) / 2, ty = c.y + S(4);
      p.shadow(tx, ty, tw, th, S(4), t.shadow.alpha(0.25), S(6));
      p.fill_rect(tx, ty, tw, th, Color::hex(0xFFFFFF));
      const std::string &path = rec[size_t(c.index)].path;
      auto it = thumbs_.find(path);
      if (it != thumbs_.end() && it->second) {
        cairo_save(cr);
        cairo_rectangle(cr, tx, ty, tw, th);
        cairo_clip(cr);
        cairo_set_source_surface(cr, it->second, tx, ty);
        cairo_paint(cr);
        cairo_restore(cr);
      } else {
        icon::draw(p, it == thumbs_.end() ? icon::Document : icon::Lock, tx + (tw - S(40)) / 2.0,
                   ty + (th - S(40)) / 2.0, S(40), Color::hex(0x9AA3AF));
      }
      p.stroke_rounded(tx + 0.5, ty + 0.5, tw - 1, th - 1, S(2), hov ? t.accent : t.border.alpha(0.7), hov ? Sd(2) : 1);
    }
    if (dropping_) {
      // Files dragged over the page: where they go.
      p.fill_rect(x(), y(), w(), h(), t.accent.alpha(t.dark ? 0.10 : 0.06));
      double inset = S(14);
      cairo_save(cr);
      double dash[] = {Sd(8), Sd(6)};
      cairo_set_dash(cr, dash, 2, 0);
      p.set(t.accent);
      cairo_set_line_width(cr, Sd(2));
      p.rounded_rect(x() + inset, y() + inset, w() - 2 * inset, h() - 2 * inset, S(16));
      cairo_stroke(cr);
      cairo_restore(cr);
    }
    cairo_restore(cr);
  }
  fl_push_clip(x(), y(), w(), h());
  int y0 = y() + pad - int(scroll_);
  text_font(t.font_size + S(8), true);
  draw_text(_("Welcome to Xanara"), x() + pad, y0, w() - 2 * pad, S(36), t.text);
  text_font(t.font_size);
  draw_text(_("Open a PDF, start a new one or choose a tool. You can also drop PDFs anywhere in this window."),
            x() + pad, y0 + S(40), w() - 2 * pad, S(24), t.text_dim);
  for (const auto &c : cards_) {
    if (c.y > y() + h() || c.y + c.h < y()) continue;
    if (c.kind == Start) {
      const StartAction &a = kStart[c.index];
      text_font(t.font_size + S(1), true);
      draw_text(_(a.title), c.x + S(74), c.y + S(14), c.w - S(84), S(26), t.text);
      text_font(t.font_size - S(1));
      draw_text(_(a.detail), c.x + S(74), c.y + S(40), c.w - S(84), S(22), t.text_dim);
    } else if (c.kind == Tool) {
      const HomeTool &a = kTools[c.index];
      text_font(t.font_size, true);
      draw_text(_(a.title), c.x + S(48), c.y + S(9), c.w - S(56), S(22), t.text);
      text_font(t.font_size - S(2));
      draw_text(_(a.detail), c.x + S(48), c.y + S(30), c.w - S(56), S(20), t.text_dim);
    } else {
      const auto &e = rec[size_t(c.index)];
      text_font(t.font_size - S(1), true);
      draw_text(basename_of(e.path), c.x, c.y + S(202), c.w, S(20), t.text, 1);
      text_font(t.font_size - S(2));
      draw_text(format(_("Page %d · %s"), e.page + 1, pretty_dir(e.path).c_str()), c.x, c.y + S(222), c.w, S(18),
                t.text_dim, 1);
    }
  }
  text_font(t.font_size + S(2), true);
  draw_text(_("Tools"), x() + pad, tools_y_, S(400), S(28), t.text);
  text_font(t.font_size - S(1));
  draw_text(_("Choose a tool, then the PDF it works on"), x() + pad, tools_y_ + S(26), w() - 2 * pad, S(22),
            t.text_dim);
  text_font(t.font_size + S(2), true);
  draw_text(_("Recent"), x() + pad, recent_y_, S(400), S(28), t.text);
  if (rec.empty()) {
    text_font(t.font_size);
    draw_text(_("The PDFs you open appear here."), x() + pad, recent_y_ + S(34), w() - 2 * pad, S(26), t.text_dim);
  }
  if (dropping_) {
    std::string text = _("Drop PDFs to open them; images become a new PDF");
    text_font(t.font_size + S(2), true);
    int tw = std::min(w() - S(60), text_width(text) + S(48)), th = S(48);
    int tx = x() + (w() - tw) / 2, ty = y() + h() - th - S(36);
    {
      Painter p(window()->w(), window()->h());
      p.shadow(tx, ty, tw, th, th / 2.0, t.shadow, S(8));
      p.fill_rounded(tx, ty, tw, th, th / 2.0, t.accent);
    }
    draw_text(text, tx + S(24), ty, tw - S(48), th, Color::hex(0xFFFFFF), 1);
  }
  fl_pop_clip();
}

int HomeView::card_at(int mx, int my) const {
  for (int k = 0; k < int(cards_.size()); ++k) {
    const Card &c = cards_[size_t(k)];
    if (mx >= c.x && mx < c.x + c.w && my >= c.y && my < c.y + c.h && my >= y() && my < y() + h()) return k;
  }
  return -1;
}

void HomeView::activate(const Card &c) {
  if (c.kind == Start) {
    std::string id = kStart[c.index].id;
    if (id == "open") app_->open_dialog();
    else if (id == "blank") app_->new_blank();
    else if (id == "images") app_->images_to_pdf_dialog();
    else app_->merge_files();
  } else if (c.kind == Tool) {
    app_->start_tool(kTools[c.index].act, _(kTools[c.index].title));
  } else {
    auto rec = app_->recent();
    if (size_t(c.index) < rec.size()) app_->open_path(rec[size_t(c.index)].path, rec[size_t(c.index)].page);
  }
}

std::string HomeView::qa() {
  if (!visible()) return "";
  build();
  std::string out;
  for (const auto &c : cards_) {
    if (c.y + c.h <= y() || c.y >= y() + h()) continue;
    std::string id = c.kind == Start  ? std::string("start:") + kStart[c.index].id
                     : c.kind == Tool ? std::string("tool:") + (tool_spec(kTools[c.index].act)
                                                                    ? tool_spec(kTools[c.index].act)->id
                                                                    : "?")
                                      : "recent:" + std::to_string(c.index);
    out += format("home %s %d %d %d %d\n", id.c_str(), c.x, c.y, c.w, c.h);
  }
  out += format("dropping %d\n", dropping_ ? 1 : 0);
  return out;
}

int HomeView::handle(int event) {
  int mx = Fl::event_x(), my = Fl::event_y();
  switch (event) {
    case FL_ENTER:
    case FL_MOVE: {
      int k = card_at(mx, my);
      if (k != hover_) {
        hover_ = k;
        // The tools say what they do (their toolbar tooltip).
        std::string tip;
        if (k >= 0 && cards_[size_t(k)].kind == Tool)
          if (const ToolSpec *spec = tool_spec(kTools[cards_[size_t(k)].index].act)) tip = _(spec->tip);
        if (tip != tip_) {
          tip_ = tip;
          Fl_Tooltip::enter_area(this, 0, 0, 0, 0, nullptr);
          if (!tip_.empty()) {
            const Card &c = cards_[size_t(k)];
            Fl_Tooltip::enter_area(this, c.x - x(), c.y - y(), c.w, c.h, tip_.c_str());
          }
        }
        redraw();
      }
      window()->cursor(k >= 0 ? FL_CURSOR_HAND : FL_CURSOR_DEFAULT);
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      tip_.clear();
      window()->cursor(FL_CURSOR_DEFAULT);
      redraw();
      return 1;
    case FL_MOUSEWHEEL:
      scroll_ = std::clamp(scroll_ + Fl::event_dy() * S(60), 0.0, double(std::max(0, content_h_ - h())));
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH: {
      int k = card_at(mx, my);
      if (k < 0) return 1;
      Card c = cards_[size_t(k)];
      auto rec = app_->recent();
      if (Fl::event_button() == FL_RIGHT_MOUSE && c.kind == Recent && size_t(c.index) < rec.size()) {
        Fl_Menu_Item items[3];
        memset(items, 0, sizeof items);
        items[0].text = _("Open");
        items[1].text = _("Remove from Recent");
        for (int i = 0; i < 2; ++i) {
          items[i].labelsize(app_->theme().font_size);
          items[i].labelcolor(app_->theme().text.fl());
        }
        const Fl_Menu_Item *m = items->popup(mx, my);
        if (m == items) app_->open_path(rec[size_t(c.index)].path);
        else if (m == items + 1) app_->forget_recent(rec[size_t(c.index)].path);
        return 1;
      }
      if (Fl::event_button() != FL_LEFT_MOUSE) return 1;
      window()->cursor(FL_CURSOR_DEFAULT);
      hover_ = -1;
      activate(c);
      return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG:
      if (!dropping_) {
        dropping_ = true;
        redraw();
      }
      return 1;
    case FL_DND_LEAVE:
    case FL_DND_RELEASE:
      if (dropping_) {
        dropping_ = false;
        redraw();
      }
      return 1;
    case FL_PASTE: {
      dropping_ = false;
      redraw();
      std::string text(Fl::event_text(), size_t(Fl::event_length()));
      app_->drop_files(split(text, '\n'));
      return 1;
    }
  }
  return Fl_Widget::handle(event);
}

}  // namespace xanara
