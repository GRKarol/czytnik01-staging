// Every Nano screen with realistic Polish content, painted by the same
// ui/NanoScreens.cpp code the firmware runs.
#include <string>
#include <vector>

#include "display/DisplayManager.h"
#include "ui/NanoScreens.h"

using namespace nano;

namespace {

struct SimSink : Sink {
  std::vector<Rect> rects;
  int pressedId = -999;
  void target(const Rect &rect, int id) override {
    (void)id;
    rects.push_back(rect);
  }
  bool pressed(int id) const override { return id == pressedId; }
};

// `~a` -> ą etc. in the firmware's single-byte encoding (LatinText.h).
String pl(const char *s) {
  String out;
  for (const char *p = s; *p; ++p) {
    if (*p == '~' && p[1]) {
      ++p;
      switch (*p) {
        case 'a': out += "\x97"; break;
        case 'c': out += "\x9B"; break;
        case 'e': out += "\x99"; break;
        case 'l': out += "\x83"; break;
        case 'n': out += "\x9D"; break;
        case 'o': out += "\xF3"; break;
        case 's': out += "\x9F"; break;
        case 'z': out += "\xB5"; break;
        case 'x': out += "\xB3"; break;
        default: out += *p;
      }
    } else {
      out += *p;
    }
  }
  return out;
}

std::vector<RailTab> railTabs(int active) {
  std::vector<RailTab> tabs;
  tabs.push_back({1, "Czytaj", Icon::Books, false, false});
  tabs.push_back({2, "Ustawienia", Icon::Sliders, false, false});
  tabs.push_back({3, "Motywy", Icon::Palette, false, false});
  tabs.push_back({4, pl("Urz~adzenie"), Icon::Device, false, true});
  tabs.push_back({5, "Pluginy", Icon::Apps, false, false});
  tabs[static_cast<size_t>(active)].active = true;
  return tabs;
}

void setRail(const std::vector<RailTab> &tabs) {
  std::vector<String> labels;
  for (const auto &t : tabs) labels.push_back(t.label);
  layout().railWidth = railWidthFor(labels);
}

Tile tile(int id, const String &label, const String &detail, Icon icon, bool accent = false) {
  Tile t;
  t.id = id;
  t.label = label;
  t.detail = detail;
  t.icon = icon;
  t.accent = accent;
  return t;
}

}  // namespace

