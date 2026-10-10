# Ekrany i przyciski — mapa dla Figmy

Ekran urządzenia: **640 x 172 px**, landscape, obrócony o 180 stopni (`BoardConfig::UI_ROTATED_180 = true`, żeby BOOT/PWR zostały u góry). Panel fizyczny ma natywną orientację 172x640 (pionową) — sterownik go obraca. Źródło: `firmware/src/board/BoardConfig.h` (`DISPLAY_WIDTH = 640`, `DISPLAY_HEIGHT = 172`).

Poniżej mapa wszystkich ekranów firmware (dotykowy panel AXS15231B) oraz — krócej — paneli aplikacji-towarzysza PWA na telefonie. Dane pochodzą z odczytu `firmware/src/app/App.cpp` (8690 linii), `App.h` i `DisplayManager.h/.cpp`. Gdzie kod nie precyzuje pozycji px, jest to zaznaczone wprost.

Wspólne elementy layoutu list (dotyczy większości ekranów menu, funkcja `DisplayManager::renderMenu`):
- Wysokość wiersza: 22px (`kCompactMenuRowHeight`), lista wyśrodkowana pionowo, przewija się tak by zaznaczony wiersz był widoczny.
- Strzałka "<" w lewym górnym rogu (4,4) — wskazówka wizualna, nie zawsze aktywny przycisk (patrz sekcja nawigacji).
- Zaznaczona pozycja: pasek koloru akcentu po lewej + tekst w kolorze fokusu.
- Odznaka baterii w prawym górnym rogu na każdym ekranie menu.
- Tryb D-Pad (opcja w Ustawieniach): dokłada 120px panel po prawej z przyciskami góra/dół/lewo/prawo/OK, obszar listy zwęża się do 520px.

Ekrany list biblioteki (`renderLibrary`, dwuwierszowe tytuł+podtytuł) używają innego row-height (biblioteka ma większe wiersze niż compact-menu, brak "<" w rogu).

---

## Ekrany urządzenia (firmware, 640x172px)

### Ekran startowy / Boot (AppState::Booting)
Cel: splash przy uruchomieniu.
- Brak przycisków dotykowych — animacja `renderBootSplash()`, potem napis "READY".
Typ layoutu: pełnoekranowa animacja/status, bez interakcji.

