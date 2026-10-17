# Roadmap

Stan na 2026-10-13. Lista, nie kontrakt.

## Zrobione

- Firmware: kreator pierwszego uruchomienia (12 kroków), motywy menu/czytania,
  RSVP + przewijanie strony z osobnymi opóźnieniami (długie słowa, złożoność,
  przecinki, kropki), prowadnica lewa/prawa, wygaszacz ekranu z etapem
  przyciemnienia i potwierdzeniem wyłączenia, przyciski PWR/prawy
  przeprogramowane (lewy fizyczny = RESET na pinie EN, nieprogramowalny
  sprzętowo).
- Czcionki na karcie SD: loader z `SD_MMC`, ~20 fontów z pełnym pokryciem
  polskich/czeskich/rumuńskich/niemieckich/francuskich znaków, paczka
  `fonts-pack` pobiera się sama przez Wi-Fi z GitHub Release (bez PWA, bez
  BLE) — patrz `docs/PLAN_FONTY_NA_SD.md`. Pozostaje tylko weryfikacja na
  sprzęcie (czas otwarcia ekranu wyboru, przełączanie fontu, fallback
  Atkinson przy braku karty).
- Flasher (`grkarol.github.io/czytnik01`): ESP Web Tools + WebSerial, krok
  przygotowania nowej karty SD (format + wgranie czcionek przez kabel).
- OTA: firmware sam odpytuje GitHub Releases i aktualizuje się przez Wi-Fi
  (`firmware/src/update/OtaUpdater.cpp`) — nie przez przeglądarkę.
- Pluginy: wbudowane w firmware (`firmware/src/plugins/builtin/`), włączane
  lokalnie z poziomu menu (Aktywne/Biblioteka/Detail, NVS), bez instalacji
  paczek ZIP z sieci.
- Aplikacja mobilna Flower (Capacitor/Android, `app/` + `android/` w tym
  repo): synchronizacja biblioteki z czytnikiem przez
  `firmware/src/sync/CompanionSyncManager.cpp` po Wi-Fi, nie przez przeglądarkę.
  Wydania na `flower.theworkpc.com/appdownload`
  (repo `GRKarol/FlowerSite.github.io`).

## Zarzucone / zastąpione innym podejściem

Pierwotny plan (Faza 2-4 w starej wersji tego pliku) zakładał PWA w
przeglądarce jako główny klient: konwerter `.epub`/`.pdf`/`.mobi` w
przeglądarce, bibliotekę i pluginy przez HTTP API urządzenia, sprawdzanie OTA
z poziomu PWA, wizard onboardingowy w PWA. W praktyce projekt poszedł inną
drogą:

- Klientem jest natywna apka Android (Capacitor), nie PWA w przeglądarce.
- Kreator pierwszego uruchomienia jest w firmware, na ekranie czytnika, nie
  w przeglądarce.
- OTA i pobieranie fontów firmware robi samodzielnie przez Wi-Fi, bez
  pośrednictwa klienta.
- Pluginy są wbudowane w firmware, nie instalowane jako paczki z sieci.

PWA/flasher na `grkarol.github.io/czytnik01` zostaje tylko do pierwszego
flashowania urządzenia (ESP Web Tools), nie jako platforma klienta.

## Otwarte

- Weryfikacja na sprzęcie v0.4.13: przyciski PWR/prawy, Wi-Fi przy zgaszonym
  ekranie, polskie litery w Atkinson/OpenDyslexic, opóźnienia w trybie
  strony, podgląd czcionki w kreatorze, krok przygotowania karty SD we
  flasherze.
- Weryfikacja na sprzęcie etapu 6 z `PLAN_FONTY_NA_SD.md` (ekran wyboru
  fontu przy ~20 pozycjach).
- Lewy przycisk fizyczny: decyzja o przelutowaniu z pinu EN na wolny GPIO,
  jeśli ma dostać własną funkcję w UI. Czeka na zdjęcia PCB albo model
  urządzenia.