void runScreens(DisplayManager &d, void (*dump)(const DisplayManager &, const char *)) {
  SimSink sink;
  auto frame = [&](const std::string &name, int tab, auto paint) {
    sink.rects.clear();
    d.nanoBeginFrame();
    paint();
    if (tab >= 0) {
      paintRail(d, sink, railTabs(tab));
    }
    d.nanoEndFrame();
    dump(d, name.c_str());
    for (const Rect &r : sink.rects) d.nanoDrawRect(r.x, r.y, r.w, r.h, 0xF81F);
    dump(d, (name + "_targets").c_str());
  };

  for (uint8_t fontIndex : {uint8_t(0), uint8_t(4)}) {
    d.setNanoUiFont(fontIndex);
    setRail(railTabs(0));
    const std::string suffix = fontIndex == 0 ? "" : "_literata";

    frame("read" + suffix, 0, [&] {
      ReadHome v;
      v.hasBook = true;
      v.title = pl("Pan Tadeusz, czyli ostatni zajazd na Litwie");
      v.author = "Adam Mickiewicz";
      v.progressLabel = "42%";
      v.progressPercent = 42;
      v.coverInitials = "PT";
      v.coverColor = 0x9A49;
      v.resumeId = 10;
      v.fontsId = 11;
      v.fontsLabel = "Czcionka";
      v.tiles = {tile(12, pl("Rozdzia~ly"), "12", Icon::List), tile(13, "Punkty zapisu", "3", Icon::Bookmark),
                 tile(14, "Biblioteka", pl("24 ksi~a~zki"), Icon::Books)};
      paintReadHome(d, sink, v);
    });

    frame("settings" + suffix, 1, [&] {
      std::vector<Section> sections(3);
      sections[0].title = "Czytanie";
      sections[0].items = {{20, "Tempo i pauzy", Icon::Sliders}, {21, "Typografia", Icon::Font}};
      sections[1].title = "System";
      sections[1].items = {{22, pl("Wy~swietlacz"), Icon::Sun}, {23, "Wygaszacz", Icon::Hourglass}};
      sections[2].items = {{24, "Tryb zaawansowany", Icon::None, true, true}, {25, "Presety", Icon::Edit}};
      paintSections(d, sink, tabContent(), sections);
    });

    for (int section = 0; section < ThemesView::kSections; ++section) {
      frame("themes" + std::to_string(section) + suffix, 2, [&] {
        ThemesView v;
        v.section = section;
        const String segs[] = {"Kolory menu", "Czcionka menu", pl("Uk~lad")};
        for (int i = 0; i < ThemesView::kSections; ++i) {
          v.segmentIds[i] = 30 + i;
          v.segmentLabels[i] = segs[i];
        }
        const char *names[] = {"Jak czytanie", "Mocha", "Macchiato", "Frappe", "Latte", "Dracula", "Nord",
                               "Gruvbox", "Tokyo", "Solarized", "Krem", "Sepia", "Grafit", "Las"};
        for (uint8_t p = 0; p < 14; ++p) {
          ThemesView::PaletteChip chip;
          chip.id = 40 + p;
          chip.palette = p;
          chip.name = names[p];
          chip.selected = p == 0;
          v.palettes.push_back(chip);
        }
        ThemesView::FontChip follow;
        follow.id = 70;
        follow.family = 4;
        follow.name = "Jak czytanie";
        follow.sample = "Literata";
        v.fonts.push_back(follow);
        for (uint8_t f = 0; f < DisplayManager::nanoUiFontCount(); ++f) {
          ThemesView::FontChip chip;
          chip.id = 71 + f;
          chip.family = f;
          chip.name = DisplayManager::nanoUiFontName(f);
          chip.sample = pl("Za~z~o~l~c ja~x~n");
          chip.selected = f == 0;
          v.fonts.push_back(chip);
        }
        for (int type = 0; type < 2; ++type) {
          ThemesView::LayoutChip chip;
          chip.id = 80 + type;
          chip.compact = type == 0;
          chip.selected = type == 1;
          chip.railRight = false;
          chip.name = type == 0 ? "Same ikony" : "Ikony + napisy";
          chip.detail = chip.selected ? String("Z lewej") : pl("Dotknij, by wybra~c");
          v.layouts.push_back(chip);
        }
        v.layoutHint = pl("Dotknij wybranego jeszcze raz: pasek przejdzie na drug~a stron~e");
        paintThemes(d, sink, v);
      });
    }

    frame("device" + suffix, 3, [&] {
      std::vector<Tile> tiles = {
          tile(90, "Karta SD", pl("24 ksi~a~zek"), Icon::SdCard),
          tile(92, "Aktualizuj", "v0.3.61", Icon::Download, true),
          tile(93, "USB", pl("Przesy~lanie plik~ow"), Icon::Usb),
          tile(94, "Aplikacja", "Wi-Fi + QR", Icon::Phone),
          tile(95, "Bluetooth", pl("W~l~aczony"), Icon::Bluetooth),
          tile(96, "Wi-Fi", "Dom_5G", Icon::Wifi),
          tile(97, "Samouczek", "", Icon::Help),
          tile(98, pl("Wy~l~acz"), "", Icon::Power),
      };
      paintTileGrid(d, sink, tabContent(), tiles, 4, 2);
    });

    frame("plugins" + suffix, 4, [&] {
      std::vector<Tile> tiles = {
          tile(100, "Dyktafon", "", Icon::Record),
          tile(101, "Klepsydra", "", Icon::Hourglass),
          tile(102, "RSS", "", Icon::List),
          tile(104, "Biblioteka funkcji", pl("8 dost~epne"), Icon::Books),
      };
      paintTileGrid(d, sink, tabContent(), tiles, 3, 2);
    });

    frame("library" + suffix, 0, [&] {
      ShelfView v;
      v.header.backId = 109;
      v.header.title = "Biblioteka";
      v.header.trailing = "24";
      v.header.pillId = 110;
      v.header.pillLabel = "Ostatnio czytane";
      v.header.pillIcon = Icon::Sort;
      const char *titles[] = {"Pan Tadeusz", "Lalka", "Quo Vadis", "Ferdydurke", "Solaris",
                              "Wiedzmin",    "Dziady", "Chlopi",   "Kordian",    "Potop"};
      for (int i = 0; i < 10; ++i) {
        ShelfBook book;
        book.title = titles[i];
        book.progress = static_cast<uint8_t>(i * 11 % 100);
        v.books.push_back(book);
      }
      v.selected = 2;
      const ShelfGeometry g = shelfGeometry();
      v.offset = shelfCenteredOffset(v.books.size(), v.selected, g.viewport.w);
      v.detailTitle = "Quo Vadis";
      v.detailAuthor = "Henryk Sienkiewicz";
      v.detailStatus = pl("Bie~z~aca");
      v.detailPercent = "22%";
      paintShelf(d, sink, v);
    });

    frame("chapters" + suffix, 0, [&] {
      WheelView v;
      v.header.backId = 120;
      v.header.title = pl("Rozdzia~ly");
      v.header.trailing = "5 / 12";
      v.count = 12;
      v.centered = 4;
      v.readingIndex = 4;
      v.firstIndex = 0;
      for (int i = 0; i < 9; ++i) {
        v.titles.push_back(pl("Ksi~ega ") + String(i + 1) + pl(": Gospodarstwo i ~lowy"));
      }
      paintWheel(d, sink, v);
    });

    frame("savepoints" + suffix, 0, [&] {
      ListView v;
      v.header.backId = 130;
      v.header.title = "Punkty zapisu";
      v.header.page = 0;
      v.header.pageCount = 2;
      v.header.prevId = 138;
      v.header.nextId = 139;
      v.columns = 1;
      v.rows = 3;
      ListItem add;
      add.id = 131;
      add.label = "Dodaj punkt zapisu";
      add.icon = Icon::Plus;
      v.items.push_back(add);
      for (int i = 0; i < 2; ++i) {
        ListItem row;
        row.kind = ListItem::Kind::Row;
        row.id = 132 + i;
        row.label = i == 0 ? pl("Ksi~ega III, scena w lesie") : pl("Rozdzia~l 7");
        row.icon = Icon::Bookmark;
        row.trailingId = 140 + i;
        row.trailingLabel = pl("Usu~n");
        v.items.push_back(row);
      }
      paintList(d, sink, v);
    });

    frame("display_list" + suffix, -1, [&] {
      ListView v;
      v.header.backId = 150;
      v.header.title = pl("Wy~swietlacz");
      v.fullScreen = true;
      v.columns = 2;
      v.rows = 3;
      auto setting = [&](int id, const String &label, const String &value) {
        ListItem it;
        it.kind = ListItem::Kind::Setting;
        it.id = id;
        it.label = label;
        it.value = value;
        v.items.push_back(it);
      };
      ListItem bright;
      bright.kind = ListItem::Kind::Slider;
      bright.id = 151;
      bright.label = pl("Jasno~s~c");
      bright.value = "70%";
      bright.sliderValue = 6;
      bright.sliderMin = 0;
      bright.sliderMax = 9;
      v.items.push_back(bright);
      setting(152, "Motyw", "Ciemny");
      setting(153, pl("Kolor wyr~o~znienia"), "Niebieski");
      setting(154, "Orientacja", pl("Pozioma, odwr~ocona"));
      ListItem t;
      t.kind = ListItem::Kind::Toggle;
      t.id = 155;
      t.label = pl("Poka~z bateri~e podczas czytania");
      t.on = true;
      v.items.push_back(t);
      setting(156, "Tryb nawigacji", "Nowoczesny");
      paintList(d, sink, v);
    });

    frame("confirm" + suffix, -1, [&] {
      ConfirmView v;
      v.question = pl("Usun~a~c ksi~a~zk~e \"Quo Vadis\" z karty?");
      v.detail = pl("Punkty zapisu trafi~a do kosza");
      v.backId = 160;
      v.backLabel = "Anuluj";
      ConfirmView::Action action;
      action.id = 161;
      action.label = pl("Usu~n");
      action.danger = true;
      v.actions.push_back(action);
      paintConfirm(d, sink, v);
    });

    frame("details" + suffix, -1, [&] {
      BookDetailsView v;
      v.header.backId = 170;
      v.header.title = "Quo Vadis";
      v.author = "Henryk Sienkiewicz";
      v.percentLabel = "22%";
      v.percent = 22;
      v.actions = {tile(171, "Czytaj dalej", "", Icon::Play, true), tile(172, pl("Rozdzia~ly"), "", Icon::List),
                   tile(173, pl("Od pocz~atku"), "", Icon::Restart), tile(174, pl("Usu~n z karty"), "", Icon::Trash)};
      paintBookDetails(d, sink, v);
    });

    frame("reader_panel" + suffix, -1, [&] {
      ReaderPanelView v;
      v.chapter = pl("Ksi~ega pierwsza: Gospodarstwo");
      v.progressLabel = "42%";
      v.timeLeft = "3 h 12 min";
      v.progressPercent = 42;
      v.before = pl("kt~ory");
      v.word = pl("przeczyta~l");
      v.after = pl("ksi~a~zk~e");
      v.menuId = 180;
      // go to: the status line (statusId)
      v.statusId = 187;
      v.rewindId = 188;
      v.lookId = 189;
      v.bookmarkId = 182;
      v.minusId = 183;
      v.wpmId = 184;
      v.plusId = 185;
      v.startId = 186;
      v.menuLabel = "Menu";
      v.wpmLabel = "350 WPM";
      v.startLabel = "Czytaj";
      v.hint = pl("Przytrzymaj, by czyta~c  -  przesu~n w bok, by przewin~a~c");
      paintReaderPanel(d, sink, v);
    });

    // Wyglad czytania: every segment, in the dark and the light reading theme.
    for (int theme = 0; theme < 2; ++theme) {
      d.setDarkMode(theme == 0);
      for (int section = 0; section < TypographyView::kSections; ++section) {
        frame("typography" + std::to_string(section) + (theme == 0 ? "" : "_light") + suffix, -1, [&] {
          TypographyView v;
          v.backId = 200;
          v.section = section;
          const String segs[] = {"Kolory", "Tekst", "Prowadnica"};
          for (int i = 0; i < TypographyView::kSections; ++i) {
            v.segmentIds[i] = 201 + i;
            v.segmentLabels[i] = segs[i];
          }
          v.before = pl("kt~ory");
          v.word = pl("przeczyta~l");
          v.after = pl("ksi~a~zk~e");
          v.fontSizeLevel = 0;
          auto item = [](ListItem::Kind kind, int id, const String &label, const String &value) {
            ListItem it;
            it.kind = kind;
            it.id = id;
            it.label = label;
            it.value = value;
            return it;
          };
          if (section == 0) {
            const char *names[] = {"Ciemny", "Jasny", "Nocny"};
            for (uint8_t t = 0; t < 3; ++t) {
              TypographyView::ThemeChip chip;
              chip.id = 210 + t;
              chip.theme = t;
              chip.name = names[t];
              chip.selected = t == (theme == 0 ? 0 : 1);
              v.themes.push_back(chip);
            }
            v.letterColorId = 214;
            v.letterColorLabel = "Kolor litery";
            v.letterColor = d.focusColorFor(false);
            ListItem hl = item(ListItem::Kind::Toggle, 215, pl("Wyr~o~znij liter~e"), "");
            hl.on = true;
            v.items.push_back(hl);
          } else if (section == 1) {
            ListItem face = item(ListItem::Kind::Button, 220, "Atkinson Hyperlegible", "");
            face.icon = Icon::Font;
            v.items.push_back(face);
            ListItem size = item(ListItem::Kind::Slider, 221, "Rozmiar", pl("Du~zy"));
            size.sliderMax = 2;
            size.sliderValue = 2;
            v.items.push_back(size);
            ListItem track = item(ListItem::Kind::Slider, 222, pl("Odst~ep liter"), "0 px");
            track.sliderMin = -3;
            track.sliderMax = 6;
            track.sliderValue = 0;
            v.items.push_back(track);
            ListItem nb = item(ListItem::Kind::Toggle, 223, pl("S~lowa obok"), "");
            nb.on = true;
            v.items.push_back(nb);
          } else {
            ListItem side = item(ListItem::Kind::Setting, 234, "Strona", "Lewa");
            v.items.push_back(side);
            ListItem pos = item(ListItem::Kind::Slider, 230, pl("Pozycja s~lowa"), "35%");
            pos.sliderMin = 20;
            pos.sliderMax = 60;
            pos.sliderValue = 35;
            v.items.push_back(pos);
            ListItem len = item(ListItem::Kind::Slider, 231, pl("D~lugo~s~c linii"), "20 px");
            len.sliderMin = 0;
            len.sliderMax = 60;
            len.sliderValue = 20;
            v.items.push_back(len);
            ListItem gap = item(ListItem::Kind::Slider, 232, "Przerwa w linii", "0 px");
            gap.sliderMax = 10;
            v.items.push_back(gap);
            ListItem reset = item(ListItem::Kind::Button, 233, "", "");
            reset.icon = Icon::Restart;
            v.items.push_back(reset);
          }
          paintTypography(d, sink, v);
        });
      }
    }
    d.setDarkMode(true);
  }

  d.setNanoUiFont(0);
  d.renderStatus("Synchronizacja", "Uruchamianie Wi-Fi", pl("Prosz~e czeka~c"));
  dump(d, "status");
  static bool qr[29 * 29];
  for (int i = 0; i < 29 * 29; ++i) qr[i] = ((i * 7919) % 13) < 6;
  d.renderStatusWithQr("Wi-Fi", "Flower-3A7F", qr, 29, pl("Zeskanuj kod w aplikacji Flower na telefonie"),
                       pl("Zako~ncz"));
  dump(d, "qr");
}