### Czytnik RSVP (AppState::Paused / Playing, tryb RSVP) — `renderReaderWord` / `renderActiveReader`
Cel: główny widok czytania, słowo po słowie z literą ORP podświetloną kolorem fokusu, słowa-widma (przygaszony tekst przed/po) po bokach.
- „<<" poprzednie zdanie — górny lewy róg, obszar dotyku x<38, y<30 — cofa do początku poprzedniego zdania.
- „SP" punkt zapisu — obok poprzedniego, x w [38,120), y<30, widoczny tylko gdy `savePointButtonVisible_` — pauzuje i otwiera klawiaturę do nazwania zakładki.
- Odznaka baterii — prawy górny róg, x ≥ 640-160, y≤40 — tap cykluje etykietę (Procent → Czas pozostały → Napięcie).
- Metryka stopki (prawy dolny róg, x ≥ 640-220, y ≥ 172-32, aktywna tylko gdy NIE czyta aktywnie) — tap cykluje (Procent → Czas rozdziału → Czas książki).
- Stopka (dół ekranu): tytuł rozdziału (lewo) + metryka (prawo) — widoczność zależy od ustawień (Bateria/Rozdział/Procent podczas czytania).
- Środek ekranu: bieżące słowo RSVP, litera-kotwica podświetlona.
- Gesty (bez osobnych przycisków, cała powierzchnia ekranu):
  - Przytrzymanie ~420ms (poza rogami) → odtwarzanie na czas przytrzymania („hold to read").
  - Podwójny tap w tym samym miejscu (okno 520ms, tolerancja 92px) → blokada odtwarzania/pauzy (play-lock).
  - Poziomy swipe (≥40px) → przewijanie/scrub po słowach, otwiera podgląd kontekstu.
  - Pionowy swipe (gdy nie w podglądzie) → zmiana WPM, pokazuje nakładkę z nową wartością.
  - Przytrzymanie + pionowy ruch w trybie podglądu kontekstu → przewijanie tekstu (browse scroll).
Typ layoutu: pełnoekranowy widok tekstu + niewidoczne strefy dotykowe w rogach/krawędziach.

### Czytnik — tryb Przewijanie/Scroll (ten sam AppState, `renderScrollReader`)
Cel: alternatywa dla RSVP — ciągły przewijany tekst z podświetlonym bieżącym słowem.
- Te same strefy dotykowe co RSVP (poprzednie zdanie, SP, bateria, metryka stopki).
- Te same gesty scrub/WPM/hold-to-read działają identycznie, tu manipulują pozycją przewijania.
Typ layoutu: przewijany tekst wielowierszowy + ta sama chrome (nagłówek/stopka) co RSVP.

### Podgląd kontekstu (`renderContextPreview` / `renderContextBrowsePreview`)
Cel: tymczasowy widok akapitu wokół aktualnego słowa, pokazywany podczas przewijania/scrub (nie osobny ekran menu, tylko tryb wyświetlania czytnika).
Typ layoutu: przewijalny blok tekstu, tap poza trybem przewijania zamyka podgląd i wraca do czytnika.

### Nakładka WPM (`renderWpmFeedback`)
Cel: pokazuje nową wartość WPM przez ~900ms po pionowym swipe. Bez przycisków — znika automatycznie.

### Przejście rozdziału (`renderChapterTransition`)
Cel: karta "ROZDZIAŁ N" + tytuł, widoczna ~1.4s przy przekroczeniu znacznika rozdziału podczas odtwarzania. Bez przycisków.

### Ostrzeżenie o baterii (overlay, nie osobny MenuScreen)
Cel: status "Bateria/Wyłączanie" przy krytycznym/niskim poziomie. Bez przycisków, znika po ~2.5s.

---

### Główne menu (MenuScreen::Main) — `renderMainMenu`
Cel: punkt wejścia do menu (otwierany krótkim naciśnięciem przycisku BOOT/Reboot z czytnika).
Lista (od góry):
- [warunkowo] „>> Update vX.Y.Z" — pojawia się tylko gdy dostępna aktualizacja OTA — od razu uruchamia `runFirmwareUpdate` (bez ekranu potwierdzenia, patrz uwaga niżej).
- „Czytaj" — zamyka menu, wraca do czytnika (Paused).
- „Biblioteka" — otwiera BookPicker.
- „Punkty zapisu" — otwiera SavePointsList.
- „Ustawienia" — otwiera SettingsHome.
- „Pluginy" — otwiera PluginsList.
- „Wyłącz" — usypia urządzenie (deep sleep).
Typ layoutu: prosta lista przycisków (compact menu). Potrójny tap przycisku BOOT/Reboot na tym ekranie tworzy szybki punkt zapisu (skrót, nie przycisk na ekranie).

### SettingsHome — `renderSettings`
Cel: hub ustawień.
- Wróć — do Main.
- Czytanie — otwiera SettingsPacing.
- Wyświetlanie — otwiera SettingsDisplay.
- Typografia — otwiera TypographyTuning.
- Połączenia — otwiera SettingsConnectivity.
- Presety — otwiera Presets.
- O aplikacji/Pomoc — otwiera SettingsAbout.
- [tylko dev mode] Wi-Fi zaawansowane — otwiera WifiSettings.
- [tylko dev mode] Aktualizacja firmware — uruchamia sprawdzanie/instalację OTA.
Typ layoutu: lista. Zaznaczona pozycja może mieć dopisane „ ?" gdy dostępna podpowiedź (podpowiedź otwiera kółko ?, BOOT/Reboot cofa o ekran).

### SettingsDisplay — `renderSettings` (menuScreen SettingsDisplay)
Lista przełączników/cykli:
- Wróć — do SettingsHome.
- Motyw — cykl Jasny/Ciemny/Nocny.
- Jasność — cykl 5 poziomów (40/55/70/85/100%).
- Ręka czytająca — cykl Prawa/Lewa (przesuwa strefy dotyku i kotwicę).
- Etykieta stopki — cykl Procent/Czas rozdziału/Czas książki.
- Etykieta baterii — cykl Procent/Czas/Napięcie.
- Wygaszacz — otwiera ScreensaverSettings.
- Bateria podczas czytania — wł/wył.
- Rozdział podczas czytania — wł/wył.
- Procent podczas czytania — wł/wył.
- Język — cykl 6 języków UI.
- Kolor litery (focus) — cykl 6 kolorów.
- Przycisk zapisu (SP) — wł/wył widoczność w czytniku.
- Pomoc (?) — wł/wył podpowiedzi.
- Nawigacja — przełącznik Swipe / D-Pad.
Typ layoutu: lista (14 pozycji + Wróć), przewijana.

### SettingsPacing — „Czytanie"
Wróć — do SettingsHome.
Tryb czytania — cykl RSVP/Scroll.
Gdy RSVP:
- Zachowanie pauzy — cykl Koniec zdania/Natychmiast.
- Tempo bazowe — cykl WPM (10-100 co 10, potem 100-1000 co 25).
- Długie słowa — opóźnienie 0-600ms co 50.
- Złożoność — opóźnienie 0-600ms co 50.
- Interpunkcja — opóźnienie 0-600ms co 50.
- Reset tempa — przywraca domyślne opóźnienia (200/200/200ms).
Gdy Scroll (te same indeksy 2-5 zastępowane):
- Rozmiar czcionki — cykl 0-8.
- Interlinia — cykl Compact/Normal/Relaxed.
- Marginesy — cykl Narrow/Normal/Wide.
- Podgląd — pokazuje przykładowy tekst przewijany przez 1.5s z aktualnymi ustawieniami.
Typ layoutu: lista, zawartość zmienia się zależnie od trybu czytania.

### SettingsConnectivity — „Połączenia"
- Wróć — do SettingsHome.
- Wi-Fi: <SSID lub „nie ustawiono"> — otwiera WifiSettings (skanowanie).
- [tylko gdy FLOWER_BLE_ENABLED] Bluetooth: wł/wył — przełącza radio BLE.
- Synchronizacja z telefonem: wł/wył — wchodzi/wychodzi z trybu CompanionSync.
- Kanały RSS — przekierowuje do PluginsList (funkcja RSS przeniesiona do pluginu).
- [tylko gdy RSVP_USB_TRANSFER_ENABLED] Kopiuj przez USB — wchodzi w UsbTransfer.
Typ layoutu: lista.

### SettingsAbout — „O aplikacji / Pomoc"
- Wróć — do SettingsHome.
- Wersja: vX.Y.Z — 10x szybki tap odblokowuje tryb developera (ukryty easter egg, analogicznie do Androida).
- Etykieta marki — informacyjna, bez akcji.
- Sprawdzenie karty SD — uruchamia diagnostykę SD.
- Samouczek — restartuje tutorial od kroku 1.
- [tylko dev mode] Tryb developera: WŁ — tap wyłącza tryb developera.
Typ layoutu: lista.

### ScreensaverSettings — „Wygaszacz"
- Wróć — do SettingsDisplay.
- Styl — cykl: Życie (Conway) / Labirynt / Voronoi / Gwiazdy / Matrix / Ekran wyłączony.
- Czas do wygaszacza — cykl 1/2/3/5/10/15/20/30 min.
- Auto-wyłączenie zasilania — cykl Nigdy/5/10/15/20/30/45/60 min (usypia urządzenie po tym czasie w wygaszaczu).
- Ochrona snu (sleep guard) — cykl Wył./5/10/.../60 min — pauzuje czytanie i wchodzi w wygaszacz jeśli brak dotyku podczas aktywnego czytania.
- Podgląd — natychmiast wchodzi w tryb Standby żeby zobaczyć wygaszacz na żywo.
Typ layoutu: lista.

### WifiSettings
- Wróć — do SettingsHome (lub do PluginsList jeśli otwarto stąd, np. z ekranu biblioteki pluginów).
- Sieć: <SSID> — tap uruchamia skanowanie.
- Wybierz sieć — uruchamia skanowanie → WifiNetworks.
- Zapomnij sieć — czyści zapisane dane logowania.
- [tylko dev mode] Auto OTA: Tak/Nie — przełącznik.
- [tylko dev mode] OTA Owner: <wartość> — otwiera klawiaturę tekstową.
Typ layoutu: lista.

### WifiNetworks — `renderLibrary`
Cel: lista zeskanowanych sieci (posortowana: zapisana sieć najpierw, potem po sile sygnału).
- Wróć — pozycja 0.
- Każda sieć: tytuł = SSID, podtytuł = „Secure/Open  X dBm". Tap: jeśli sieć wymaga hasła → otwiera klawiaturę (TextEntry, maskowane pole); jeśli otwarta → zapisuje od razu.
Typ layoutu: lista dwuwierszowa (tytuł+podtytuł), bez strzałki "<" w rogu (styl biblioteki).

### TextEntry — klawiatura ekranowa
Cel: wspólny ekran do wpisywania tekstu (hasło Wi-Fi, OTA owner, nazwa punktu zapisu, nazwa presetu). Nie ma własnej pozycji w MenuScreen dla „SavePointNameEntry" — to ten sam ekran z innym `TextEntryPurpose`.
- Pasek tytułu/prompt/wpisywana wartość (maskowana gwiazdkami dla haseł) na górze.
- 3 rzędy liter/cyfr (qwertyuiop / asdfghjkl / zxcvbnm — lub wersje ABC/symbole), pozycje liczone dynamicznie na szerokość 640px, margines 8px, wysokość rzędu 27px + 4px odstęp.
- Rząd sterujący (dół): „abc" / „ABC" / „123" (przełącz tryb klawiatury) / „space" / „back" (backspace) / „clear" lub „show"/„hide" (dla pól maskowanych) / „save" (akcent) / „cancel".
Typ layoutu: pełnoekranowa klawiatura dotykowa, tap = natychmiastowa akcja (bez potwierdzania osobnym gestem).

### TypographyTuning — „Typografia"
Cel: ekran podglądu i strojenia typografii czytnika, inny layout niż lista — pokazuje realny przykład słowa.
- Nagłówek „Typografia N/9" (numer próbki).
- Słowo przed / bieżące (renderowane z aktualnymi ustawieniami) / słowo po — przykład na żywo.
- Linia wartości: nazwa ustawienia + aktualna wartość.
- Linia podpowiedzi (zależna od wybranej pozycji: „tap by zmienić próbkę" / „tap by przełączyć" / „tap by zresetować" itd.).
Nawigacja: pionowy swipe przełącza pozycję ustawienia (patrz lista niżej), poziomy swipe zmienia słowo-próbkę (9 próbek), tap w lewym górnym rogu (x<80,y<35) = Wróć.
Pozycje ustawień (cyklicznie, tap = zastosuj/zmień):
- Wróć — do SettingsHome.
- Rozmiar czcionki — cykl S/M/L.
- Krój pisma — cykl Standard/Atkinson/OpenDyslexic.
- Słowa widma — wł/wył.
- Podświetlenie fokusowe — wł/wył.
- Tracking (odstępy) — -2..+3 px.
- Pozycja kotwicy — 30-40% (+20% offsetu gdy lewa ręka).
- Szerokość prowadnicy — 12-30px co 2.
- Przerwa prowadnicy — 2-8px.
- Reset — przywraca domyślną typografię.
Typ layoutu: pojedynczy podgląd + wartość, nie lista.

### BookPicker — „Biblioteka" — `renderLibrary`
Cel: lista książek/artykułów z karty SD (posortowana: aktualnie czytana → ostatnio otwierane → reszta).
- Wróć — pozycja 0.
- Każda pozycja: tytuł + podtytuł (autor i/lub procent ukończenia, albo „aktualna książka"). Tap → otwiera BookDetails.
Typ layoutu: lista dwuwierszowa (biblioteka).

### BookDetails
- Wróć — do BookPicker.
- Tytuł + autor — informacyjne.
- Procent ukończone — informacyjne.
- „Czytaj od miejsca" — wczytuje książkę, wraca do czytnika w zapisanym miejscu (domyślnie zaznaczona pozycja).
- „Rozdziały" — otwiera ChapterPicker.
- „Od nowa" — otwiera RestartConfirm.
- „Usuń książkę" — otwiera BookDeleteConfirm.
Typ layoutu: lista.

### BookDeleteConfirm
- Wróć — do BookDetails.
- „Usunąć: <tytuł>" — informacyjne.
- „Nie, wróć" — domyślnie zaznaczone, wraca do BookDetails.
- „Tak, usuń" — usuwa plik i czyści zapisany postęp, wraca do BookPicker.
Typ layoutu: lista potwierdzenia.

### ChapterPicker
- Wróć — do BookDetails (lub Main jeśli brak kontekstu).
- Lista rozdziałów (numer + tytuł, gwiazdka „*" przy aktualnym) — tap przeskakuje do rozdziału.
- Jeśli brak znaczników rozdziałów: pozycja zastępcza „Początek książki".
- Ostatnia pozycja: „Od nowa" — otwiera RestartConfirm.
Typ layoutu: lista przewijalna.

### SavePointsList — „Punkty zapisu"
- Wróć — do Main.
- „+ Dodaj punkt zapisu" — otwiera klawiaturę (TextEntry, nazwa punktu) jeśli książka jest otwarta.
- Dla każdego zapisanego punktu: nazwa (tap = wczytaj książkę i przeskocz do miejsca) + „Usuń: <nazwa>" (tap = usuń od razu, bez dodatkowego potwierdzenia).
Typ layoutu: lista, pary wiersz-danych + wiersz-usuwania.

### PluginsList — „Pluginy"
- Wróć — do Main.
- Zainstalowane pluginy: „<Nazwa> vX" — tap uruchamia plugin (przejmuje ekran i dotyk, App tylko monitoruje watchdog).
- Separator „---" — nieaktywny wizualnie.
- „Biblioteka" — otwiera PluginLibraryScreen (wymaga Wi-Fi, jeśli brak — przekierowuje do WifiSettings).
Typ layoutu: lista.

### PluginLibraryScreen — biblioteka pluginów online
- Wróć — do PluginsList.
- Lista pluginów z rejestru zdalnego, etykieta „Zainstaluj" / „Aktualizuj" / „Zainstalowany" — tap otwiera PluginDetail.
Typ layoutu: lista.

### PluginDetail
- Wróć — do PluginLibraryScreen.
- 1-2 linie opisu (informacyjne, zawijane do ~35 znaków).
- „Zainstaluj" / „Aktualizuj" (ostatnia pozycja) — pobiera i instaluje plugin, wraca do biblioteki.
Typ layoutu: lista/karta szczegółów.

### RestartConfirm
- Nagłówek „Na pewno?" (nieklikalny, indeks 0).
- „Nie, zachowaj miejsce" — domyślnie zaznaczone, wraca do ekranu wywołującego.
- „Tak, zacznij od nowa" — resetuje czytnik do słowa 0.
Typ layoutu: lista potwierdzenia z nagłówkiem.

### SdCardRepairConfirm
- Nagłówek „Napraw foldery" (info).
- „Nie teraz" — anuluje, wraca do Main.
- „Utwórz foldery" — naprawia strukturę folderów na karcie SD.
Typ layoutu: lista potwierdzenia z nagłówkiem.

### UpdateConfirm
- Nagłówek „Dostępna aktualizacja".
- Linia wersji: „<obecna> -> <nowa>" (info).
- „Pomiń na razie" — wraca do czytnika.
- „Aktualizuj" — uruchamia `runFirmwareUpdate`.
Typ layoutu: lista potwierdzenia z nagłówkiem.
**Uwaga (niespójność w kodzie):** funkcja `openUpdateConfirm()` (App.cpp:6149) jest zdefiniowana, ale nigdzie w kodzie nie jest wywoływana — `maybeOpenUpdateConfirm()` to jawne no-op z komentarzem, że aktualizacja jest teraz obsługiwana przyciskiem „>> Update" bezpośrednio w Main menu. Ekran UpdateConfirm jest więc martwym kodem w obecnej wersji firmware — do Figmy warto go zaprojektować tylko jeśli planowany jest powrót do tego flow, w przeciwnym razie pominąć.

### WelcomeLanguage — kreator pierwszego uruchomienia, krok 1/5
Lista bez „Wróć" (pierwszy ekran): English, Polski, Deutsch, Español, Français, Română — tap wybiera język UI i przechodzi do WelcomeTheme.
Typ layoutu: lista bez przycisku wstecz.

### WelcomeTheme — krok 2/5
Lista: Jasny / Ciemny / Nocny — tap wybiera motyw, przechodzi do WelcomeHighlightColor.

### WelcomeHighlightColor — krok 3/5
Lista 6 kolorów (Czerwony/Niebieski/Zielony/Żółty/Pomarańczowy/Fioletowy) — tap wybiera kolor litery ORP, przechodzi do WelcomePacing.

### WelcomePacing — krok 4/5
Lista: Brak (0ms) / Lekkie (100ms) / Średnie (200ms, domyślne) / Mocne (300ms) / Bardzo mocne (400ms) — tap ustawia opóźnienie tempa, przechodzi do WelcomeConnect.

### WelcomeConnect — krok 5/5
- „Wi-Fi: <SSID urządzenia>" — informacyjne.
- „IP: 192.168.4.1" — informacyjne.
- separator „---" — informacyjne.
- „Połącz z telefonem" — zostawia punkt dostępu (AP) aktywny do parowania z aplikacją.
- „Pomiń na razie" — zamyka AP jeśli nikt się nie podłączył.
Tap na dowolnym z pierwszych 3 (informacyjnych) wierszy traktowany jest jak „Pomiń". Każdy wybór kończy kreator i uruchamia samouczek (TutorialStep1).
Typ layoutu: lista.

### TutorialStep1–5 — samouczek po kreatorze
Cel: 5-krokowe wprowadzenie po pierwszym uruchomieniu (osobne od tutorial-wizard w PWA — to wersja na urządzeniu).
- Karta pełnoekranowa: tytuł + opis + licznik „N/5".
- Cały ekran jest przyciskiem — tap przechodzi do kolejnego kroku, na kroku 5 kończy samouczek i wraca do czytnika.
Treść kroków: 1) RSVP — słowa jedno po drugim, litera ORP. 2) Tempo — przytrzymaj + góra/dół. 3) Pauza — dotknij ekranu. 4) Menu — przycisk boczny (PWR). 5) Pomoc — długie przytrzymanie bocznego przycisku (BOOT) w menu.
Typ layoutu: pełnoekranowa karta informacyjna, cały obszar klikalny.

