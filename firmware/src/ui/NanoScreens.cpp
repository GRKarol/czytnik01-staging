#include "ui/NanoScreens.h"

#include <math.h>

#include <algorithm>

namespace nano {

namespace {

// Head, focus letter and tail of a sample word, split near where the reader
// puts its focus letter.
void splitSampleWord(const String &word, String &head, String &focus, String &tail) {
  const size_t n = word.length();
  const size_t at = n <= 1 ? 0 : (n <= 5 ? 1 : (n <= 9 ? 2 : 3));
  head = word.substring(0, at);
  focus = word.substring(at, at + 1);
  tail = word.substring(at + 1);
}

Layout gLayout;

int right(const Rect &rect) { return rect.x + rect.w; }
int bottom(const Rect &rect) { return rect.y + rect.h; }

void addTarget(Sink &sink, const Rect &rect, int id) {
  if (id != kNoTarget) {
    sink.target(rect, id);
  }
}

void iconButton(DisplayManager &d, Sink &sink, const Rect &rect, int id, Icon icon, bool enabled = true) {
  d.nanoButton(rect, "", enabled, icon, 1, "", "", sink.pressed(id));
  if (enabled) {
    addTarget(sink, rect, id);
  }
}

constexpr int kHelpButtonW = 34;

// A book cover: the picture from the Flower app, or the book's colour with
// its initials. The thin binding line near the left edge goes over both, so
// a picture sits in exactly the frame of the default cover.
void paintCover(DisplayManager &d, const Rect &cover, int radius, int bindingX, int bindingW, uint16_t color,
                const String &initials, uint8_t size, const NanoImage &image) {
  if (image.valid()) {
    d.nanoImage(cover, radius, image);
  } else {
    d.nanoFillRoundRect(cover.x, cover.y, cover.w, cover.h, radius, color);
  }
  d.nanoFillRect(cover.x + bindingX, cover.y, bindingW, cover.h, d.nanoBlend(Role::Background, 90));
  if (!image.valid()) {
    const int textX = bindingX + bindingW;
    d.nanoText(Rect(cover.x + textX, cover.y, cover.w - textX, cover.h), initials, size, 0xFFFF, Align::Center);
  }
}

// Cuts the "?" button off the right of `rect` (when the item has one) and
// paints it; returns what is left for the tile itself.
Rect withHelpButton(DisplayManager &d, Sink &sink, const Rect &rect, int helpId) {
  if (helpId == kNoTarget || rect.w < 120) {
    return rect;
  }
  const Rect help(rect.x + rect.w - kHelpButtonW + 4, rect.y, kHelpButtonW - 4, rect.h);
  const int r = std::min<int>(13, (std::min(help.w, help.h) - 2) / 2);
  const int cx = help.x + help.w / 2;
  const int cy = help.y + help.h / 2;
  d.nanoFillCircle(cx, cy, r, d.nanoColor(sink.pressed(helpId) ? Role::SurfaceActive : Role::SurfaceMuted));
  d.nanoText(Rect(cx - r, cy - r, r * 2 + 1, r * 2 + 1), "?", 2, d.nanoColor(Role::Muted), Align::Center);
  addTarget(sink, help, helpId);
  return Rect(rect.x, rect.y, rect.w - kHelpButtonW, rect.h);
}

}  // namespace

Layout &layout() { return gLayout; }

int railWidthFor(const std::vector<String> &labels) {
  int widest = 0;
  for (const String &label : labels) {
    widest = std::max(widest, DisplayManager::nanoTextWidth(label, 2));
  }
  // nanoTab(): 6px pill inset, 10px + 22px icon + 8px, text, 8px + 6px.
  return std::max(136, std::min(196, widest + 62));
}

Rect railRect() {
  const int width = gLayout.compact ? kCompactRailW : gLayout.railWidth;
  return Rect(gLayout.railRight ? kScreenW - width : 0, 0, width, kScreenH);
}

Rect tabContent() {
  const Rect rail = railRect();
  if (gLayout.railRight) {
    return Rect(kMargin, 8, rail.x - kMargin * 2, kScreenH - 16);
  }
  const int x = rail.w + kMargin;
  return Rect(x, 8, kScreenW - x - kMargin, kScreenH - 16);
}

Rect fullContent() { return Rect(kMargin, 8, kScreenW - kMargin * 2, kScreenH - 16); }

// ─── Rail ───────────────────────────────────────────────────────────────────

void paintRail(DisplayManager &d, Sink &sink, const std::vector<RailTab> &tabs) {
  const Rect rail = railRect();
  d.nanoRailBackground(rail);
  if (tabs.empty()) {
    return;
  }
  const int top = 4;
  const int available = kScreenH - kRailFooterH - top;
  const int tabHeight = available / static_cast<int>(tabs.size());
  int y = top;
  for (size_t i = 0; i < tabs.size(); ++i) {
    const Rect rect(rail.x, y, rail.w, tabHeight);
    d.nanoTab(rect, gLayout.compact ? String() : tabs[i].label, tabs[i].active, tabs[i].icon,
              sink.pressed(tabs[i].id), tabs[i].badge, gLayout.railRight);
    addTarget(sink, rect, tabs[i].id);
    y += tabHeight;
  }
  d.nanoBatteryInline(Rect(rail.x, kScreenH - kRailFooterH, rail.w, kRailFooterH - 2), gLayout.compact);
}

// ─── Header ─────────────────────────────────────────────────────────────────

int paintHeader(DisplayManager &d, Sink &sink, const Rect &area, const Header &header) {
  int x = area.x;
  int limit = right(area);
  const int y = area.y;
  if (header.backId != kNoTarget) {
    iconButton(d, sink, Rect(x, y, 48, kHeaderH), header.backId, Icon::ChevronLeft);
    x += 48 + 10;
  }
  if (header.pageCount > 1) {
    const Rect next(limit - 36, y, 36, kHeaderH);
    const Rect label(next.x - 48, y, 48, kHeaderH);
    const Rect prev(label.x - 36, y, 36, kHeaderH);
    iconButton(d, sink, prev, header.prevId, Icon::ChevronLeft, header.page > 0);
    iconButton(d, sink, next, header.nextId, Icon::ChevronRight, header.page + 1 < header.pageCount);
    d.nanoLabel(label,
                String(static_cast<unsigned>(header.page + 1)) + "/" + String(static_cast<unsigned>(header.pageCount)),
                1, Role::Muted, Align::Center);
    limit = prev.x - 10;
  }
  if (header.pillId != kNoTarget && !header.pillLabel.isEmpty()) {
    const int wanted = DisplayManager::nanoTextWidth(header.pillLabel, 1) + 28 + (header.pillIcon == Icon::None ? 0 : 24);
    const int width = std::min(wanted, std::max(60, (limit - x) / 2));
    const Rect pill(limit - width, y + 2, width, kHeaderH - 4);
    d.nanoPill(pill, header.pillLabel, header.pillIcon, sink.pressed(header.pillId), false);
    addTarget(sink, Rect(pill.x, y, pill.w, kHeaderH), header.pillId);
    limit = pill.x - 10;
  }
  if (!header.trailing.isEmpty()) {
    const int width = std::min(DisplayManager::nanoTextWidth(header.trailing, 1), (limit - x) / 3);
    d.nanoLabel(Rect(limit - width, y, width, kHeaderH), header.trailing, 1, Role::Muted, Align::End);
    limit -= width + 10;
  }
  d.nanoLabel(Rect(x, y, std::max(0, limit - x), kHeaderH), header.title, 2, Role::Foreground);
  return y + kHeaderH + 6;
}

// ─── Tile grid ──────────────────────────────────────────────────────────────

void paintTileGrid(DisplayManager &d, Sink &sink, const Rect &area, const std::vector<Tile> &tiles, int columns,
                   int rows) {
  columns = std::max(1, columns);
  rows = std::max(1, rows);
  const int cellW = (area.w - kGap * (columns - 1)) / columns;
  const int cellH = (area.h - kGap * (rows - 1)) / rows;
  const size_t count = std::min(tiles.size(), static_cast<size_t>(columns * rows));
  for (size_t i = 0; i < count; ++i) {
    const int column = static_cast<int>(i) % columns;
    const int row = static_cast<int>(i) / columns;
    const int x = area.x + column * (cellW + kGap);
    const int y = area.y + row * (cellH + kGap);
    const int w = column == columns - 1 ? right(area) - x : cellW;
    const int h = row == rows - 1 ? bottom(area) - y : cellH;
    const Tile &tile = tiles[i];
    const Rect rect(x, y, w, h);
    d.nanoTile(rect, tile.label, tile.icon, tile.detail, sink.pressed(tile.id), tile.accent || sink.armed(tile.id),
               tile.enabled);
    if (tile.enabled) {
      addTarget(sink, rect, tile.id);
    }
  }
}

// ─── Czytaj ─────────────────────────────────────────────────────────────────

void paintReadHome(DisplayManager &d, Sink &sink, const ReadHome &view) {
  const Rect content = tabContent();
  constexpr int kCardH = 78;
  constexpr int kFontsW = 60;
  const Rect card(content.x, content.y, content.w - kFontsW - kGap, kCardH);
  const bool cardPressed = sink.pressed(view.resumeId);
  const uint16_t surface = d.nanoColor(cardPressed ? Role::SurfaceActive : Role::SurfaceMuted);
  d.nanoFillRoundRect(card.x, card.y, card.w, card.h, 10, surface);
  addTarget(sink, card, view.resumeId);

  // Round play button on the right edge of the card.
  const int playR = 19;
  const int playCx = right(card) - 14 - playR;
  const int playCy = card.y + 30;
  if (view.hasBook) {
    d.nanoFillCircle(playCx, playCy, playR, d.nanoColor(Role::Accent));
    d.nanoIcon(Rect(playCx - playR + 2, playCy - playR, playR * 2, playR * 2), Icon::Play, d.nanoColor(Role::OnAccent),
               d.nanoColor(Role::Accent));
  }

  if (view.hasBook) {
    // Cover: a colored book with its initials.
    const Rect cover(card.x + 10, card.y + 10, kCoverW, kCoverH);
    paintCover(d, cover, 6, 6, 2, view.coverColor, view.coverInitials, 2, view.cover);

    const int textX = right(cover) + 12;
    const int textRight = playCx - playR - 12;
    d.nanoLabel(Rect(textX, card.y + 8, textRight - textX, 24), view.title, 2, Role::Foreground);
    d.nanoLabel(Rect(textX, card.y + 32, textRight - textX, 18), view.author, 1, Role::Muted);
    // Progress row along the bottom of the card.
    const int barY = card.y + kCardH - 18;
    const int labelW = 48;
    d.nanoProgress(Rect(textX, barY + 5, right(card) - 14 - labelW - 8 - textX, 5), view.progressPercent, 0, 100);
    d.nanoLabel(Rect(right(card) - 14 - labelW, barY - 3, labelW, 20), view.progressLabel, 1, Role::Accent,
                Align::End);
  } else {
    d.nanoIcon(Rect(card.x + 12, card.y, 30, card.h), Icon::Books, d.nanoColor(Role::Accent), surface);
    const int textX = card.x + 54;
    d.nanoLabel(Rect(textX, card.y + 16, right(card) - 14 - textX, 26), view.title, 2, Role::Foreground);
    d.nanoLabel(Rect(textX, card.y + 42, right(card) - 14 - textX, 20), view.hint, 1, Role::Muted);
  }

  const Rect fonts(right(card) + kGap, content.y, kFontsW, kCardH);
  const uint16_t fontsSurface = d.nanoColor(sink.pressed(view.fontsId) ? Role::SurfaceActive : Role::SurfaceMuted);
  d.nanoFillRoundRect(fonts.x, fonts.y, fonts.w, fonts.h, 10, fontsSurface);
  // "Aa" set large in the UI font: the font picker's own face.
  d.nanoText(Rect(fonts.x, fonts.y, fonts.w, fonts.h), "Aa", 3, d.nanoColor(Role::Foreground), Align::Center);
  addTarget(sink, fonts, view.fontsId);

  const Rect row(content.x, content.y + kCardH + kGap, content.w, content.h - kCardH - kGap);
  paintTileGrid(d, sink, row, view.tiles, std::max<int>(1, static_cast<int>(view.tiles.size())), 1);
}

// ─── Sections ───────────────────────────────────────────────────────────────

void paintSections(DisplayManager &d, Sink &sink, const Rect &area, const std::vector<Section> &sections) {
  constexpr int kLabelH = 16;
  int labelCount = 0;
  int rowCount = 0;
  for (const Section &section : sections) {
    labelCount += section.title.isEmpty() ? 0 : 1;
    int column = 0;
    for (const SectionItem &item : section.items) {
      if (item.fullWidth || column == 2) {
        if (column != 0) ++rowCount;
        column = 0;
      }
      if (item.fullWidth) {
        ++rowCount;
        continue;
      }
      ++column;
    }
    if (column != 0) ++rowCount;
  }
  if (rowCount == 0) {
    return;
  }
  const int free = area.h - labelCount * kLabelH - (rowCount - 1) * 6;
  const int rowH = std::max(28, std::min(40, free / rowCount));
  const int half = (area.w - kGap) / 2;
  int y = area.y;
  for (const Section &section : sections) {
    if (!section.title.isEmpty()) {
      d.nanoSeparator(Rect(area.x, y, area.w, kLabelH - 2), section.title);
      y += kLabelH;
    }
    int column = 0;
    for (size_t i = 0; i < section.items.size(); ++i) {
      const SectionItem &item = section.items[i];
      if ((item.fullWidth || column == 2) && column != 0) {
        y += rowH + 6;
        column = 0;
      }
      const bool alone = item.fullWidth;
      // A switch row next to a plain button gets the larger share: its
      // label ("Tryb zaawansowany") is the long one.
      const SectionItem *pair = column == 0 && i + 1 < section.items.size() ? &section.items[i + 1] : nullptr;
      const SectionItem *prev = column == 1 && i > 0 ? &section.items[i - 1] : nullptr;
      int split = half;
      if (pair != nullptr && !pair->fullWidth && item.toggle && !pair->toggle) {
        split = (area.w - kGap) * 3 / 5;
      } else if (prev != nullptr && prev->toggle && !item.toggle) {
        split = (area.w - kGap) * 3 / 5;
      }
      const int x = column == 0 ? area.x : area.x + split + kGap;
      const int w = alone ? area.w : (column == 0 ? split : right(area) - x);
      const Rect rect = withHelpButton(d, sink, Rect(x, y, w, rowH), item.helpId);
      if (item.toggle) {
        d.nanoToggle(rect, item.label, item.on, sink.pressed(item.id));
      } else {
        d.nanoButton(rect, item.label, true, item.icon, 1, "", "", sink.pressed(item.id));
      }
      addTarget(sink, rect, item.id);
      if (alone) {
        y += rowH + 6;
        column = 0;
      } else {
        ++column;
      }
    }
    if (column != 0) {
      y += rowH + 6;
    }
  }
}

// ─── Motywy ─────────────────────────────────────────────────────────────────

void paintThemes(DisplayManager &d, Sink &sink, const ThemesView &view) {
  const Rect content = tabContent();
  // Segmented control.
  constexpr int count = ThemesView::kSections;
  const Rect segments(content.x, content.y, content.w, 30);
  d.nanoFillRoundRect(segments.x, segments.y, segments.w, segments.h, 15, d.nanoColor(Role::SurfaceMuted));
  const int segmentW = segments.w / count;
  for (int i = 0; i < count; ++i) {
    const int x = segments.x + i * segmentW;
    const Rect rect(x, segments.y, i == count - 1 ? right(segments) - x : segmentW, segments.h);
    const bool active = view.section == i;
    if (active) {
      d.nanoFillRoundRect(rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, 12, d.nanoColor(Role::Accent));
    } else if (sink.pressed(view.segmentIds[i])) {
      d.nanoFillRoundRect(rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, 12, d.nanoColor(Role::SurfaceActive));
    }
    const uint8_t size = DisplayManager::nanoTextWidth(view.segmentLabels[i], 2) <= static_cast<int>(rect.w) - 12 ? 2 : 1;
    d.nanoText(rect, view.segmentLabels[i], size, d.nanoColor(active ? Role::OnAccent : Role::Foreground),
               Align::Center);
    addTarget(sink, rect, view.segmentIds[i]);
  }

  const Rect body(content.x, content.y + 38, content.w, content.h - 38);
  if (view.section == 0) {
    constexpr int kColumns = 5;
    constexpr int kRows = 3;
    const int cellW = (body.w - 6 * (kColumns - 1)) / kColumns;
    const int cellH = (body.h - 6 * (kRows - 1)) / kRows;
    size_t cell = 0;
    auto cellRect = [&](size_t index) {
      const int column = static_cast<int>(index) % kColumns;
      const int row = static_cast<int>(index) / kColumns;
      const int x = body.x + column * (cellW + 6);
      return Rect(x, body.y + row * (cellH + 6), column == kColumns - 1 ? right(body) - x : cellW, cellH);
    };
    for (const auto &chip : view.palettes) {
      if (cell >= static_cast<size_t>(kColumns * kRows)) break;
      const Rect rect = cellRect(cell++);
      d.nanoPaletteChip(rect, chip.palette, chip.name, chip.selected, sink.pressed(chip.id));
      addTarget(sink, rect, chip.id);
    }
    if (view.ownAccentId != kNoTarget && cell < static_cast<size_t>(kColumns * kRows)) {
      const Rect rect = cellRect(cell);
      d.nanoPill(rect, view.ownAccentLabel, view.ownAccentOn ? Icon::Check : Icon::None, sink.pressed(view.ownAccentId),
                 view.ownAccentOn);
      addTarget(sink, rect, view.ownAccentId);
    }
  } else if (view.section == 1) {
    constexpr int kColumns = 4;
    constexpr int kRows = 2;
    const int cellW = (body.w - kGap * (kColumns - 1)) / kColumns;
    const int cellH = (body.h - kGap * (kRows - 1)) / kRows;
    for (size_t i = 0; i < view.fonts.size() && i < static_cast<size_t>(kColumns * kRows); ++i) {
      const int column = static_cast<int>(i) % kColumns;
      const int row = static_cast<int>(i) / kColumns;
      const int x = body.x + column * (cellW + kGap);
      const Rect rect(x, body.y + row * (cellH + kGap), column == kColumns - 1 ? right(body) - x : cellW, cellH);
      const auto &chip = view.fonts[i];
      d.nanoFontChip(rect, chip.family, chip.name, chip.sample, chip.selected, sink.pressed(chip.id));
      addTarget(sink, rect, chip.id);
    }
  } else {
    const int hintH = view.layoutHint.isEmpty() ? 0 : 18;
    const Rect row(body.x, body.y, body.w, body.h - hintH);
    const int n = static_cast<int>(view.layouts.size());
    const int cellW = n > 0 ? (row.w - kGap * (n - 1)) / n : row.w;
    for (int i = 0; i < n; ++i) {
      const int x = row.x + i * (cellW + kGap);
      const Rect rect(x, row.y, i == n - 1 ? right(row) - x : cellW, row.h);
      const auto &chip = view.layouts[static_cast<size_t>(i)];
      d.nanoLayoutChip(rect, chip.compact, chip.railRight, chip.name, chip.detail, chip.selected,
                       sink.pressed(chip.id));
      addTarget(sink, rect, chip.id);
    }
    if (hintH > 0) {
      d.nanoLabel(Rect(body.x, bottom(body) - hintH + 2, body.w, hintH - 2), view.layoutHint, 1, Role::Muted,
                  Align::Center);
    }
  }
}

// ─── Biblioteka ─────────────────────────────────────────────────────────────

namespace {
constexpr int kShelfDetailHeight = 38;
constexpr int kShelfGap = 5;
constexpr int kShelfSpineBaseWidth = 29;
constexpr int kShelfSpineWidthStep = 2;
constexpr int kShelfSpinePeriod = 4;
constexpr int kShelfCycleWidth = kShelfSpinePeriod * (kShelfSpineBaseWidth + kShelfGap) +
                                 kShelfSpineWidthStep * kShelfSpinePeriod * (kShelfSpinePeriod - 1) / 2;

uint16_t shelfSpineColor(size_t index) {
  constexpr uint16_t kColors[] = {0x99E3, 0x1AF5, 0x0B6A, 0x7B98, 0x4490, 0xB4CD, 0x9A49, 0x32FA};
  return kColors[index % 8];
}
}  // namespace

int shelfSpineWidth(size_t index) {
  return kShelfSpineBaseWidth + kShelfSpineWidthStep * static_cast<int>(index % kShelfSpinePeriod);
}

int32_t shelfSpineLeft(size_t index) {
  const int32_t within = static_cast<int32_t>(index % kShelfSpinePeriod);
  return static_cast<int32_t>(index / kShelfSpinePeriod) * kShelfCycleWidth +
         within * (kShelfSpineBaseWidth + kShelfGap) + kShelfSpineWidthStep * within * (within - 1) / 2;
}

size_t shelfSpineIndexAt(int32_t contentX, size_t count) {
  if (count == 0 || contentX <= 0) {
    return 0;
  }
  const size_t cycle = static_cast<size_t>(contentX / kShelfCycleWidth);
  const int32_t within = contentX % kShelfCycleWidth;
  const size_t offset = within >= shelfSpineLeft(3)   ? 3
                        : within >= shelfSpineLeft(2) ? 2
                        : within >= shelfSpineLeft(1) ? 1
                                                      : 0;
  return std::min(count - 1, cycle * kShelfSpinePeriod + offset);
}

ShelfGeometry shelfGeometry() {
  const Rect content = tabContent();
  ShelfGeometry g;
  g.header = Rect(content.x, content.y, content.w, kHeaderH);
  const int detailY = bottom(content) - kShelfDetailHeight;
  const int viewportY = bottom(g.header) + 6;
  g.viewport = Rect(content.x, viewportY, content.w, detailY - 6 - viewportY);
  g.detail = Rect(content.x, detailY, content.w, kShelfDetailHeight);
  g.marker = g.viewport.x + g.viewport.w / 2;
  return g;
}

int32_t shelfClampOffset(size_t count, int32_t offset, int viewportWidth) {
  if (count == 0) {
    return 0;
  }
  const size_t last = count - 1;
  const int32_t lastCenter = shelfSpineLeft(last) + shelfSpineWidth(last) / 2;
  const int32_t firstCenter = shelfSpineWidth(0) / 2;
  return std::max<int32_t>(viewportWidth / 2 - lastCenter, std::min<int32_t>(offset, viewportWidth / 2 - firstCenter));
}

int32_t shelfCenteredOffset(size_t count, size_t index, int viewportWidth) {
  if (count == 0) {
    return 0;
  }
  index = std::min(index, count - 1);
  return shelfClampOffset(count, viewportWidth / 2 - shelfSpineLeft(index) - shelfSpineWidth(index) / 2,
                          viewportWidth);
}

size_t shelfNearest(size_t count, int32_t offset, int markerX, int viewportX) {
  if (count == 0) {
    return 0;
  }
  const int32_t contentX = markerX - viewportX - offset;
  const size_t candidate = shelfSpineIndexAt(contentX, count);
  const size_t first = candidate == 0 ? 0 : candidate - 1;
  const size_t last = std::min(count - 1, candidate + 1);
  size_t best = first;
  int32_t bestDistance = INT32_MAX;
  for (size_t i = first; i <= last; ++i) {
    const int32_t center = viewportX + shelfSpineLeft(i) + shelfSpineWidth(i) / 2 + offset;
    const int32_t distance = std::abs(center - markerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void paintShelf(DisplayManager &d, Sink &sink, const ShelfView &view) {
  const ShelfGeometry g = shelfGeometry();
  paintHeader(d, sink, g.header, view.header);
  const size_t count = view.books.size();
  if (count == 0) {
    d.nanoLabel(Rect(g.viewport.x, g.viewport.y, g.viewport.w, bottom(g.detail) - g.viewport.y), view.emptyLabel, 2,
                Role::Muted, Align::Center, 3);
    return;
  }
  const size_t selected = std::min(view.selected, count - 1);
  const uint16_t accent = d.nanoColor(Role::Accent);
  const uint16_t floor = d.nanoColor(Role::ProgressTrack);
  d.nanoSetClip(g.viewport.x, g.viewport.y - 8, g.viewport.w, g.viewport.h + 11);
  const int32_t contentLeft = -view.offset;
  const size_t firstVisible = shelfSpineIndexAt(contentLeft, count);
  const size_t lastVisible = shelfSpineIndexAt(contentLeft + g.viewport.w, count);
  const int maxHeight = g.viewport.h;
  for (size_t i = firstVisible; i <= lastVisible && i < count; ++i) {
    const ShelfBook &book = view.books[i];
    const int width = shelfSpineWidth(i);
    const int variation = static_cast<int>((i * 7 + book.title.length() * 3) % 19);
    const int height = std::min(maxHeight - 8, maxHeight - 26 + variation);
    const int x = g.viewport.x + static_cast<int>(shelfSpineLeft(i) + view.offset);
    const bool active = i == selected;
    const int y = bottom(g.viewport) - height - (active ? 6 : 0);
    const uint16_t fill = shelfSpineColor(i);
    if (book.spine.valid()) {
      d.nanoImage(Rect(x, y, width, height), 3, book.spine);
    } else {
      d.nanoFillRoundRect(x, y, width, height, 3, fill);
    }
    d.nanoFillRect(x + 3, y + 5, width - 6, 1, d.nanoBlend(Role::Background, 60));
    d.nanoFillRect(x + 3, y + height - 6, width - 6, 1, d.nanoBlend(Role::Background, 60));
    if (active) {
      d.nanoFillRoundRect(x, y - 5, width, 3, 1, accent);
    }
    if (book.progress > 0) {
      // Bookmark ribbon hanging from the top, as long as the progress.
      const int ribbonX = x + width - 9;
      const int ribbonHeight = std::max(8, (height - 8) * book.progress / 100);
      // Body, then the two tails of the swallowtail end (drawn rather than
      // cut out with the spine colour, so it also works over a picture).
      const int tailY = y + ribbonHeight - 3;
      d.nanoFillRect(ribbonX, y, 5, ribbonHeight - 3, 0xDACA);
      d.nanoFillTriangle(ribbonX, tailY, ribbonX + 2, tailY, ribbonX, y + ribbonHeight, 0xDACA);
      d.nanoFillTriangle(ribbonX + 2, tailY, ribbonX + 4, tailY, ribbonX + 4, y + ribbonHeight, 0xDACA);
    }
    if (book.spine.valid()) {
      continue;  // the picture carries its own lettering
    }
    // Spine lettering: up to 6 capitals, top to bottom.
    String title = book.title;
    String lower = title;
    lower.toLowerCase();
    if (lower.startsWith("the ")) {
      title = title.substring(4);
    }
    int letterY = y + 10;
    int written = 0;
    for (size_t c = 0; c < title.length() && written < 6 && letterY + 10 < y + height - 6; ++c) {
      char letter = title[c];
      if (letter >= 'a' && letter <= 'z') {
        letter = static_cast<char>(letter - 'a' + 'A');
      }
      const uint8_t value = static_cast<uint8_t>(letter);
      if ((letter >= 'A' && letter <= 'Z') || (letter >= '0' && letter <= '9') || value >= 0x80) {
        d.nanoText(Rect(x, letterY, width - (book.progress > 0 ? 6 : 0), 10), String(letter), 1, 0xFFFF,
                   Align::Center);
        letterY += 11;
        ++written;
      }
    }
  }
  d.nanoFillRoundRect(g.viewport.x, bottom(g.viewport), g.viewport.w, 3, 1, floor);
  d.nanoResetClip();

  // Detail strip: title + author on the left, progress on the right.
  const int percentW = 64;
  const int textW = g.detail.w - percentW - 12;
  d.nanoLabel(Rect(g.detail.x, g.detail.y, textW, 20), view.detailTitle, 2, Role::Foreground);
  String second = view.detailAuthor;
  if (!view.detailStatus.isEmpty()) {
    second += "  -  " + view.detailStatus;
  }
  d.nanoLabel(Rect(g.detail.x, g.detail.y + 20, textW, 17), second, 1, Role::Muted);
  d.nanoLabel(Rect(right(g.detail) - percentW, g.detail.y, percentW, g.detail.h), view.detailPercent, 3, Role::Accent,
              Align::End);
}

// ─── Rozdzialy ──────────────────────────────────────────────────────────────

Rect wheelViewport() {
  const Rect content = tabContent();
  return Rect(content.x, content.y + kHeaderH + 6, content.w, content.h - kHeaderH - 6);
}

int wheelRowCenter(const Rect &viewport, int row, int offset) {
  const int raw = row * kWheelRowStep + offset;
  const int magnitude = std::min(std::abs(raw), static_cast<int>(viewport.h));
  return viewport.y + viewport.h / 2 + raw * (2 * viewport.h - magnitude) / (2 * viewport.h);
}

int wheelRowHeight(bool centered) { return centered ? 30 : 20; }

bool wheelRowVisible(const Rect &viewport, int y, int height) {
  return y - height / 2 >= viewport.y && y + height / 2 <= viewport.y + viewport.h;
}

void paintWheel(DisplayManager &d, Sink &sink, const WheelView &view) {
  const Rect content = tabContent();
  paintHeader(d, sink, Rect(content.x, content.y, content.w, kHeaderH), view.header);
  const Rect viewport = wheelViewport();
  if (view.count == 0) {
    d.nanoButton(viewport, view.emptyLabel, true, Icon::Play, 1, "", "", sink.pressed(view.emptyId));
    addTarget(sink, viewport, view.emptyId);
    return;
  }
  const int centerY = viewport.y + viewport.h / 2;
  const int halfHeight = std::max(1, viewport.h / 2);
  const int maximumWidth = viewport.w;
  d.nanoSetClip(viewport.x, viewport.y, viewport.w, viewport.h);
  for (size_t local = 0; local < view.titles.size(); ++local) {
    const size_t i = view.firstIndex + local;
    const int y = wheelRowCenter(viewport, static_cast<int>(i) - static_cast<int>(view.centered), view.offset);
    const int curved = y - centerY;
    const bool centered = i == view.centered;
    const uint8_t alpha =
        centered ? 255 : static_cast<uint8_t>(std::max(40, 210 - std::abs(curved) * 170 / halfHeight));
    const int height = wheelRowHeight(centered);
    if (!wheelRowVisible(viewport, y, height)) {
      continue;
    }
    const int width = centered ? maximumWidth
                               : maximumWidth - std::min(std::abs(curved), halfHeight) * (maximumWidth / 4) / halfHeight;
    const int x = viewport.x + (viewport.w - width) / 2;
    const int top = y - height / 2;
    if (centered) {
      d.nanoFillRoundRect(x, top, width, height, 8, d.nanoColor(Role::SurfaceActive));
      d.nanoFillRoundRect(x + 5, top + 6, 3, height - 12, 1, d.nanoColor(Role::Accent));
    } else {
      d.nanoFillRoundRect(x, top, width, height, 6, d.nanoBlend(Role::SurfaceMuted, alpha));
    }
    if (i == view.readingIndex) {
      d.nanoFillCircle(x + width - 14, y, 4, centered ? d.nanoColor(Role::Accent) : d.nanoBlend(Role::Accent, alpha));
    }
    const String &title = view.titles[local];
    d.nanoText(Rect(x + 16, top, width - 40, height), title, centered ? 2 : 1, d.nanoBlend(Role::Foreground, alpha),
               centered ? Align::Start : Align::Center);
  }
  d.nanoResetClip();
}

// ─── Lists ──────────────────────────────────────────────────────────────────

// One control of a list or of a Wyglad czytania row.
void paintListItem(DisplayManager &d, Sink &sink, const Rect &rect, const ListItem &item) {
  const bool pressed = sink.pressed(item.id);
  switch (item.kind) {
    case ListItem::Kind::Separator:
      d.nanoSeparator(Rect(rect.x, rect.y + rect.h / 2 - 7, rect.w, 14), item.label);
      break;
    case ListItem::Kind::Label:
      d.nanoLabel(rect, item.label, 2, Role::Muted, Align::Start, 3);
      break;
    case ListItem::Kind::Setting:
      d.nanoSetting(rect, item.label, item.value, true, pressed);
      addTarget(sink, rect, item.id);
      break;
    case ListItem::Kind::Toggle:
      d.nanoToggle(rect, item.label, item.on, pressed);
      addTarget(sink, rect, item.id);
      break;
    case ListItem::Kind::Slider:
      d.nanoSlider(rect, item.label, item.value, item.sliderValue, item.sliderMin, item.sliderMax, pressed,
                   item.dragging);
      sink.slider(rect, item.id);
      addTarget(sink, rect, item.id);
      break;
    case ListItem::Kind::Row: {
      const int trailingW = item.trailingId == kNoTarget ? 0 : std::min(120, rect.w / 4);
      const Rect name(rect.x, rect.y, rect.w - (trailingW > 0 ? trailingW + kGap : 0), rect.h);
      d.nanoButton(name, item.label, true, item.icon, 1, item.value, "", pressed, item.armed || sink.armed(item.id));
      addTarget(sink, name, item.id);
      if (trailingW > 0) {
        const Rect trailing(right(rect) - trailingW, rect.y, trailingW, rect.h);
        const bool armed = item.trailingArmed || sink.armed(item.trailingId);
        d.nanoButton(trailing, armed ? item.trailingLabel : String(), true, Icon::Trash, 1, "", "",
                     sink.pressed(item.trailingId), armed);
        addTarget(sink, trailing, item.trailingId);
      }
      break;
    }
    case ListItem::Kind::Button:
    default: {
      const bool armed = item.armed || sink.armed(item.id);
      d.nanoButton(rect, item.label, true, item.icon, rect.h >= 48 ? 2 : 1, item.value, "", pressed, armed,
                   item.typeface);
      if (item.marked && !armed) {
        d.nanoFillCircle(right(rect) - 16, rect.y + rect.h / 2, 8, d.nanoColor(Role::Accent));
        d.nanoIcon(Rect(right(rect) - 26, rect.y + rect.h / 2 - 10, 20, 20), Icon::Check, d.nanoColor(Role::OnAccent),
                   d.nanoColor(Role::Accent));
      }
      addTarget(sink, rect, item.id);
      break;
    }
  }
}

void paintList(DisplayManager &d, Sink &sink, const ListView &view) {
  const Rect area = view.fullScreen ? fullContent() : tabContent();
  const int top = paintHeader(d, sink, Rect(area.x, area.y, area.w, kHeaderH), view.header);
  const Rect grid(area.x, top, area.w, bottom(area) - top);
  const int columns = std::max(1, view.columns);
  const int rows = std::max(1, view.rows);
  const int gap = 6;
  const int cellW = (grid.w - kGap * (columns - 1)) / columns;
  const int cellH = (grid.h - gap * (rows - 1)) / rows;
  int column = 0;
  int row = 0;
  for (const ListItem &item : view.items) {
    const bool wide = item.fullWidth || item.kind == ListItem::Kind::Label || item.kind == ListItem::Kind::Separator ||
                      item.kind == ListItem::Kind::Row || columns == 1;
    if (wide && column != 0) {
      column = 0;
      ++row;
    }
    if (row >= rows) {
      break;
    }
    const int span = item.kind == ListItem::Kind::Label ? std::min(2, rows - row) : 1;
    const int x = grid.x + column * (cellW + kGap);
    const int y = grid.y + row * (cellH + gap);
    const int w = wide ? grid.w : (column == columns - 1 ? right(grid) - x : cellW);
    const int h = span * cellH + (span - 1) * gap;
    paintListItem(d, sink, withHelpButton(d, sink, Rect(x, y, w, h), item.helpId), item);
    if (wide) {
      column = 0;
      row += span;
    } else if (++column >= columns) {
      column = 0;
      ++row;
    }
  }
}

// ─── Book details ───────────────────────────────────────────────────────────

void paintBookDetails(DisplayManager &d, Sink &sink, const BookDetailsView &view) {
  const Rect full = fullContent();
  const int top = paintHeader(d, sink, Rect(full.x, full.y, full.w, kHeaderH), view.header);
  // Cover down the left, twice the size of the one on the Czytaj card.
  const Rect cover(full.x, top, kCoverW * 2, kCoverH * 2);
  paintCover(d, cover, 8, 12, 3, view.coverColor, view.coverInitials, 3, view.cover);
  const Rect area(right(cover) + 12, full.y, right(full) - right(cover) - 12, full.h);
  const int percentW = 60;
  d.nanoLabel(Rect(area.x, top, area.w - percentW - 10, 18), view.author, 1, Role::Muted);
  d.nanoLabel(Rect(right(area) - percentW, top, percentW, 18), view.percentLabel, 1, Role::Accent, Align::End);
  d.nanoProgress(Rect(area.x, top + 22, area.w, 5), view.percent, 0, 100);
  const int gridY = top + 36;
  const int columns = view.actions.size() > 4 ? 3 : 2;
  paintTileGrid(d, sink, Rect(area.x, gridY, area.w, bottom(area) - gridY), view.actions, columns, 2);
}

// ─── Confirm ────────────────────────────────────────────────────────────────

void paintConfirm(DisplayManager &d, Sink &sink, const ConfirmView &view) {
  const Rect content = fullContent();
  const int width = std::min(480, static_cast<int>(content.w));
  const Rect panel(content.x + (content.w - width) / 2, content.y + 4, width, content.h - 8);
  const int textH = view.detail.isEmpty() ? 64 : 48;
  d.nanoLabel(Rect(panel.x, panel.y, panel.w, textH), view.question, 2, Role::Foreground, Align::Center, 2);
  if (!view.detail.isEmpty()) {
    d.nanoLabel(Rect(panel.x, panel.y + textH, panel.w, 20), view.detail, 1, Role::Muted, Align::Center);
  }
  const int buttonsH = 44;
  const int buttonsY = bottom(panel) - buttonsH;
  int x = panel.x;
  const bool hasBack = view.backId != kNoTarget;
  const int backW = hasBack ? (view.backLabel.isEmpty() ? 56 : 140) : 0;
  const int actionCount = static_cast<int>(view.actions.size());
  const int actionsW = panel.w - (hasBack ? backW + kGap : 0);
  const int actionW = actionCount > 0 ? (actionsW - kGap * (actionCount - 1)) / actionCount : 0;
  if (hasBack) {
    const Rect back(x, buttonsY, backW, buttonsH);
    d.nanoButton(back, view.backLabel, true, Icon::ChevronLeft, 1, "", "", sink.pressed(view.backId));
    addTarget(sink, back, view.backId);
    x += backW + kGap;
  }
  for (int i = 0; i < actionCount; ++i) {
    const auto &action = view.actions[static_cast<size_t>(i)];
    const int w = i == actionCount - 1 ? right(panel) - x : actionW;
    const Rect rect(x, buttonsY, w, buttonsH);
    d.nanoButton(rect, action.label, true, action.danger ? Icon::Trash : Icon::None, 1, "", "", sink.pressed(action.id),
                 action.armed || sink.armed(action.id));
    addTarget(sink, rect, action.id);
    x += w + kGap;
  }
}

// ─── Reader panel ───────────────────────────────────────────────────────────

Rect readerPanelWordArea() { return Rect(0, 24, kScreenW, 96); }

Rect readerPanelBar() { return Rect(0, 128, kScreenW, kScreenH - 128); }

Rect readerPanelStatusArea() { return Rect(0, 0, kScreenW - 100, 22); }

void paintReaderPanel(DisplayManager &d, Sink &sink, const ReaderPanelView &view) {
  // Top line: chapter left, time left + percent right, battery.
  const int statusW = 150;
  d.nanoBatteryInline(Rect(kScreenW - 104, 2, 96, 20), false, Align::End);
  String status = view.progressLabel;
  if (!view.timeLeft.isEmpty()) {
    status = view.timeLeft + "  -  " + status;
  }
  const bool statusPressed = sink.pressed(view.statusId);
  if (statusPressed) {
    d.nanoFillRoundRect(4, 1, kScreenW - 112, 22, 6, d.nanoColor(Role::SurfaceMuted));
  }
  d.nanoLabel(Rect(kScreenW - 112 - statusW, 2, statusW, 20), status, 1, Role::Muted, Align::End);
  d.nanoLabel(Rect(12, 2, kScreenW - 136 - statusW, 20), view.chapter, 1, Role::Muted);
  addTarget(sink, readerPanelStatusArea(), view.statusId);

  const Rect words = readerPanelWordArea();
  if (view.scrollMode) {
    d.nanoScrollPreview(Rect(0, words.y, kScreenW, words.h - (view.hint.isEmpty() ? 0 : 14)), view.words,
                        view.currentLocal);
  } else {
    d.nanoReaderPreview(Rect(0, words.y + 2, kScreenW, words.h - (view.hint.isEmpty() ? 4 : 18)), view.before,
                        view.word, view.after);
  }
  if (!view.hint.isEmpty()) {
    d.nanoLabel(Rect(12, bottom(words) - 16, kScreenW - 24, 16), view.hint, 1, Role::Subtle, Align::Center);
  }
  d.nanoProgress(Rect(12, 122, kScreenW - 24, 3), view.progressPercent, 0, 100);

  // Bottom bar: Menu | chapters | go to | bookmark | << | colors | - WPM + | Czytaj.
  const int y = 132;
  const int h = kScreenH - y - 4;
  constexpr int kSmall = 44;
  int x = 10;
  const Rect menu(x, y, 72, h);
  d.nanoButton(menu, view.menuLabel, true, Icon::None, 1, "", "", sink.pressed(view.menuId));
  addTarget(sink, menu, view.menuId);
  x += menu.w + kGap;
  auto small = [&](int id, Icon icon) {
    if (id == kNoTarget) {
      return;
    }
    const Rect rect(x, y, kSmall, h);
    d.nanoButton(rect, "", true, icon, 1, "", "", sink.pressed(id));
    addTarget(sink, rect, id);
    x += kSmall + kGap;
  };
  small(view.chaptersId, Icon::List);
  small(view.gotoId, Icon::Target);
  const Rect bookmark(x, y, kSmall, h);
  const uint16_t bookmarkSurface = d.nanoColor(sink.pressed(view.bookmarkId) ? Role::SurfaceActive : Role::SurfaceMuted);
  d.nanoFillRoundRect(bookmark.x, bookmark.y, bookmark.w, bookmark.h, 8, bookmarkSurface);
  d.nanoIcon(bookmark, Icon::Bookmark, d.nanoColor(view.bookmarkFilled ? Role::Accent : Role::Foreground),
             bookmarkSurface);
  if (!view.bookmarkFilled) {
    // Hollow ribbon: punch the inside back out.
    const int cx = bookmark.x + bookmark.w / 2;
    const int cy = bookmark.y + bookmark.h / 2;
    d.nanoFillRect(cx - 4, cy - 7, 8, 11, bookmarkSurface);
  }
  addTarget(sink, bookmark, view.bookmarkId);
  x += bookmark.w + kGap;
  small(view.rewindId, Icon::Rewind);
  small(view.lookId, Icon::Palette);

  // WPM stepper: one pill with - and + ends.
  const int stepperW = 164;
  const Rect stepper(x, y, stepperW, h);
  d.nanoFillRoundRect(stepper.x, stepper.y, stepper.w, stepper.h, 8, d.nanoColor(Role::SurfaceMuted));
  const Rect minus(stepper.x, y, 42, h);
  const Rect plus(right(stepper) - 42, y, 42, h);
  const Rect value(right(minus), y, stepper.w - 84, h);
  if (sink.pressed(view.minusId)) {
    d.nanoFillRoundRect(minus.x, minus.y, minus.w, minus.h, 8, d.nanoColor(Role::SurfaceActive));
  }
  if (sink.pressed(view.plusId)) {
    d.nanoFillRoundRect(plus.x, plus.y, plus.w, plus.h, 8, d.nanoColor(Role::SurfaceActive));
  }
  if (sink.pressed(view.wpmId)) {
    d.nanoFillRect(value.x, value.y, value.w, value.h, d.nanoColor(Role::SurfaceActive));
  }
  d.nanoIcon(minus, Icon::Minus, d.nanoColor(Role::Foreground), d.nanoColor(Role::SurfaceMuted));
  d.nanoIcon(plus, Icon::Plus, d.nanoColor(Role::Foreground), d.nanoColor(Role::SurfaceMuted));
  d.nanoLabel(value, view.wpmLabel, 2, Role::Foreground, Align::Center);
  addTarget(sink, minus, view.minusId);
  addTarget(sink, plus, view.plusId);
  addTarget(sink, value, view.wpmId);
  x += stepperW + kGap;

  const Rect start(x, y, kScreenW - 10 - x, h);
  d.nanoButton(start, view.startLabel, true, Icon::Play, 1, "", "", sink.pressed(view.startId), true);
  addTarget(sink, start, view.startId);
}

// ─── Przejdz do ─────────────────────────────────────────────────────────────

namespace {
void paintSegments(DisplayManager &d, Sink &sink, const Rect &segments, const int ids[3], const String labels[3],
                   int active) {
  d.nanoFillRoundRect(segments.x, segments.y, segments.w, segments.h, segments.h / 2, d.nanoColor(Role::SurfaceMuted));
  const int segmentW = segments.w / 3;
  for (int i = 0; i < 3; ++i) {
    const int x = segments.x + i * segmentW;
    const Rect rect(x, segments.y, i == 2 ? right(segments) - x : segmentW, segments.h);
    const bool on = active == i;
    if (on) {
      d.nanoFillRoundRect(rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, (rect.h - 6) / 2, d.nanoColor(Role::Accent));
    } else if (sink.pressed(ids[i])) {
      d.nanoFillRoundRect(rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, (rect.h - 6) / 2,
                          d.nanoColor(Role::SurfaceActive));
    }
    d.nanoText(rect, labels[i], 1, d.nanoColor(on ? Role::OnAccent : Role::Foreground), Align::Center);
    addTarget(sink, rect, ids[i]);
  }
}
}  // namespace

Rect goToBarRect() { return Rect(kMargin + 4, 92, kScreenW - (kMargin + 4) * 2, 26); }

void paintGoTo(DisplayManager &d, Sink &sink, const GoToView &view) {
  const Rect area = fullContent();
  Header header = view.header;
  paintHeader(d, sink, Rect(area.x, area.y, area.w - 318, kHeaderH), header);
  paintSegments(d, sink, Rect(right(area) - 308, area.y, 308, kHeaderH), view.segmentIds, view.segmentLabels,
                view.segment);

  // Big readout + the chapter it lands in.
  const int valueW = std::min(300, DisplayManager::nanoTextWidth(view.value, 3) + 8);
  d.nanoText(Rect(area.x + 4, 46, valueW, 36), view.value, 3, d.nanoColor(Role::Foreground));
  d.nanoLabel(Rect(area.x + 4 + valueW + 14, 46, area.w - valueW - 22, 20), view.detail, 2, Role::Accent);
  d.nanoLabel(Rect(area.x + 4 + valueW + 14, 66, area.w - valueW - 22, 16), view.hint, 1, Role::Muted);

  // Bar: tap or drag anywhere on it.
  const Rect bar = goToBarRect();
  const int range = std::max(1, view.sliderMax - view.sliderMin);
  const int value = std::max(view.sliderMin, std::min(view.sliderValue, view.sliderMax));
  const int fill = bar.h + (bar.w - bar.h) * (value - view.sliderMin) / range;
  d.nanoFillRoundRect(bar.x, bar.y, bar.w, bar.h, bar.h / 2, d.nanoColor(Role::SurfaceMuted));
  d.nanoFillRoundRect(bar.x, bar.y, fill, bar.h, bar.h / 2, d.nanoColor(Role::Accent));
  d.nanoFillCircle(bar.x + fill - bar.h / 2, bar.y + bar.h / 2, bar.h / 2 - (view.dragging ? 2 : 4),
                   d.nanoColor(Role::OnAccent));

  // Fine steps and the action.
  const int y = 128;
  const int h = kScreenH - y - 6;
  const Rect minus(area.x, y, 64, h);
  const Rect plus(area.x + 64 + kGap, y, 64, h);
  d.nanoButton(minus, "", true, Icon::Minus, 1, "", "", sink.pressed(view.minusId));
  d.nanoButton(plus, "", true, Icon::Plus, 1, "", "", sink.pressed(view.plusId));
  addTarget(sink, minus, view.minusId);
  addTarget(sink, plus, view.plusId);
  const Rect read(right(area) - 220, y, 220, h);
  d.nanoButton(read, view.readLabel, true, Icon::Play, 1, "", "", sink.pressed(view.readId), true);
  addTarget(sink, read, view.readId);
}

// ─── Choice ─────────────────────────────────────────────────────────────────

void paintChoice(DisplayManager &d, Sink &sink, const ChoiceView &view) {
  const Rect area = fullContent();
  int top = area.y;
  if (view.header.backId != kNoTarget || !view.header.title.isEmpty()) {
    top = paintHeader(d, sink, Rect(area.x, area.y, area.w, kHeaderH), view.header);
  }
  if (!view.question.isEmpty()) {
    d.nanoLabel(Rect(area.x, top, area.w, 26), view.question, 2, Role::Muted, Align::Center);
    top += 32;
  }
  const int count = std::max<int>(1, static_cast<int>(view.options.size()));
  const int cellW = (area.w - kGap * (count - 1)) / count;
  for (int i = 0; i < count && i < static_cast<int>(view.options.size()); ++i) {
    const auto &option = view.options[static_cast<size_t>(i)];
    const int x = area.x + i * (cellW + kGap);
    const Rect rect(x, top, i == count - 1 ? right(area) - x : cellW, bottom(area) - top);
    d.nanoTile(rect, option.label, option.icon, option.detail, sink.pressed(option.id), option.accent);
    addTarget(sink, rect, option.id);
  }
}

// ─── Help page ──────────────────────────────────────────────────────────────

Rect helpBodyRect() {
  const Rect area = fullContent();
  return Rect(area.x + 6, area.y + kHeaderH + 8, area.w - 22, area.h - kHeaderH - 8);
}

int helpTextWidth() { return helpBodyRect().w; }

int helpContentHeight(const HelpView &view) {
  const int lineH = DisplayManager::nanoLineHeight(2);
  int height = 0;
  for (const String &line : view.lines) {
    height += line.isEmpty() ? lineH / 2 : lineH;
  }
  return height;
}

void paintHelp(DisplayManager &d, Sink &sink, const HelpView &view) {
  const Rect area = fullContent();
  paintHeader(d, sink, Rect(area.x, area.y, area.w, kHeaderH), view.header);
  const Rect body = helpBodyRect();
  const int lineH = DisplayManager::nanoLineHeight(2);
  d.nanoSetClip(body.x, body.y, body.w, body.h);
  int y = body.y - view.scroll;
  for (const String &line : view.lines) {
    const int h = line.isEmpty() ? lineH / 2 : lineH;
    if (y + h > body.y && y < bottom(body) && !line.isEmpty()) {
      d.nanoText(Rect(body.x, y, body.w, lineH), line, 2, d.nanoColor(Role::Foreground));
    }
    y += h;
  }
  d.nanoResetClip();
  // Scroll bar: where the visible part sits in the whole text.
  const int total = helpContentHeight(view);
  if (total > body.h) {
    const int trackX = right(area) - 6;
    d.nanoFillRoundRect(trackX, body.y, 4, body.h, 2, d.nanoColor(Role::SurfaceMuted));
    const int thumbH = std::max(16, body.h * body.h / total);
    const int thumbY = body.y + (body.h - thumbH) * view.scroll / std::max(1, total - body.h);
    d.nanoFillRoundRect(trackX, thumbY, 4, thumbH, 2, d.nanoColor(Role::Accent));
  }
}

// ─── Kolor litery ───────────────────────────────────────────────────────────

void paintColorPicker(DisplayManager &d, Sink &sink, const ColorPickerView &view) {
  const Rect area = fullContent();
  // Header with a live preview of the choice on the right, in the reading
  // colors.
  const int previewW = 176;
  paintHeader(d, sink, Rect(area.x, area.y, area.w - previewW - kGap, kHeaderH), view.header);
  const Rect preview(right(area) - previewW, area.y, previewW, kHeaderH);
  d.nanoFillRoundRect(preview.x, preview.y, preview.w, preview.h, 8, view.previewBackground);
  String left;
  String mid;
  String rest;
  splitSampleWord(view.sampleWord.isEmpty() ? String("reading") : view.sampleWord, left, mid, rest);
  constexpr uint8_t kScale = 48;
  const int wl = d.nanoTypefaceTextWidth(left, kScale);
  const int wm = d.nanoTypefaceTextWidth(mid, kScale);
  const int wr = d.nanoTypefaceTextWidth(rest, kScale);
  const int tx = preview.x + (preview.w - wl - wm - wr) / 2;
  const int ty = preview.y + 1;
  d.nanoSetClip(preview.x, preview.y, preview.w, preview.h);
  d.nanoTypefaceText(tx, ty, left, view.previewWord, kScale);
  d.nanoTypefaceText(tx + wl, ty, mid, view.previewFocus, kScale);
  d.nanoTypefaceText(tx + wl + wm, ty, rest, view.previewWord, kScale);
  d.nanoResetClip();

  const Rect grid(area.x, area.y + kHeaderH + 8, area.w, area.h - kHeaderH - 8);
  const int columns = std::max(1, view.columns);
  const int rows = std::max(1, view.rows);
  const int gap = 5;
  const int cellW = (grid.w - gap * (columns - 1)) / columns;
  const int cellH = (grid.h - gap * (rows - 1)) / rows;
  for (size_t i = 0; i < view.swatches.size() && i < static_cast<size_t>(columns * rows); ++i) {
    const int column = static_cast<int>(i) % columns;
    const int row = static_cast<int>(i) / columns;
    const int x = grid.x + column * (cellW + gap);
    const Rect rect(x, grid.y + row * (cellH + gap), column == columns - 1 ? right(grid) - x : cellW, cellH);
    const auto &swatch = view.swatches[i];
    d.nanoColorSwatch(rect, swatch.color, swatch.selected, sink.pressed(swatch.id));
    addTarget(sink, rect, swatch.id);
  }
}

// ── Wygaszacze ──────────────────────────────────────────────────────────────

void paintSaverOverlay(DisplayManager &d, const SaverOverlay &overlay) {
  if (!overlay.label.isEmpty() && overlay.labelAlpha > 0) {
    d.nanoText(Rect(0, 6, kScreenW, 20), overlay.label, 1, d.nanoBlend(Role::Muted, overlay.labelAlpha),
               Align::Center);
  }
  if (!overlay.hint.isEmpty() && overlay.hintAlpha > 0) {
    d.nanoText(Rect(0, kScreenH - 22, kScreenW, 20), overlay.hint, 1, d.nanoBlend(Role::Muted, overlay.hintAlpha),
               Align::Center);
  }
}

void paintSaverBook(DisplayManager &d, const SaverBookView &view) {
  d.nanoBeginFrame();
  const int cardW = 420;
  const int cardH = 104;
  const int x = (kScreenW - cardW) / 2 + view.driftX;
  const int y = (kScreenH - cardH) / 2 + view.driftY;
  if (view.hasBook) {
    const Rect cover(x, y, cardH * kCoverW / kCoverH, cardH);
    paintCover(d, cover, 8, 10, 3, view.coverColor, view.coverInitials, 3, view.cover);
    const int textX = right(cover) + 18;
    const int textW = x + cardW - textX;
    d.nanoText(Rect(textX, y + 2, textW, 34), view.title, 3, d.nanoColor(Role::Foreground));
    d.nanoText(Rect(textX, y + 38, textW, 22), view.author, 1, d.nanoColor(Role::Muted));
    const int barY = y + cardH - 16;
    const int labelW = 56;
    const Rect bar(textX, barY, textW - labelW - 10, 6);
    d.nanoFillRoundRect(bar.x, bar.y, bar.w, bar.h, 3, d.nanoColor(Role::ProgressTrack));
    const int fill = bar.w * std::max(0, std::min(100, view.progressPercent)) / 100;
    if (fill > 0) {
      d.nanoFillRoundRect(bar.x, bar.y, std::max(6, fill), bar.h, 3, d.nanoColor(Role::Accent));
    }
    d.nanoText(Rect(right(bar) + 10, barY - 8, labelW, 22), view.progressLabel, 1, d.nanoColor(Role::Accent),
               Align::End);
  } else {
    d.nanoIcon(Rect(x, y, 40, cardH), Icon::Books, d.nanoColor(Role::Accent), d.nanoColor(Role::Background));
    d.nanoText(Rect(x + 56, y, cardW - 56, cardH), view.title, 2, d.nanoColor(Role::Muted));
  }
  paintSaverOverlay(d, view.overlay);
  d.nanoEndFrame();
}

void paintSaverWords(DisplayManager &d, const SaverWordsView &view) {
  d.nanoBeginFrame();
  constexpr int kWordGap = 22;
  const int centreX = kScreenW / 2;
  for (const SaverLane &lane : view.lanes) {
    if (lane.words.empty()) continue;
    std::vector<int> widths;
    widths.reserve(lane.words.size());
    uint32_t total = 0;
    for (const String &word : lane.words) {
      const int w = DisplayManager::nanoTextWidth(word, lane.size);
      widths.push_back(w);
      total += static_cast<uint32_t>(w + kWordGap);
    }
    if (total == 0) continue;
    const uint16_t ink = d.nanoBlend(Role::Foreground, lane.alpha);
    int x = -static_cast<int>(lane.offset % total);
    size_t i = 0;
    while (x < kScreenW) {
      const int w = widths[i];
      if (x + w > 0) {
        const bool centre = lane.markCentre && x <= centreX && x + w >= centreX;
        d.nanoTextLineAt(x, lane.y, lane.words[i], lane.size, centre ? d.nanoColor(Role::Accent) : ink);
      }
      x += w + kWordGap;
      i = (i + 1) % lane.words.size();
    }
  }
  paintSaverOverlay(d, view.overlay);
  d.nanoEndFrame();
}

void paintSaverWaves(DisplayManager &d, const SaverWavesView &view) {
  d.nanoBeginFrame();
  // Each wave: amplitude (px), wavelength (px), speed (phase units per
  // frame), vertical centre, brightness. Faint ones first so the bright
  // one sits on top.
  struct Wave {
    int amplitude;
    int wavelength;
    int speed;
    int centre;
    uint8_t alpha;
  };
  constexpr Wave kWaves[] = {
      {44, 560, 3, 70, 90}, {30, 380, -4, 104, 130}, {24, 260, 5, 62, 170}, {36, 440, 2, 90, 255},
  };
  constexpr float kTwoPi = 6.2831853f;
  for (const Wave &wave : kWaves) {
    const uint16_t color = d.nanoBlend(Role::Accent, wave.alpha);
    const float shift = static_cast<float>(static_cast<int32_t>(view.phase) * wave.speed) / 100.0f;
    // Slow swell of the amplitude so the picture never repeats exactly.
    const float swell = 0.75f + 0.25f * sinf(static_cast<float>(view.phase) / 90.0f + wave.centre);
    int prevX = 0;
    int prevY = 0;
    for (int x = 0; x <= kScreenW; x += 4) {
      const float t = kTwoPi * static_cast<float>(x) / static_cast<float>(wave.wavelength) + shift;
      const int y = wave.centre + static_cast<int>(sinf(t) * wave.amplitude * swell);
      if (x > 0) {
        d.nanoDrawLine(prevX, prevY, x, y, color);
        d.nanoDrawLine(prevX, prevY + 1, x, y + 1, color);
        if (wave.alpha == 255) {
          d.nanoDrawLine(prevX, prevY + 2, x, y + 2, color);
        }
      }
      prevX = x;
      prevY = y;
    }
  }
  paintSaverOverlay(d, view.overlay);
  d.nanoEndFrame();
}

// ── Samouczek ───────────────────────────────────────────────────────────────

namespace {

void paintTutorialArt(DisplayManager &d, const Rect &area, const TutorialView &view) {
  const uint16_t surface = d.nanoColor(Role::SurfaceMuted);
  d.nanoFillRoundRect(area.x, area.y, area.w, area.h, 12, surface);
  const int cx = area.x + area.w / 2;
  const int cy = area.y + area.h / 2;
  const uint16_t fg = d.nanoColor(Role::Foreground);
  const uint16_t muted = d.nanoColor(Role::Muted);
  const uint16_t accent = d.nanoColor(Role::Accent);
  switch (view.art) {
    case TutorialArt::Rsvp: {
      // The word in the reading typeface, its focus letter in the accent,
      // with the guide lines the reading screen draws.
      const String word = view.artWord.isEmpty() ? String("reading") : view.artWord;
      constexpr uint8_t kScale = 52;
      const int focus =
          std::min<int>(static_cast<int>(word.length()) - 1, (static_cast<int>(word.length()) + 2) / 4);
      const String head = word.substring(0, focus);
      const String letter = word.substring(focus, focus + 1);
      const String tail = word.substring(focus + 1);
      const int headW = d.nanoTypefaceTextWidth(head, kScale);
      const int letterW = d.nanoTypefaceTextWidth(letter, kScale);
      // 69 = kEmbeddedAtkinsonHeight (that header carries the glyph bitmaps).
      const int textH = 69 * kScale / 100;
      const int letterX = cx - letterW / 2;
      const int top = cy - textH / 2 - 4;
      d.nanoTypefaceText(letterX - headW, top, head, fg, kScale);
      d.nanoTypefaceText(letterX, top, letter, accent, kScale);
      d.nanoTypefaceText(letterX + letterW, top, tail, fg, kScale);
      d.nanoFillRect(cx - 1, area.y + 10, 2, 10, muted);
      d.nanoFillRect(cx - 1, area.y + area.h - 20, 2, 10, muted);
      d.nanoDrawLine(area.x + 20, area.y + 15, area.x + area.w - 20, area.y + 15, d.nanoBlend(Role::Muted, 90));
      d.nanoDrawLine(area.x + 20, area.y + area.h - 15, area.x + area.w - 20, area.y + area.h - 15,
                     d.nanoBlend(Role::Muted, 90));
      break;
    }
    case TutorialArt::Start: {
      const Rect pill(area.x + 16, cy - 18, 124, 36);
      d.nanoFillRoundRect(pill.x, pill.y, pill.w, pill.h, 18, accent);
      d.nanoIcon(Rect(pill.x + 8, pill.y, 26, pill.h), Icon::Play, d.nanoColor(Role::OnAccent), accent);
      d.nanoText(Rect(pill.x + 34, pill.y, pill.w - 40, pill.h), view.artStart, 2, d.nanoColor(Role::OnAccent),
                 Align::Center);
      // A finger held on the word: a dot with two rings.
      const int fx = area.x + area.w - 54;
      d.nanoFillCircle(fx, cy, 9, fg);
      d.nanoDrawCircle(fx, cy, 17, d.nanoBlend(Role::Foreground, 150));
      d.nanoDrawCircle(fx, cy, 26, d.nanoBlend(Role::Foreground, 80));
      break;
    }
    case TutorialArt::Speed: {
      const int r = 17;
      const uint16_t knob = d.nanoColor(Role::SurfaceActive);
      d.nanoFillCircle(area.x + 34, cy - 6, r, knob);
      d.nanoIcon(Rect(area.x + 34 - r, cy - 6 - r, 2 * r, 2 * r), Icon::Minus, fg, knob);
      d.nanoFillCircle(area.x + area.w - 34, cy - 6, r, knob);
      d.nanoIcon(Rect(area.x + area.w - 34 - r, cy - 6 - r, 2 * r, 2 * r), Icon::Plus, fg, knob);
      d.nanoText(Rect(area.x + 56, cy - 34, area.w - 112, 40), "300", 4, fg, Align::Center);
      d.nanoText(Rect(area.x + 56, cy + 6, area.w - 112, 18), view.artUnit, 1, muted, Align::Center);
      const Rect bar(area.x + 20, area.y + area.h - 16, area.w - 40, 6);
      d.nanoFillRoundRect(bar.x, bar.y, bar.w, bar.h, 3, d.nanoColor(Role::ProgressTrack));
      d.nanoFillRoundRect(bar.x, bar.y, bar.w * 30 / 100, bar.h, 3, accent);
      break;
    }
    case TutorialArt::Scrub: {
      // The status bar a tap on which jumps somewhere else in the book.
      const Rect bar(area.x + 16, area.y + 14, area.w - 32, 5);
      d.nanoFillRoundRect(bar.x, bar.y, bar.w, bar.h, 2, d.nanoColor(Role::ProgressTrack));
      d.nanoFillRoundRect(bar.x, bar.y, bar.w * 42 / 100, bar.h, 2, accent);
      d.nanoText(Rect(area.x + 40, cy - 14, area.w - 80, 28), view.artWord, 2, fg, Align::Center);
      d.nanoIcon(Rect(area.x + 6, cy - 14, 28, 28), Icon::ChevronLeft, accent, surface);
      d.nanoIcon(Rect(area.x + area.w - 34, cy - 14, 28, 28), Icon::ChevronRight, accent, surface);
      d.nanoDrawLine(area.x + 60, cy + 26, area.x + area.w - 60, cy + 26, d.nanoBlend(Role::Muted, 140));
      d.nanoFillCircle(area.x + area.w - 60, cy + 26, 5, fg);
      break;
    }
    case TutorialArt::Menu: {
      const Icon icons[4] = {Icon::Book, Icon::Sliders, Icon::Palette, Icon::Device};
      const int rowH = (area.h - 12) / 4;
      for (int i = 0; i < 4; ++i) {
        const Rect row(area.x + 8, area.y + 6 + i * rowH, area.w - 16, rowH - 2);
        const uint16_t rowSurface = i == 0 ? d.nanoColor(Role::SurfaceActive) : surface;
        if (i == 0) {
          d.nanoFillRoundRect(row.x, row.y, row.w, row.h, 6, rowSurface);
          d.nanoFillRoundRect(row.x + 2, row.y + 4, 3, row.h - 8, 1, accent);
        }
        d.nanoIcon(Rect(row.x + 10, row.y, 22, row.h), icons[i], i == 0 ? accent : muted, rowSurface);
        d.nanoText(Rect(row.x + 40, row.y, row.w - 44, row.h), view.artTabs[i], 1, i == 0 ? fg : muted);
      }
      break;
    }
    case TutorialArt::Help: {
      const Rect tile(area.x + 16, cy - 26, area.w - 32, 52);
      d.nanoFillRoundRect(tile.x, tile.y, tile.w, tile.h, 10, d.nanoColor(Role::SurfaceActive));
      d.nanoText(Rect(tile.x + 14, tile.y, tile.w - 70, tile.h), view.artTile, 2, fg);
      const int qx = tile.x + tile.w - 26;
      d.nanoFillCircle(qx, cy, 14, accent);
      d.nanoText(Rect(qx - 14, cy - 14, 28, 28), "?", 2, d.nanoColor(Role::OnAccent), Align::Center);
      d.nanoDrawCircle(qx, cy, 21, d.nanoBlend(Role::Accent, 120));
      break;
    }
  }
}

}  // namespace

void paintTutorial(DisplayManager &d, Sink &sink, const TutorialView &view) {
  d.nanoBeginFrame();
  // Top row: caption with the page count, Pomiń on the right.
  const String counter =
      String(static_cast<unsigned>(view.page + 1)) + "/" + String(static_cast<unsigned>(view.pageCount));
  d.nanoText(Rect(14, 4, 300, 22), view.caption + "  " + counter, 1, d.nanoColor(Role::Muted));
  if (view.skipId != kNoTarget) {
    const int w = std::max(76, DisplayManager::nanoTextWidth(view.skipLabel, 1) + 28);
    const Rect skip(kScreenW - w - 8, 3, w, 24);
    d.nanoPill(skip, view.skipLabel, Icon::None, sink.pressed(view.skipId), false);
    addTarget(sink, Rect(skip.x - 8, 0, skip.w + 16, 34), view.skipId);
  }

  const Rect art(10, 32, 244, 100);
  paintTutorialArt(d, art, view);

  const int textX = right(art) + 16;
  const int textW = kScreenW - textX - 12;
  d.nanoText(Rect(textX, 30, textW, 30), view.title, 3, d.nanoColor(Role::Foreground));
  const std::vector<String> lines = DisplayManager::nanoWrapText(view.body, textW, 1);
  const int lineH = DisplayManager::nanoLineHeight(1);
  int y = 62;
  for (const String &line : lines) {
    if (y + lineH > 138) break;
    d.nanoText(Rect(textX, y, textW, lineH), line, 1, d.nanoColor(Role::Muted));
    y += lineH;
  }

  // Bottom row: Wstecz, page dots, Dalej / Gotowe.
  const int rowY = kScreenH - 32;
  if (view.backId != kNoTarget) {
    const Rect back(10, rowY, 120, 28);
    d.nanoPill(back, view.backLabel, Icon::ChevronLeft, sink.pressed(view.backId), false);
    addTarget(sink, Rect(0, rowY - 6, 150, kScreenH - rowY + 6), view.backId);
  }
  const int dotGap = 14;
  const int dotsW = static_cast<int>(view.pageCount) * dotGap;
  int dotX = (kScreenW - dotsW) / 2 + dotGap / 2;
  for (size_t i = 0; i < view.pageCount; ++i) {
    const bool on = i == view.page;
    d.nanoFillCircle(dotX, rowY + 14, on ? 4 : 3, on ? d.nanoColor(Role::Accent) : d.nanoColor(Role::Subtle));
    dotX += dotGap;
  }
  const Rect next(kScreenW - 150, rowY, 140, 28);
  const bool pressedNext = sink.pressed(view.nextId);
  const uint16_t nextFill = pressedNext ? d.nanoBlend(Role::Accent, 170) : d.nanoColor(Role::Accent);
  d.nanoFillRoundRect(next.x, next.y, next.w, next.h, 14, nextFill);
  d.nanoText(Rect(next.x + 6, next.y, next.w - 30, next.h), view.nextLabel, 2, d.nanoColor(Role::OnAccent),
             Align::Center);
  d.nanoIcon(Rect(next.x + next.w - 30, next.y, 24, next.h), Icon::ChevronRight, d.nanoColor(Role::OnAccent),
             nextFill);
  addTarget(sink, Rect(kScreenW - 170, rowY - 6, 170, kScreenH - rowY + 6), view.nextId);

  d.nanoEndFrame();
}


// ── Kreator ─────────────────────────────────────────────────────────────────

namespace {

void paintWizardChip(DisplayManager &d, Sink &sink, const Rect &rect, const WizardChip &chip) {
  const bool pressed = sink.pressed(chip.id);
  if (chip.art == WizardChipArt::Theme) {
    String head;
    String letter;
    String tail;
    splitSampleWord(chip.word.isEmpty() ? String("reading") : chip.word, head, letter, tail);
    d.nanoReadingThemeChip(rect, chip.theme, chip.label, head, letter, tail, chip.selected, pressed);
    addTarget(sink, rect, chip.id);
    return;
  }
  if (chip.art == WizardChipArt::Palette) {
    d.nanoPaletteChip(rect, chip.palette, chip.label, chip.selected, pressed);
    addTarget(sink, rect, chip.id);
    return;
  }
  if (chip.art == WizardChipArt::UiFont) {
    d.nanoFontChip(rect, chip.family, chip.label, "", chip.selected, pressed);
    addTarget(sink, rect, chip.id);
    return;
  }
  if (chip.art == WizardChipArt::Typeface) {
    if (chip.disabled) {
      // Still on its way to the card: its place is kept so the pages do
      // not shift as the pack arrives.
      d.nanoButton(rect, chip.label, false, Icon::None, 1, "", "", false, false, chip.typeface);
      return;
    }
    d.nanoButton(rect, chip.label, true, Icon::None, 1, "", "", pressed || chip.selected, false, chip.typeface);
    if (chip.selected) {
      d.nanoDrawRoundRect(rect.x, rect.y, rect.w, rect.h, 10, d.nanoColor(Role::Accent));
      d.nanoDrawRoundRect(rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2, 9, d.nanoColor(Role::Accent));
    }
    addTarget(sink, rect, chip.id);
    return;
  }
  const uint16_t surface = d.nanoColor(chip.selected || pressed ? Role::SurfaceActive : Role::SurfaceMuted);
  d.nanoFillRoundRect(rect.x, rect.y, rect.w, rect.h, 10, surface);
  if (chip.selected) {
    d.nanoDrawRoundRect(rect.x, rect.y, rect.w, rect.h, 10, d.nanoColor(Role::Accent));
    d.nanoDrawRoundRect(rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2, 9, d.nanoColor(Role::Accent));
  }
  const uint16_t ink = d.nanoColor(chip.selected ? Role::Foreground : Role::Muted);
  switch (chip.art) {
    case WizardChipArt::Swatch: {
      const int r = std::min(12, static_cast<int>(rect.h) / 4);
      d.nanoFillCircle(rect.x + rect.w / 2, rect.y + rect.h / 2 - 8, r, chip.swatch);
      d.nanoText(Rect(rect.x + 4, rect.y + rect.h - 24, rect.w - 8, 20), chip.label, 1, ink, Align::Center);
      break;
    }
    case WizardChipArt::Rsvp: {
      // The focus letter in the accent, as the reader shows it.
      String head;
      String letter;
      String tail;
      splitSampleWord(chip.word.isEmpty() ? String("reading") : chip.word, head, letter, tail);
      const int headW = DisplayManager::nanoTextWidth(head, 3);
      const int letterW = DisplayManager::nanoTextWidth(letter, 3);
      const int tailW = DisplayManager::nanoTextWidth(tail, 3);
      const int x = rect.x + (rect.w - headW - letterW - tailW) / 2;
      const int cy = rect.y + rect.h / 2 - 8;
      d.nanoTextLineAt(x, cy, head, 3, d.nanoColor(Role::Foreground));
      d.nanoTextLineAt(x + headW, cy, letter, 3, d.nanoColor(Role::Accent));
      d.nanoTextLineAt(x + headW + letterW, cy, tail, 3, d.nanoColor(Role::Foreground));
      d.nanoText(Rect(rect.x + 4, rect.y + rect.h - 22, rect.w - 8, 20), chip.label, 1, ink, Align::Center);
      break;
    }
    case WizardChipArt::Book: {
      if (rect.h < 52) {
        // Low chips (six books on one page): title and author one small
        // line each.
        d.nanoText(Rect(rect.x + 8, rect.y + 2, rect.w - 16, rect.h / 2), chip.label, 1,
                   d.nanoColor(chip.selected ? Role::Foreground : Role::Muted), Align::Center);
        d.nanoText(Rect(rect.x + 8, rect.y + rect.h / 2 - 1, rect.w - 16, rect.h / 2), chip.detail, 1,
                   chip.selected ? d.nanoColor(Role::Accent) : d.nanoBlend(Role::Muted, 170), Align::Center);
        break;
      }
      // A title that fits one line keeps the big size; longer ones drop to
      // the small size on two lines so the author still has its row.
      const bool oneLine = DisplayManager::nanoTextWidth(chip.label, 2) <= rect.w - 16;
      d.nanoText(Rect(rect.x + 8, rect.y + 4, rect.w - 16, rect.h - 26), chip.label, oneLine ? 2 : 1,
                 d.nanoColor(chip.selected ? Role::Foreground : Role::Muted), Align::Center, 2);
      d.nanoText(Rect(rect.x + 6, rect.y + rect.h - 20, rect.w - 12, 16), chip.detail, 1,
                 chip.selected ? d.nanoColor(Role::Accent) : d.nanoBlend(Role::Muted, 170), Align::Center);
      break;
    }
    case WizardChipArt::Scroll: {
      const int lineW[] = {rect.w - 60, rect.w - 40, rect.w - 76};
      for (int i = 0; i < 3; ++i) {
        const int y = rect.y + 12 + i * 10;
        d.nanoFillRoundRect(rect.x + 20, y, lineW[i], 4, 2,
                            i == 1 ? d.nanoColor(Role::Foreground) : d.nanoBlend(Role::Muted, 120));
      }
      d.nanoFillRoundRect(rect.x + 20 + lineW[1] / 3, rect.y + 20, 36, 6, 3, d.nanoColor(Role::Accent));
      d.nanoText(Rect(rect.x + 4, rect.y + rect.h - 22, rect.w - 8, 20), chip.label, 1, ink, Align::Center);
      break;
    }
    default:
      d.nanoText(Rect(rect.x + 4, rect.y, rect.w - 8, rect.h), chip.label, 2, ink, Align::Center);
      break;
  }
  addTarget(sink, rect, chip.id);
}

// The largest size, from maxSize down to 1, at which text fits width on one
// line.
uint8_t fittingTextSize(const String &text, int width, uint8_t maxSize) {
  uint8_t size = maxSize;
  while (size > 1 && DisplayManager::nanoTextWidth(text, size) > width) {
    --size;
  }
  return size;
}

void paintWizardQr(DisplayManager &d, const Rect &box, const bool *qr, uint8_t size) {
  // White quiet zone behind dark modules, whatever the palette: phone
  // cameras want dark-on-light.
  d.nanoFillRoundRect(box.x, box.y, box.w, box.h, 8, 0xFFFF);
  if (qr == nullptr || size == 0) {
    return;
  }
  const int module = std::max(1, (std::min<int>(box.w, box.h) - 12) / size);
  const int side = module * size;
  const int x0 = box.x + (box.w - side) / 2;
  const int y0 = box.y + (box.h - side) / 2;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      if (qr[y * size + x]) {
        d.nanoFillRect(x0 + x * module, y0 + y * module, module, module, 0x0000);
      }
    }
  }
}

}  // namespace

void paintWizard(DisplayManager &d, Sink &sink, const WizardView &view) {
  d.nanoBeginFrame();
  const uint16_t fg = d.nanoColor(Role::Foreground);
  const uint16_t muted = d.nanoColor(Role::Muted);
  const uint16_t accent = d.nanoColor(Role::Accent);

  // Step bar: one segment per step, done and current in the accent.
  if (view.stepCount > 1) {
    const int gap = 4;
    const int barX = 16;
    const int barW = kScreenW - 32;
    const int count = static_cast<int>(view.stepCount);
    const int segW = (barW - gap * (count - 1)) / count;
    for (int i = 0; i < count; ++i) {
      const bool done = static_cast<size_t>(i) <= view.step;
      d.nanoFillRoundRect(barX + i * (segW + gap), 5, segW, 4, 2,
                          done ? accent : d.nanoColor(Role::ProgressTrack));
    }
  }

  const bool qr = view.body == WizardBody::Qr;
  const int textW = qr ? 440 : kScreenW - 32;
  if (view.body != WizardBody::Message && view.body != WizardBody::Preview) {
    // A long title (English "Connect your reader to your phone" beside the
    // QR) drops a size instead of losing its end to an ellipsis.
    d.nanoText(Rect(16, 14, textW, 32), view.title, fittingTextSize(view.title, textW, 3), fg);
    if (view.body == WizardBody::Loading) {
      // Room for a two-line tip over the bar.
      d.nanoText(Rect(16, 46, textW, 38), view.subtitle, 1, muted, Align::Start, 2);
    } else if (!view.tallChips) {
      d.nanoText(Rect(16, 46, textW, 20), view.subtitle, 1, muted);
    }
  }

  const Rect content = view.tallChips ? Rect(16, 50, kScreenW - 32, 84) : Rect(16, 68, kScreenW - 32, 64);
  switch (view.body) {
    case WizardBody::Chips: {
      Rect area = content;
      // Tall chips (all six books) never page: no arrow slots, full width.
      const bool paged = view.pagePrevId != kNoTarget || view.pageNextId != kNoTarget ||
                         (view.chipColumns > 0 && !view.tallChips);
      if (paged) {
        // Arrow buttons either side; a missing id (first or last page)
        // leaves its slot empty so the chips never shift between pages.
        constexpr int kArrowW = 34;
        const Rect prev(content.x, content.y, kArrowW, content.h);
        const Rect next(content.x + content.w - kArrowW, content.y, kArrowW, content.h);
        if (view.pagePrevId != kNoTarget) {
          d.nanoIconButton(prev, Icon::ChevronLeft, sink.pressed(view.pagePrevId));
          addTarget(sink, Rect(prev.x - 6, prev.y - 4, prev.w + 10, prev.h + 8), view.pagePrevId);
        }
        if (view.pageNextId != kNoTarget) {
          d.nanoIconButton(next, Icon::ChevronRight, sink.pressed(view.pageNextId));
          addTarget(sink, Rect(next.x - 4, next.y - 4, next.w + 10, next.h + 8), view.pageNextId);
        }
        area = Rect(content.x + kArrowW + 8, content.y, content.w - 2 * (kArrowW + 8), content.h);
      }
      const int rows = std::max(1, view.chipRows);
      const int total = std::max<int>(1, static_cast<int>(view.chips.size()));
      const int columns = view.chipColumns > 0 ? view.chipColumns : std::max(1, (total + rows - 1) / rows);
      const int gap = rows > 1 ? 6 : 8;
      const int chipW = (area.w - gap * (columns - 1)) / columns;
      const int chipH = (area.h - gap * (rows - 1)) / rows;
      for (int i = 0; i < static_cast<int>(view.chips.size()); ++i) {
        const int column = i % columns;
        const int row = i / columns;
        if (row >= rows) break;
        paintWizardChip(d, sink,
                        Rect(area.x + column * (chipW + gap), area.y + row * (chipH + gap), chipW, chipH),
                        view.chips[i]);
      }
      break;
    }
    case WizardBody::Preview: {
      const Rect area(16, 14, kScreenW - 32, 118);
      if (view.previewMode == 0) {
        d.nanoReaderSample(area, view.previewBefore, view.previewWord, view.previewAfter, view.previewSizeLevel);
      } else if (view.scrollWords != nullptr && !view.scrollWords->empty()) {
        d.nanoDrawRoundRect(area.x, area.y, area.w, area.h, 12, d.nanoColor(Role::ProgressTrack));
        d.nanoScrollPreview(Rect(area.x + 12, area.y + 8, area.w - 24, area.h - 16), *view.scrollWords,
                            view.scrollCurrent);
      }
      break;
    }
    case WizardBody::Message: {
      d.nanoText(Rect(16, 30, kScreenW - 32, 44), view.title, fittingTextSize(view.title, kScreenW - 32, 4), fg,
                 Align::Center);
      if (view.autoPercent >= 0) {
        d.nanoText(Rect(16, 78, kScreenW - 32, 22), view.subtitle, 2, muted, Align::Center);
      } else {
        // No auto-advance bar: room for a two-line explanation.
        d.nanoText(Rect(16, 78, kScreenW - 32, 50), view.subtitle, 2, muted, Align::Center, 2);
      }
      if (view.autoPercent >= 0) {
        const Rect bar(kScreenW / 2 - 80, 114, 160, 4);
        d.nanoFillRoundRect(bar.x, bar.y, bar.w, bar.h, 2, d.nanoColor(Role::ProgressTrack));
        d.nanoFillRoundRect(bar.x, bar.y, std::max(4, bar.w * std::min(100, view.autoPercent) / 100), bar.h, 2,
                            accent);
      }
      break;
    }
    case WizardBody::Loading: {
      // A segment gliding along the track, back and forth.
      const Rect track(content.x + 40, content.y + 30, content.w - 80, 6);
      d.nanoFillRoundRect(track.x, track.y, track.w, track.h, 3, d.nanoColor(Role::ProgressTrack));
      if (!view.status.isEmpty()) {
        d.nanoText(Rect(track.x, track.y + 14, track.w, 18), view.status, 1, muted, Align::Center);
      }
      if (view.percent >= 0) {
        const int fill = std::max(6, track.w * std::min(100, view.percent) / 100);
        d.nanoFillRoundRect(track.x, track.y, fill, track.h, 3, accent);
        break;
      }
      const int segW = track.w / 4;
      const int travel = track.w - segW;
      const int period = 40;
      const int t = static_cast<int>(view.phase % (2 * period));
      const int pos = t < period ? t : 2 * period - t;
      d.nanoFillRoundRect(track.x + travel * pos / period, track.y, segW, track.h, 3, accent);
      break;
    }
    case WizardBody::Qr: {
      paintWizardQr(d, Rect(kScreenW - 16 - 124, 14, 124, 124), view.qr, view.qrSize);
      d.nanoText(Rect(16, 70, textW, 24), view.qrLine, 2, fg);
      d.nanoText(Rect(16, 98, textW, 36), view.qrHint, 1, d.nanoColor(Role::Accent), Align::Start, 2);
      break;
    }
  }

  // Bottom row.
  const int rowY = kScreenH - 32;
  if (view.backId != kNoTarget) {
    const Rect back(10, rowY, 120, 28);
    d.nanoPill(back, view.backLabel, Icon::ChevronLeft, sink.pressed(view.backId), false);
    addTarget(sink, Rect(0, rowY - 6, 150, kScreenH - rowY + 6), view.backId);
  }
  int rightEdge = kScreenW - 10;
  if (view.nextId != kNoTarget) {
    const Rect next(rightEdge - 140, rowY, 140, 28);
    const uint16_t fill = sink.pressed(view.nextId) ? d.nanoBlend(Role::Accent, 170) : accent;
    d.nanoFillRoundRect(next.x, next.y, next.w, next.h, 14, fill);
    d.nanoText(Rect(next.x + 6, next.y, next.w - 30, next.h), view.nextLabel, 2, d.nanoColor(Role::OnAccent),
               Align::Center);
    d.nanoIcon(Rect(next.x + next.w - 30, next.y, 24, next.h), Icon::ChevronRight, d.nanoColor(Role::OnAccent),
               fill);
    addTarget(sink, Rect(next.x - 10, rowY - 6, next.w + 20, kScreenH - rowY + 6), view.nextId);
    rightEdge = next.x - 10;
  }
  if (view.extraId != kNoTarget) {
    const int w = std::max(100, DisplayManager::nanoTextWidth(view.extraLabel, 1) + 44);
    const Rect extra(rightEdge - w, rowY, w, 28);
    d.nanoPill(extra, view.extraLabel, Icon::Play, sink.pressed(view.extraId), false);
    addTarget(sink, Rect(extra.x - 6, rowY - 6, extra.w + 12, kScreenH - rowY + 6), view.extraId);
    rightEdge = extra.x - 10;
  }
  if (!view.footer.isEmpty()) {
    const int left = view.backId != kNoTarget ? 140 : 16;
    // Two lines when it doesn't fit beside the buttons.
    d.nanoText(Rect(left, rowY - 4, std::max(0, rightEdge - left), 36), view.footer, 1, muted,
               view.backId != kNoTarget ? Align::Center : Align::Start, 2);
  }

  d.nanoEndFrame();
}


// ── Aplikacja (phone sync) ──────────────────────────────────────────────────

void paintSync(DisplayManager &d, Sink &sink, const SyncView &view) {
  d.nanoBeginFrame();
  // Segment buttons, top left.
  int x = 10;
  for (int i = 0; i < 2; ++i) {
    const int w = std::max(110, DisplayManager::nanoTextWidth(view.pageLabels[i], 1) + 32);
    const Rect seg(x, 6, w, 26);
    d.nanoPill(seg, view.pageLabels[i], Icon::None, sink.pressed(view.pageIds[i]), static_cast<size_t>(i) == view.page);
    addTarget(sink, Rect(seg.x - 4, 0, seg.w + 8, 40), view.pageIds[i]);
    x += w + 8;
  }
  // Zakończ, bottom left under the text.
  const int stopW = std::max(120, DisplayManager::nanoTextWidth(view.stopLabel, 2) + 36);
  const Rect stop(10, kScreenH - 36, stopW, 30);
  d.nanoPill(stop, view.stopLabel, Icon::Power, sink.pressed(view.stopId), false);
  addTarget(sink, Rect(0, stop.y - 6, stop.w + 20, kScreenH - stop.y + 6), view.stopId);

  // QR on the right, on white so phone cameras read it in any palette.
  const Rect box(kScreenW - 10 - 128, 22, 128, 128);
  d.nanoFillRoundRect(box.x, box.y, box.w, box.h, 8, 0xFFFF);
  if (view.qr != nullptr && view.qrSize > 0) {
    const int module = std::max(1, (box.w - 12) / view.qrSize);
    const int side = module * view.qrSize;
    const int x0 = box.x + (box.w - side) / 2;
    const int y0 = box.y + (box.h - side) / 2;
    for (int row = 0; row < view.qrSize; ++row) {
      for (int col = 0; col < view.qrSize; ++col) {
        if (view.qr[row * view.qrSize + col]) {
          d.nanoFillRect(x0 + col * module, y0 + row * module, module, module, 0x0000);
        }
      }
    }
  }
  // Page dots under the code.
  for (int i = 0; i < 2; ++i) {
    const bool on = static_cast<size_t>(i) == view.page;
    d.nanoFillCircle(box.x + box.w / 2 - 7 + i * 14, kScreenH - 12, on ? 4 : 3,
                     on ? d.nanoColor(Role::Accent) : d.nanoColor(Role::Subtle));
  }

  const int textW = box.x - 16 - 12;
  d.nanoText(Rect(16, 42, textW, 32), view.title, 3, d.nanoColor(Role::Foreground));
  d.nanoText(Rect(16, 76, textW, 24), view.line, 2, d.nanoColor(Role::Accent));
  d.nanoText(Rect(16, 102, textW, 32), view.hint, 1, d.nanoColor(Role::Muted), Align::Start, 2);
  d.nanoEndFrame();
}


// ─── Wyglad czytania ────────────────────────────────────────────────────────

namespace {
constexpr int kTypographySampleH = 80;
constexpr int kTypographyBarH = 28;
}  // namespace

Rect typographySampleRect() { return Rect(0, 0, kScreenW, kTypographySampleH); }

void paintTypography(DisplayManager &d, Sink &sink, const TypographyView &view) {
  // The sample is the reading screen, so it keeps the reading colors while
  // the controls below wear the menu palette: what changes the reading
  // screen sits on the reading screen.
  const Rect sample = typographySampleRect();
  d.nanoReaderSample(sample, view.before, view.word, view.after, view.fontSizeLevel);
  addTarget(sink, sample, view.sampleId);

  const int barY = bottom(sample) + 4;
  const Rect back(kMargin, barY, 44, kTypographyBarH);
  iconButton(d, sink, back, view.backId, Icon::ChevronLeft);
  const Rect segments(right(back) + kGap, barY, kScreenW - kMargin - right(back) - kGap, kTypographyBarH);
  paintSegments(d, sink, segments, view.segmentIds, view.segmentLabels, view.section);

  const int rowY = barY + kTypographyBarH + 6;
  const Rect row(kMargin, rowY, kScreenW - kMargin * 2, kScreenH - 4 - rowY);
  // Theme chips are narrow, the letter color and the other controls share
  // what is left equally.
  const int themeCount = static_cast<int>(view.themes.size());
  const int otherCount = static_cast<int>(view.items.size()) + (view.letterColorId != kNoTarget ? 1 : 0);
  const int cells = themeCount + otherCount;
  if (cells == 0) {
    return;
  }
  const int gaps = kGap * (cells - 1);
  const int themeW = themeCount > 0 ? std::min(100, (row.w - gaps) / cells) : 0;
  const int otherW = otherCount > 0 ? (row.w - gaps - themeW * themeCount) / otherCount : 0;
  int x = row.x;
  int cell = 0;
  auto nextRect = [&](int width) {
    ++cell;
    const int w = cell == cells ? right(row) - x : width;
    const Rect rect(x, row.y, w, row.h);
    x += w + kGap;
    return rect;
  };
  for (const TypographyView::ThemeChip &chip : view.themes) {
    const Rect rect = nextRect(themeW);
    d.nanoReadingThemePill(rect, chip.theme, chip.name, chip.selected, sink.pressed(chip.id));
    addTarget(sink, rect, chip.id);
  }
  if (view.letterColorId != kNoTarget) {
    const Rect rect = nextRect(otherW);
    d.nanoLetterColorTile(rect, view.letterColor, view.letterColorLabel, sink.pressed(view.letterColorId));
    addTarget(sink, rect, view.letterColorId);
  }
  for (const ListItem &item : view.items) {
    paintListItem(d, sink, nextRect(otherW), item);
  }
}

}  // namespace nano
