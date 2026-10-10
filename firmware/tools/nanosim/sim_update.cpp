// Screens of the 2026-09-27 update (go to, bookmark name choice, battery
// styles, scroll-mode reader panel, ...).
#include <string>
#include <vector>

#include "display/DisplayManager.h"
#include "ui/NanoScreens.h"
#include "app/generated/HelpData.h"

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

}  // namespace

void runUpdateScreens(DisplayManager &d, void (*dump)(const DisplayManager &, const char *)) {
  SimSink sink;
  auto frame = [&](const std::string &name, auto paint) {
    sink.rects.clear();
    d.nanoBeginFrame();
    paint();
    d.nanoEndFrame();
    dump(d, name.c_str());
    for (const Rect &r : sink.rects) d.nanoDrawRect(r.x, r.y, r.w, r.h, 0xF81F);
    dump(d, (name + "_targets").c_str());
  };
  auto panel = [&](bool scroll, uint8_t sizeLevel = 0) {
    ReaderPanelView v;
    v.fontSizeLevel = sizeLevel;
    v.lookId = 189;
    v.chapter = pl("Ksi~ega pierwsza: Gospodarstwo");
    v.progressLabel = "42%";
    v.timeLeft = "3 h 12 min";
    v.progressPercent = 42;
    v.before = pl("kt~ory");
    v.word = pl("przeczyta~l");
    v.after = pl("ksi~a~zk~e");
    v.scrollMode = scroll;
    const char *text[] = {"Litwo!", "Ojczyzno", "moja!", "ty", "jeste~s", "jak", "zdrowie.", "Ile", "ci~e",
                          "trzeba", "ceni~c,", "ten", "tylko", "si~e", "dowie,", "kto", "ci~e", "straci~l.",
                          "Dzi~s", "pi~eknos~c", "tw~a", "w", "ca~lej", "ozdobie", "widz~e", "i", "opisuj~e,",
                          "bo", "t~eskni~e", "po", "tobie.", "Panno", "~swi~eta,", "co", "Jasnej", "bronisz",
                          "Cz~estochowy", "i", "w", "Ostrej", "~swiecisz", "Bramie!"};
    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); ++i) {
      DisplayManager::ContextWord w;
      w.text = pl(text[i]);
      w.paragraphStart = i == 0;
      v.words.push_back(w);
    }
    v.currentLocal = 17;
    v.menuId = 180;
    // go to: the status line (statusId)
    v.statusId = 187;
    v.rewindId = 188;
    v.bookmarkId = 182;
    v.minusId = 183;
    v.wpmId = 184;
    v.plusId = 185;
    v.startId = 186;
    v.menuLabel = "Menu";
    v.wpmLabel = "350 WPM";
    v.startLabel = "Czytaj";
    v.hint = scroll ? String() : pl("Przytrzymaj, by czyta~c  -  przesu~n w bok, by przewin~a~c");
    paintReaderPanel(d, sink, v);
  };
  frame("u_panel_rsvp", [&] { panel(false); });
  frame("u_panel_rsvp_medium", [&] { panel(false, 1); });
  frame("u_panel_rsvp_small", [&] { panel(false, 2); });
  frame("u_panel_scroll", [&] { panel(true); });
  d.setDarkMode(false);
  frame("u_panel_scroll_light", [&] { panel(true); });
  d.setDarkMode(true);

  for (int segment = 0; segment < 3; ++segment) {
    frame("u_goto" + std::to_string(segment), [&] {
      GoToView v;
      v.header.backId = 0;
      v.header.title = pl("Przejd~x do");
      v.segmentIds[0] = 1;
      v.segmentIds[1] = 2;
      v.segmentIds[2] = 3;
      v.segmentLabels[0] = "Procent";
      v.segmentLabels[1] = "Strona";
      v.segmentLabels[2] = pl("Rozdzia~l");
      v.segment = segment;
      v.detail = pl("Ksi~ega pi~ata: Rada");
      if (segment == 0) {
        v.value = "42%";
        v.hint = "Strona 120 z 286";
        v.sliderMax = 100;
        v.sliderValue = 42;
      } else if (segment == 1) {
        v.value = "120 / 286";
        v.hint = pl("1 strona = 250 s~l~ow  -  42%");
        v.sliderMin = 1;
        v.sliderMax = 286;
        v.sliderValue = 120;
      } else {
        v.value = "5 / 12";
        v.hint = "Strona 120 z 286  -  42%";
        v.sliderMax = 11;
        v.sliderValue = 4;
      }
      v.minusId = 4;
      v.plusId = 5;
      v.readId = 6;
      v.readLabel = pl("Czytaj st~ad");
      paintGoTo(d, sink, v);
    });
  }

  frame("u_bookmark_choice", [&] {
    ChoiceView v;
    v.header.backId = 0;
    v.header.title = pl("Nazwij zak~ladk~e");
    v.question = pl("Jak nazwa~c zak~ladk~e?");
    ChoiceView::Option a;
    a.id = 1;
    a.label = pl("Domy~slna nazwa");
    a.detail = pl("42.3% Ksi~ega pierwsza");
    a.icon = Icon::Bookmark;
    a.accent = true;
    ChoiceView::Option b;
    b.id = 2;
    b.label = pl("W~lasna nazwa");
    b.detail = "Wpisz na klawiaturze";
    b.icon = Icon::Edit;
    v.options = {a, b};
    paintChoice(d, sink, v);
  });

  frame("u_colors", [&] {
    ColorPickerView v;
    v.header.backId = 0;
    v.header.title = "Kolor litery";
    const uint8_t hues[12][3] = {{235, 30, 40},  {250, 120, 20}, {250, 185, 20}, {245, 230, 30},
                                 {150, 220, 30}, {30, 200, 70},  {20, 190, 160}, {30, 180, 240},
                                 {20, 80, 255},  {100, 60, 240}, {170, 50, 235}, {240, 50, 150}};
    auto rgb = [](int r, int g, int b) { return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); };
    for (int row = 0; row < 3; ++row) {
      for (int h = 0; h < 12; ++h) {
        int r = hues[h][0], g = hues[h][1], b = hues[h][2];
        if (row == 1) { r += (255 - r) * 45 / 100; g += (255 - g) * 45 / 100; b += (255 - b) * 45 / 100; }
        if (row == 2) { r = r * 62 / 100; g = g * 62 / 100; b = b * 62 / 100; }
        ColorPickerView::Swatch s;
        s.id = row * 12 + h + 1;
        s.color = rgb(r, g, b);
        s.selected = row == 0 && h == 8;
        v.swatches.push_back(s);
      }
    }
    v.previewBackground = 0;
    v.previewWord = 0xFFFF;
    v.previewFocus = rgb(20, 80, 255);
    paintColorPicker(d, sink, v);
  });

  frame("u_help", [&] {
    HelpView v;
    v.header.backId = 0;
    v.header.title = HelpData::title(HelpTopic::BatteryStyle, 5);
    v.lines = DisplayManager::nanoWrapText(HelpData::body(HelpTopic::BatteryStyle, 5), helpTextWidth(), 2);
    v.header.pageCount = 2;
    v.header.prevId = 1;
    v.header.nextId = 2;
    paintHelp(d, sink, v);
  });

  frame("u_display_help", [&] {
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
      it.helpId = id + 100;
      v.items.push_back(it);
    };
    ListItem bright;
    bright.kind = ListItem::Kind::Slider;
    bright.id = 151;
    bright.label = pl("Jasno~s~c");
    bright.value = "70%";
    bright.sliderValue = 70;
    bright.sliderMin = 20;
    bright.sliderMax = 100;
    bright.helpId = 251;
    v.items.push_back(bright);
    setting(154, "Orientacja", pl("Pozioma, odwr~ocona"));
    setting(155, pl("Wska~xnik baterii"), "Ikona + %");
    setting(156, "Stopka", "Procent");
    setting(157, "Bateria", "Procent");
    setting(158, "Wygaszacz", "Gwiazdy");
    paintList(d, sink, v);
  });

  // Battery indicator styles, charging and not, in the rail footer size.
  frame("u_battery", [&] {
    for (int style = 0; style < 4; ++style) {
      d.setBatteryStyle(static_cast<uint8_t>(style));
      d.setBatteryState(true, 76, false);
      d.nanoBatteryInline(Rect(20, 10 + style * 38, 140, 22));
      d.setBatteryState(true, 58, true);
      d.nanoBatteryInline(Rect(200, 10 + style * 38, 140, 22));
      d.setBatteryState(true, 12, false);
      d.nanoBatteryInline(Rect(380, 10 + style * 38, 140, 22));
      d.nanoBatteryInline(Rect(540, 10 + style * 38, 60, 22), true);
    }
    d.setBatteryStyle(0);
    d.setBatteryState(true, 76, false);
  });
}