### Presets
- Wróć — do SettingsHome.
- „+ Zapisz obecne" (lub „(Limit 10 osiągnięty)" jeśli pełny) — otwiera klawiaturę do nazwania presetu.
- Lista zapisanych presetów (nazwa) — tap otwiera PresetsDeleteConfirm (mimo nazwy, ten ekran służy też do zastosowania presetu, patrz niżej).
Typ layoutu: lista.

### PresetsDeleteConfirm
Cel: mimo nazwy w kodzie, to ekran szczegółów presetu z dwoma akcjami, nie tylko potwierdzenie usunięcia.
- Wróć — do Presets.
- „Zastosuj: <nazwa>" — wczytuje ustawienia z presetu.
- „Usuń: <nazwa>" — usuwa plik presetu.
Typ layoutu: lista.

### Standby (AppState::Standby) — wygaszacz ekranu
Cel: animacja oszczędzająca ekran po czasie bezczynności (styl wybierany w ScreensaverSettings: Życie/Labirynt/Voronoi/Gwiazdy/Matrix/Ekran wyłączony).
- Fazy: przyciemnienie (30 s, jedno dotknięcie budzi) -> pytanie „Wygasić ekran?" z licznikiem 10 s -> ekran zgaszony. Zgaszony ekran budzi dowolny przycisk albo dwa szybkie stuknięcia.
- Opcjonalny, delikatnie pulsujący tekst podpowiedzi (fade in/out co 10s).
Typ layoutu: pełnoekranowa animacja, zero interakcji dotykowej.

### CompanionSync (AppState::CompanionSync) — parowanie z telefonem
Cel: status z kodem QR do połączenia z aplikacją-towarzyszem (Wi-Fi AP).
- Nagłówek „< Wróć | Wi-Fi" — cały ekran jest przyciskiem: tap = wyjście z trybu synchronizacji.
- Kod QR (jeśli dostępny) lub tekst statusu.
- Alternatywnie: długie przytrzymanie PWR (1.2s) też wychodzi.
Typ layoutu: status/QR pełnoekranowy.

### UsbTransfer (AppState::UsbTransfer)
Cel: status trybu udostępniania karty SD jako pamięci masowej USB.
- Nagłówek „USB | Tap = wróć" — cały ekran to przycisk wyjścia (po odłączeniu hosta lub przytrzymaniu PWR 1.2s).
Typ layoutu: status pełnoekranowy.

### Sleeping (AppState::Sleeping)
Cel: ekran przejściowy przy zasypianiu — napis „SLEEP" (lub „OFF" przy pełnym wyłączeniu). Brak interakcji.

---

## Mapa nawigacji

```
Boot → (pierwsze uruchomienie) WelcomeLanguage → WelcomeTheme → WelcomeHighlightColor
     → WelcomePacing → WelcomeConnect → TutorialStep1 → ... → TutorialStep5 → Paused (czytnik)
Boot → (kolejne uruchomienia) Paused/Playing (czytnik)

Paused/Playing (czytnik) ⇄ Main            [BOOT/Reboot toggluje]
Paused/Playing → SavePointNameEntry (TextEntry)   [przycisk SP]
Paused/Playing ⇄ Standby                    [bezczynność / krótki PWR (od razu zgaszony ekran) / kombinacja BOOT+PWR]

Main → SettingsHome
Main → BookPicker (Biblioteka)
Main → SavePointsList
Main → PluginsList
Main → PowerOff (Sleeping/deep sleep)
Main → (jeśli pending OTA) uruchamia update bezpośrednio (bez ekranu UpdateConfirm)

SettingsHome → SettingsPacing (Czytanie)
SettingsHome → SettingsDisplay
SettingsHome → TypographyTuning
SettingsHome → SettingsConnectivity
SettingsHome → Presets
SettingsHome → SettingsAbout
SettingsHome → WifiSettings              [tylko dev mode]

SettingsDisplay → ScreensaverSettings
ScreensaverSettings → Standby            [Podgląd]

SettingsConnectivity → WifiSettings
SettingsConnectivity → CompanionSync
SettingsConnectivity → PluginsList        [Kanały RSS, przekierowanie]
SettingsConnectivity → UsbTransfer        [tylko gdy kompilacja z USB transfer]

SettingsAbout → TutorialStep1 (restart samouczka)

WifiSettings → WifiNetworks (skanowanie)
WifiNetworks → TextEntry (hasło Wi-Fi)     [jeśli sieć zabezpieczona]
WifiSettings → TextEntry (OTA owner)       [tylko dev mode]

BookPicker → BookDetails
BookDetails → ChapterPicker
BookDetails → RestartConfirm
BookDetails → BookDeleteConfirm
ChapterPicker → RestartConfirm (przez „Od nowa")

SavePointsList → TextEntry (nazwa punktu zapisu)

PluginsList → (uruchomienie pluginu, przejmuje ekran)
PluginsList → PluginLibraryScreen
PluginLibraryScreen → PluginDetail

Presets → TextEntry (nazwa presetu)
Presets → PresetsDeleteConfirm (Zastosuj/Usuń)

UpdateConfirm — martwy ekran, nigdy nie otwierany automatycznie (patrz uwaga wyżej)

Wszystkie ekrany z „Wróć" (indeks 0) → wracają do ekranu nadrzędnego wypisanego wyżej,
albo do Main jeśli brak głębszego kontekstu.
```

---

## Panele PWA (telefon/przeglądarka)

Aplikacja-towarzysz to osobna warstwa webowa (Vite + TypeScript + Lit), responsywna, nie 640x172px. Główny plik `src/app/app.element.ts` renderuje nagłówek (logo, status połączenia, odznaka DEV), treść widoku i dolny pasek nawigacji z 6 zakładkami: **Start / Książki / Konwerter / Pluginy / Aktualizacje / Więcej**.

### app.element.ts — powłoka aplikacji
- Ekran „Start" bez połączenia: wybór transportu (WiFi / Bluetooth / USB-Serial jako „tryb zaawansowany"), każdy z opisem wsparcia przeglądarki.
- Ekran „Start" po połączeniu: siatka kafelków skrótów (Książki / Konwerter / Pluginy / Aktualizacje) + przycisk „Rozłącz".
- Nad wszystkim: `onboarding-wizard` (kreator pierwszego użycia) i `pwa-install-dialog` (podpowiedź instalacji PWA) renderowane jako nakładki.

### library-panel.element.ts — Książki
Rola: zarządzanie biblioteką na urządzeniu z poziomu telefonu.
- „Wyślij plik na urządzenie" (input pliku .rsvp/.txt/.epub) + „Odśwież".
- Zakładki filtrów: Wszystko / Książki / Artykuły (z licznikami).
- Pasek sortowania: Ostatnio dodane / Tytuł / Postęp.
- Lista pozycji: okładka-inicjał kolorowa, tytuł, autor+rozmiar+procent, przycisk ulubione (★/☆), przycisk usuń (✕, z potwierdzeniem).

### converter-panel.element.ts — Konwerter
Rola: konwersja plików (EPUB/PDF/TXT/MD/HTML, MOBI zapowiedziane) na format .rsvp bezpośrednio w przeglądarce, bez wysyłania na serwer.
- Strefa drag&drop + przycisk „Wybierz plik".
- Po konwersji: pola edycji tytułu/autora, statystyki (liczba słów/rozdziałów/paragrafów/rozmiar pliku), przycisk „Pobierz .rsvp", przycisk „Wyślij na urządzenie" (obecnie wyłączony — wymaga połączenia), podgląd pierwszych linijek pliku wynikowego.

### settings-panel.element.ts — Ustawienia (sekcja w zakładce „Więcej")
Rola: zdalny odpowiednik ustawień urządzenia, z suwakami/przełącznikami zamiast list menu.
- Nagłówek marki — 10x tap odblokowuje tryb developera (ten sam mechanizm co na urządzeniu).
- Grupa „Tryb czytania" (segmentowany przełącznik RSVP/Scroll).
- Grupa „Ustawienia RSVP" (widoczna tylko w trybie RSVP): pauza (Tap/Przytrzymanie/Auto), suwak tempa WPM, suwaki opóźnień (długie słowa/złożone/interpunkcja), przełącznik słów-widm.
- Grupa „Typografia RSVP": rozmiar czcionki, krój, podświetlenie fokusowe, tracking, pozycja kotwicy, szerokość/przerwa prowadnicy.
- Grupa „Ustawienia Scroll" (widoczna tylko w trybie Scroll): rozmiar czcionki, interlinia, marginesy.
- Grupa „Wyświetlanie": motyw, jasność, ręka czytająca.
- Grupa „HUD podczas czytania": przełączniki bateria/rozdział/procent, metryka stopki, etykieta baterii.
- Grupa „Język": rozwijana lista 6 języków.
- Grupa „Developer" (widoczna tylko po odblokowaniu): przełącznik trybu developera.
- Przycisk „Pomoc / Przewodnik" — otwiera `help-panel` jako pod-widok.
Każde ustawienie z ikoną „?" otwiera `setting-tooltip` (opis + efekt + wartość domyślna).

### help-panel.element.ts — Pomoc / Przewodnik
Rola: pełny ekran pomocy wywoływany z ustawień.
- Przycisk „Wstecz" (strzałka).
- Sekcja „Szybki start" — 3 kroki (połącz / wyślij książkę / zacznij czytać).
- Kategorie w formie akordeonu (Wyświetlanie / Czytanie / HUD / Język / Połączenie), max 1 rozwinięty element na kategorię.
- Przycisk restartu samouczka (`tutorial-wizard`).

### tutorial-wizard.element.ts — Samouczek (wersja PWA)
Rola: nakładka pełnoekranowa z 4 krokami (RSVP / tempo WPM / tryby pauzy / elementy HUD), pokazywana raz po pierwszym połączeniu z urządzeniem.
- Pasek postępu tekstowy.
- Obszar wizualny + tytuł + opis dla każdego kroku.
- Przyciski: Wstecz (poza pierwszym krokiem), Pomiń, Dalej/Zakończ (ostatni krok).
- Obsługa gestów swipe lewo/prawo do zmiany kroku.

### onboarding.element.ts — Kreator pierwszego użycia
Rola: pełnoekranowy wizard pokazywany raz po otwarciu PWA (localStorage flaga).
- 3 kroki: powitanie / dodaj do ekranu głównego (iOS: instrukcja Share Sheet, Android/desktop: przycisk instalacji PWA, pomijany jeśli już zainstalowane) / połącz urządzenie (instrukcja Wi-Fi).
- Kropki postępu, przyciski „Wróć"/„Pomiń" i „Dalej"/„Zaczynamy".

### pwa-install-dialog.element.ts — Podpowiedź instalacji PWA
Rola: nienachalny dialog zachęcający do instalacji jako aplikacja (pojawia się z opóźnieniem 30s po zdarzeniu `beforeinstallprompt`, z wyciszeniem na 7 dni po odrzuceniu). Może być też wywołany programowo z kreatora onboardingu.

### setting-tooltip.element.ts — Tooltip ustawienia
Rola: mały przycisk „?" przy etykiecie ustawienia, otwiera dymek z opisem/efektem/wartością domyślną. Tylko jeden tooltip widoczny naraz (singleton przez event bus na dokumencie).

### first-use-hint.element.ts — Podpowiedź pierwszego użycia ekranu
Rola: blokująca nakładka pokazywana raz na dany ekran (np. „reading", „converter") przy pierwszym wejściu, z przyciskiem zamknięcia. Stan zapisywany w localStorage per `screenKey`.

### flower-decor.element.ts — Dekoracja tła
Rola: czysto wizualna warstwa kwiatków w tle (SVG, 3 poziomy gęstości: low/medium/high), `pointer-events: none` — brak interakcji.
