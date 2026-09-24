# Flower Companion — pełna dokumentacja API czytnika

## Architektura połączenia

Czytnik Flower (ESP32-S3) po włączeniu **Sync z telefonem** tworzy Access Point WiFi o nazwie `Flower-XXXX` (sufix z MAC). Telefon łączy się z tą siecią, czytnik jest dostępny pod adresem **http://192.168.4.1**.

### Stos sieciowy na urządzeniu:

- **WebServer** (port 80) — obsługuje REST API i serwuje stronę companion
- **DNSServer** (port 53, tryb AP) — odpowiada na KAŻDE zapytanie DNS adresem 192.168.4.1 (captive portal bypass)
- **mDNS** (tryb Station) — urządzenie widoczne jako `rsvp-nano.local`
- **CORS** — wszystkie endpointy API zwracają `Access-Control-Allow-Origin: *`

### Captive Portal Bypass:

Żeby Android/iOS nie wyświetlał "sieć bez internetu":

- `GET /generate_204` → 204 No Content (Android)
- `GET /gen_204` → 204 No Content (Android alternatywny)
- `GET /hotspot-detect.html` → 200 z `<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>` (iOS)
- `GET /connecttest.txt` → 200 z `Microsoft Connect Test` (Windows)

---

## Endpointy API

Wszystkie odpowiedzi JSON zawierają pole `"ok": true/false`. W przypadku błędu: `{"ok":false,"error":"opis błędu"}`.

---

### GET /

**Cel:** Serwuje wbudowaną stronę HTML companion (UI w przeglądarce)  
**Odpowiedź:** `text/html`, cała strona SPA (HTML+CSS+JS w jednym pliku)  
**Cache:** `no-store, max-age=0`

---

### GET /api/hello

**Cel:** Ping / handshake — sprawdzenie czy urządzenie odpowiada  
**Odpowiedź:**

```json
{ "ok": true }
```

**Użycie:** Aplikacja mobilna sprawdza tym endpointem czy pod danym IP jest czytnik Flower.

---

### GET /api/info

**Cel:** Informacje o urządzeniu  
**Odpowiedź:**

```json
{
  "ok": true,
  "name": "Flower",
  "mode": "access_point", // lub "station"
  "baseUrl": "http://192.168.4.1",
  "networkSsid": "Flower-AB12",
  "pairingCode": "1234",
  "uploadPath": "/api/books",
  "api": 1,
  "firmwareVersion": "v0.3.2"
}
```

**Pola:**

- `mode` — tryb sieci: `"access_point"` (czytnik tworzy swoją sieć) lub `"station"` (czytnik podłączony do routera)
- `pairingCode` — 4-cyfrowy kod do parowania BLE
- `api` — wersja protokołu API (obecnie 1)

---

### GET /api/capabilities

**Cel:** Lista funkcji urządzenia (do auto-discovery)  
**Odpowiedź:**

```json
{
  "ok": true,
  "api": 1,
  "firmwareVersion": "v0.3.2",
  "features": {
    "settings": true,
    "books": true,
    "ota": true,
    "pluginsList": true,
    "pluginsRemove": true,
    "pluginsInstallPackage": false,
    "bluetoothTransfer": false,
    "rss": true,
    "focusTimer": true,
    "wifiTimeout": true
  }
}
```

---

### GET /api/books

**Cel:** Lista książek na karcie SD czytnika  
**Odpowiedź:**

```json
{
  "ok": true,
  "books": [
    {
      "name": "books/moja-ksiazka.rsvp",
      "category": "book",
      "title": "Moja Książka",
      "author": "Jan Kowalski",
      "bytes": 124800,
      "progressPercent": 42,
      "chapters": [
        { "title": "Rozdział 1", "startWord": 0 },
        { "title": "Rozdział 2", "startWord": 1523 }
      ]
    }
  ]
}
```

**Pola książki:**

- `name` — ścieżka relatywna (np. `books/plik.rsvp`, `articles/news.rsvp`)
- `category` — `"book"`, `"article"` lub `"legacy"`
- `title`, `author` — z metadanych pliku .rsvp (`@title`, `@author`)
- `bytes` — rozmiar pliku
- `progressPercent` — procent przeczytania (jeśli znany, 0–100)
- `chapters` — tablica rozdziałów z tytułem i indeksem początkowego słowa (tylko .rsvp)

**Struktura na SD:**

```
/books/
├── books/        ← książki
│   ├── moja-ksiazka.rsvp
│   └── inna.txt
├── articles/     ← artykuły (z RSS lub ręcznie)
│   └── newsletter.rsvp
└── stary-plik.rsvp  ← legacy (bez podkatalogu)
```

---

### POST /api/books

**Cel:** Upload książki na czytnik  
**Content-Type:** `multipart/form-data`  
**Parametry query string:**

- `name` — nazwa pliku (opcjonalnie, domyślnie z multipart)
- `category` — `"book"` (domyślne) lub `"article"`

**Body:** multipart z polem `file` zawierającym dane pliku  
**Akceptowane formaty:** `.rsvp`, `.txt`, `.epub`

**Odpowiedź sukces:**

```json
{ "ok": true, "path": "/books/books/moja-ksiazka.rsvp" }
```

**Przykład (JavaScript):**

```javascript
const fd = new FormData();
fd.append("file", blob, "moja-ksiazka.rsvp");
await fetch("/api/books?name=moja-ksiazka.rsvp&category=book", {
  method: "POST",
  body: fd,
});
```

---

### DELETE /api/books

**Cel:** Usunięcie książki  
**Parametr query string:**

- `name` — ścieżka pliku (np. `books/moja-ksiazka.rsvp`)

**Odpowiedź:**

```json
{ "ok": true, "path": "/books/books/moja-ksiazka.rsvp" }
```

---

### GET /api/settings

**Cel:** Pobranie wszystkich ustawień czytnika  
**Odpowiedź:**

```json
{
  "ok": true,
  "version": 1,
  "reading": {
    "wpm": 300,
    "readerMode": "rsvp",
    "pauseMode": "sentence_end",
    "accurateTimeEstimate": true,
    "pacing": {
      "longWordMs": 200,
      "complexWordMs": 200,
      "punctuationMs": 200
    }
  },
  "display": {
    "brightnessIndex": 3,
    "darkMode": true,
    "nightMode": false,
    "handedness": "right",
    "footerMetric": "percentage",
    "batteryLabel": "percent",
    "readingBattery": true,
    "readingChapter": false,
    "readingProgress": false,
    "language": 0,
    "phantomWords": true,
    "fontSizeIndex": 0
  },
  "typography": {
    "typeface": "standard",
    "focusHighlight": true,
    "tracking": 0,
    "anchorPercent": 30,
    "guideWidth": 30,
    "guideGap": 5
  },
  "limits": {
    "wpm": { "min": 10, "max": 1000 },
    "brightnessIndex": { "min": 0, "max": 4 },
    "pacingMs": { "min": 0, "max": 600 },
    "tracking": { "min": -2, "max": 3 },
    "anchorPercent": { "min": 30, "max": 40 },
    "guideWidth": { "min": 12, "max": 30 },
    "guideGap": { "min": 2, "max": 8 }
  },
  "scroll": {
    "scrollFontSize": 4,
    "scrollLineSpacing": 1,
    "scrollMargin": 1
  },
  "developer": {
    "devMode": false
  }
}
```

---

### PUT/PATCH /api/settings

**Cel:** Zmiana ustawień (częściowa — wysyłasz tylko zmienione klucze)  
**Content-Type:** `application/json`  
**Limit:** max 2048 bajtów

**Body — klucze do wysłania (wszystkie opcjonalne):**

```json
{
  "wpm": 300,
  "readerMode": "rsvp",
  "pauseMode": "sentence_end",
  "longWordMs": 200,
  "complexWordMs": 200,
  "punctuationMs": 200,
  "brightnessIndex": 3,
  "darkMode": true,
  "nightMode": false,
  "handedness": "right",
  "footerMetric": "percentage",
  "batteryLabel": "percent",
  "readingBattery": true,
  "readingChapter": false,
  "readingProgress": false,
  "language": 0,
  "phantomWords": true,
  "fontSizeIndex": 0,
  "typeface": "standard",
  "focusHighlight": true,
  "tracking": 0,
  "anchorPercent": 30,
  "guideWidth": 30,
  "guideGap": 5,
  "scrollFontSize": 4,
  "scrollLineSpacing": 1,
  "scrollMargin": 1,
  "devMode": false
}
```

**WAŻNE:** Firmware parsuje klucze z PŁASKIEGO JSON-a (nie zagnieżdżonego). Wysyłasz spłaszczony obiekt z kluczami jak wyżej.

**Odpowiedź:** Zwraca pełny JSON settings (jak GET), opcjonalnie z:

```json
{ "restartRequired": true, "restartReason": "Typeface change requires display reload" }
```

---

### Szczegóły ustawień:

| Klucz               | Typ    | Zakres                                            | Opis                                                 |
| ------------------- | ------ | ------------------------------------------------- | ---------------------------------------------------- |
| `wpm`               | int    | 10–1000                                           | Tempo czytania RSVP (słów/minutę)                    |
| `readerMode`        | string | `"rsvp"` / `"scroll"`                             | Tryb czytnika                                        |
| `pauseMode`         | string | `"sentence_end"` / `"instant"`                    | Zachowanie pauzy                                     |
| `longWordMs`        | int    | 0–600                                             | Opóźnienie dla długich słów [ms]                     |
| `complexWordMs`     | int    | 0–600                                             | Opóźnienie dla złożonych słów [ms]                   |
| `punctuationMs`     | int    | 0–600                                             | Opóźnienie po interpunkcji [ms]                      |
| `brightnessIndex`   | int    | 0–4                                               | Jasność ekranu (0=min, 4=max)                        |
| `darkMode`          | bool   | —                                                 | Ciemny motyw                                         |
| `nightMode`         | bool   | —                                                 | Nocny motyw (nadpisuje darkMode)                     |
| `handedness`        | string | `"right"` / `"left"`                              | Dłoń trzymająca czytnik                              |
| `footerMetric`      | string | `"percentage"` / `"chapter_time"` / `"book_time"` | Co wyświetlać w stopce                               |
| `batteryLabel`      | string | `"percent"` / `"time_remaining"` / `"voltage"`    | Format baterii                                       |
| `readingBattery`    | bool   | —                                                 | Pokaż baterię podczas czytania                       |
| `readingChapter`    | bool   | —                                                 | Pokaż rozdział podczas czytania                      |
| `readingProgress`   | bool   | —                                                 | Pokaż % postępu podczas czytania                     |
| `language`          | int    | 0–5                                               | Język (0=PL, 1=EN, 2=DE, 3=ES, 4=FR, 5=IT)           |
| `phantomWords`      | bool   | —                                                 | Słowa widma (kontekst wokół aktualnego słowa)        |
| `fontSizeIndex`     | int    | 0–2                                               | Rozmiar czcionki RSVP (S/M/L)                        |
| `typeface`          | string | `"standard"` / `"open_dyslexic"` / `"atkinson"`   | Krój czcionki                                        |
| `focusHighlight`    | bool   | —                                                 | Podświetlenie litery fokusowej (ORP)                 |
| `tracking`          | int    | -2 do +3                                          | Odstępy między literami                              |
| `anchorPercent`     | int    | 30–40                                             | Pozycja kotwicy ORP [%]                              |
| `guideWidth`        | int    | 12–30                                             | Szerokość prowadnicy [px]                            |
| `guideGap`          | int    | 2–8                                               | Przerwa prowadnicy [px]                              |
| `scrollFontSize`    | int    | 0–8                                               | Rozmiar czcionki w trybie Scroll                     |
| `scrollLineSpacing` | int    | 0–2                                               | Interlinia w Scroll (0=Compact, 1=Normal, 2=Relaxed) |
| `scrollMargin`      | int    | 0–2                                               | Marginesy w Scroll (0=Narrow, 1=Normal, 2=Wide)      |
| `devMode`           | bool   | —                                                 | Tryb developera                                      |

---

### GET /api/wifi

**Cel:** Sprawdzenie zapisanego WiFi domowego  
**Odpowiedź:**

```json
{ "ok": true, "configured": true, "ssid": "MojaSiec" }
```

(Hasło NIGDY nie jest zwracane)

---

### PUT /api/wifi

**Cel:** Zapisanie WiFi domowego (do OTA i RSS)  
**Body:**

```json
{ "ssid": "NazwaSieci", "password": "haslo123" }
```

**Odpowiedź:**

```json
{ "ok": true, "configured": true, "ssid": "NazwaSieci" }
```

---

### DELETE /api/wifi

**Cel:** Usunięcie zapisanego WiFi  
**Odpowiedź:** `{"ok":true,"configured":false,"ssid":""}`

---

### GET /api/rss-feeds

**Cel:** Lista zapisanych feedów RSS  
**Odpowiedź:**

```json
{ "ok": true, "feeds": ["https://example.com/feed/", "https://blog.pl/rss"] }
```

---

### PUT /api/rss-feeds

**Cel:** Zapisanie feedów RSS  
**Body:**

```json
{ "feeds": ["https://example.com/feed/", "https://blog.pl/rss"] }
```

**Limit:** max 24 feedów, max 4096 bajtów

---

### POST /api/ota

**Cel:** Upload i instalacja firmware  
**Content-Type:** `multipart/form-data`  
**Body:** multipart z polem `firmware` zawierającym plik .bin

**Progress:** używaj XMLHttpRequest z `xhr.upload.onprogress` do śledzenia postępu  
**Odpowiedź sukces:** `{"ok":true,"reboot":true}` — czytnik restartuje się po 500ms  
**Odpowiedź błąd:** `{"ok":false,"error":"Update.begin failed"}`

**Przykład:**

```javascript
const fd = new FormData();
fd.append("firmware", binBlob, "flower-firmware.bin");
const xhr = new XMLHttpRequest();
xhr.open("POST", "http://192.168.4.1/api/ota");
xhr.upload.onprogress = (e) => {
  console.log(Math.round((e.loaded * 100) / e.total) + "%");
};
xhr.onload = () => {
  // Czytnik się restartuje, WiFi zaraz padnie
};
xhr.send(fd);
```

---

### GET /api/plugins

**Cel:** Lista zainstalowanych pluginów  
**Odpowiedź:**

```json
{
  "ok": true,
  "plugins": [
    {
      "id": "focus-timer",
      "name": "Focus Timer",
      "installed": true,
      "builtin": false,
      "active": true
    },
    { "id": "rss", "name": "RSS Feeds", "installed": true, "builtin": false, "active": true }
  ]
}
```

---

### DELETE /api/plugins?id=...

**Cel:** Usunięcie pluginu  
**Parametr:** `id` — identyfikator pluginu  
**Odpowiedź:** zależy od pluginu (focus-timer nie da się usunąć, rss wymaga OTA)

---

### POST /api/power/wifi-timeout

**Cel:** Ustawienie auto-wyłączenia WiFi po zadanym czasie  
**Body:**

```json
{ "timeout": 300 }
```

**Pole:** `timeout` — czas w sekundach (0 = brak limitu, max 3600)  
**Odpowiedź:**

```json
{ "ok": true, "timeoutSeconds": 300 }
```

---

### OPTIONS (dowolny /api/\*)

**Cel:** CORS preflight  
**Odpowiedź:** 204 z headerami:

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, PUT, PATCH, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, Authorization
Access-Control-Max-Age: 86400
```

---

## Format pliku .rsvp

Format książki na czytniku:

```
@rsvp 1
@title Tytuł Książki
@author Imię Nazwisko
@source plik-źródłowy.epub

@chapter Rozdział 1
Tekst pierwszego paragrafu. Każda linia
jest zawinięta do ~96 znaków.

Następny paragraf po pustej linii.

@chapter Rozdział 2
Kolejny rozdział zaczyna się dyrektywą @chapter.
```

**Dyrektywy:**

- `@rsvp 1` — wersja formatu (obowiązkowy nagłówek)
- `@title ...` — tytuł książki
- `@author ...` — autor
- `@source ...` — plik źródłowy (informacyjnie)
- `@chapter ...` — początek rozdziału
- `@para` — opcjonalnie oznacza początek paragrafu (zazwyczaj nie trzeba)
- Pusta linia = separator paragrafów

---

## Typowy flow użytkownika (aplikacja mobilna)

1. **Połączenie:**
   - Użytkownik włącza "Sync z telefonem" na czytniku
   - Telefon łączy się z WiFi `Flower-XXXX`
   - Aplikacja sprawdza `GET /api/hello` pod `http://192.168.4.1`
   - Jeśli ok → `GET /api/info` żeby pobrać metadata

2. **Biblioteka:**
   - `GET /api/books` → wyświetlenie listy
   - Upload: `POST /api/books` z multipart
   - Usuwanie: `DELETE /api/books?name=books/plik.rsvp`

3. **Ustawienia:**
   - `GET /api/settings` → wypełnienie formularza
   - Zmiana: `PATCH /api/settings` z JSON payload (tylko zmienione klucze)

4. **Aktualizacja:**
   - Aplikacja pobiera .bin z GitHub Releases
   - `POST /api/ota` z multipart upload
   - Czytnik się restartuje

5. **Konwersja (w aplikacji, offline):**
   - Konwertuj EPUB/PDF/MOBI/AZW3/DOCX/TXT/MD/HTML → .rsvp w telefonie
   - Wynik: `POST /api/books` z plikiem .rsvp
   - Czytnik widzi nową książkę w bibliotece

---

## Połączenie z GitHub (OTA)

- Releases: `https://api.github.com/repos/GRKarol/czytnik01/releases/latest`
- Asset firmware: szukaj pliku o nazwie `flower-firmware.bin`
- Porównanie wersji: semver (tag release vs `firmwareVersion` z `/api/info`)

---

## Notatki implementacyjne

- **Pamięć NVS** — ustawienia zapisywane w ESP32 NVS (Non-Volatile Storage), namespace `"rsvp"`
- **Karta SD** — książki na SD_MMC (slot na kartę microSD)
- **Limit uploadu** — brak ograniczenia rozmiaru, ale karta SD ma skończoną pojemność
- **Timeout WiFi** — opcjonalnie wyłącza WiFi po X sekundach bezczynności
- **Restart po OTA** — po udanym OTA urządzenie robi `ESP.restart()` po 500ms delay
- **Chapters** — czytane bezpośrednio z pliku .rsvp (nie trzeba osobnego indeksu)

---

## Nowe endpointy (v0.3.4)

### GET /api/state

**Cel:** Jeden zbiorczy request zamiast 6 oddzielnych (1 round-trip)  
**Odpowiedź:** Zawiera WSZYSTKO w jednym JSON:

```json
{
  "ok": true,
  "info": {
    "name": "Flower",
    "mode": "access_point",
    "baseUrl": "http://192.168.4.1",
    "networkSsid": "Flower-AB12",
    "pairingCode": "1234",
    "firmwareVersion": "v0.3.4",
    "api": 1,
    "batteryPercent": 78,
    "sdFreeKb": 14523400,
    "sdTotalKb": 15523840
  },
  "capabilities": {
    "settings": true,
    "books": true,
    "ota": true,
    "pluginsList": true,
    "pluginsRemove": true,
    "pluginsInstallPackage": false,
    "bluetoothTransfer": false,
    "rss": true,
    "focusTimer": true,
    "wifiTimeout": true
  },
  "settings": {
    /* identyczne z GET /api/settings */
  },
  "books": [
    /* identyczne z GET /api/books .books */
  ],
  "plugins": [
    /* identyczne z GET /api/plugins .plugins */
  ],
  "rss": { "feeds": ["https://..."] },
  "wifi": { "configured": true, "ssid": "MojaSiec" }
}
```

---

### GET /api/log/tail?n=50

**Cel:** Ostatnie N linii logów firmware (ring buffer 100 linii)  
**Parametr:** `n` — ile linii zwrócić (domyślnie 50, max 100)  
**Odpowiedź:**

```json
{
  "ok": true,
  "total": 87,
  "lines": [
    "[sync] upload start books/moja-ksiazka.rsvp",
    "[sync] upload end bytes=124800 error=",
    "[app] settings saved: wpm=350"
  ]
}
```

---

### GET /api/lang/codes

**Cel:** Mapowanie kodów językowych — app nie zgaduje, czyta z urządzenia  
**Odpowiedź:**

```json
{
  "ok": true,
  "languages": [
    { "code": "pl", "id": 0, "name": "Polski" },
    { "code": "en", "id": 1, "name": "English" },
    { "code": "de", "id": 2, "name": "Deutsch" },
    { "code": "es", "id": 3, "name": "Español" },
    { "code": "fr", "id": 4, "name": "Français" },
    { "code": "it", "id": 5, "name": "Italiano" }
  ]
}
```

---

### GET /api/books/position?name=books/moja-ksiazka.rsvp

**Cel:** Pobranie pozycji czytania dla konkretnej książki  
**Parametr:** `name` — ścieżka książki (ta sama co w books list)  
**Odpowiedź:**

```json
{
  "ok": true,
  "name": "books/moja-ksiazka.rsvp",
  "wordIndex": 1523,
  "wordCount": 45000,
  "percent": 3
}
```

---

### PUT /api/books/position?name=books/moja-ksiazka.rsvp

**Cel:** Ustawienie pozycji czytania (synchronizacja z aplikacją)  
**Body:**

```json
{
  "wordIndex": 2000,
  "wordCount": 45000
}
```

**Odpowiedź:** Jak GET (z zaktualizowanymi wartościami)

---

## Pola dodane do /api/info (v0.3.4)

| Pole             | Typ         | Opis                              |
| ---------------- | ----------- | --------------------------------- |
| `batteryPercent` | int (0–100) | Aktualny % baterii                |
| `sdFreeKb`       | int         | Wolne miejsce na karcie SD [KB]   |
| `sdTotalKb`      | int         | Całkowita pojemność karty SD [KB] |

---

## Poprawki (v0.3.4)

- `kMaxUiLanguage` zmienione z 1 na 5 — teraz firmware akceptuje wszystkie 6 języków (PL/EN/DE/ES/FR/IT)
- Dodano endpointy captive portal: `/ncsi.txt`, `/redirect`, `/check_network` (HyperOS/MIUI)

---

## Nowe endpointy (v0.3.6)

### UDP Broadcast Discovery (port 5555)

Czytnik w trybie AP co 2 sekundy wysyła broadcast UDP na `192.168.4.255:5555`:

```
FLOWER|192.168.4.1|v0.3.6|1234
```

Format: `FLOWER|<ip>|<firmwareVersion>|<pairingCode>`

**Użycie w natywnej appce:**

```kotlin
val socket = DatagramSocket(5555)
socket.broadcast = true
val buffer = ByteArray(128)
val packet = DatagramPacket(buffer, buffer.size)
socket.receive(packet) // blokuje do ~2s max
val msg = String(packet.data, 0, packet.length)
// msg = "FLOWER|192.168.4.1|v0.3.6|1234"
val parts = msg.split("|")
// parts[0] = "FLOWER" (identyfikator)
// parts[1] = IP czytnika
// parts[2] = wersja firmware
// parts[3] = pairing code
```

**Zalety vs HTTP polling:**

- Wykrycie w <50ms (vs 3000ms polling)
- Zero obciążenia HTTP servera
- Działa nawet zanim Android uzna sieć za "ok"

---

### DELETE /api/log

**Cel:** Czyszczenie ring bufora logów  
**Odpowiedź:**

```json
{ "ok": true, "cleared": true }
```

---

## Pełna specyfikacja sterowania czytnikiem z aplikacji mobilnej

### Architektura komunikacji

```
┌─────────────────────────────────┐
│     Aplikacja Android           │
│  ┌───────────────────────────┐  │
│  │  UDP Listener (port 5555) │  │  ← wykrywanie czytnika (<50ms)
│  │  HTTP Client (port 80)    │  │  ← sterowanie (REST API)
│  └───────────────────────────┘  │
└───────────────┬─────────────────┘
                │ WiFi (192.168.4.1)
┌───────────────┴─────────────────┐
│     Czytnik ESP32-S3            │
│  ┌───────────────────────────┐  │
│  │  UDP Broadcast (co 2s)    │  │
│  │  HTTP Server (port 80)    │  │
│  │  DNS Server (port 53)     │  │
│  └───────────────────────────┘  │
└─────────────────────────────────┘
```

---

### 1. Połączenie — krok po kroku

| Krok | App robi                               | Czytnik robi                                 |
| ---- | -------------------------------------- | -------------------------------------------- |
| 1    | Nic (czeka)                            | User włącza "Sync z telefonem" → AP startuje |
| 2    | User skanuje QR lub ręcznie łączy WiFi | Wyświetla QR + IP na ekranie                 |
| 3    | UDP listener wykrywa broadcast         | Nadaje broadcast co 2s                       |
| 4    | `GET /api/state` (1 request)           | Zwraca pełny stan                            |
| 5    | Keep-alive ping co 8s                  | Odpowiada na /api/hello                      |

---

### 2. Biblioteka — zarządzanie książkami

| Akcja            | Endpoint                             | Metoda | Body/Params                     |
| ---------------- | ------------------------------------ | ------ | ------------------------------- |
| Lista książek    | `/api/state` → `.books[]`            | GET    | —                               |
| Upload książki   | `/api/books?name=X&category=book`    | POST   | multipart (pole `file`)         |
| Upload artykułu  | `/api/books?name=X&category=article` | POST   | multipart (pole `file`)         |
| Usuń książkę     | `/api/books?name=books/plik.rsvp`    | DELETE | —                               |
| Pozycja czytania | `/api/books/position?name=X`         | GET    | —                               |
| Zapisz pozycję   | `/api/books/position?name=X`         | PUT    | `{"wordIndex":N,"wordCount":M}` |

**Chaptery z /api/books:**
Każda książka .rsvp w liście ma pole `chapters: [{title, startWord}]` — app może wyświetlić spis treści i przeskoczyć do rozdziału (zapisując `wordIndex` = `startWord` danego rozdziału).

---

### 3. Ustawienia — pełna lista

| Akcja            | Endpoint                   | Metoda    |
| ---------------- | -------------------------- | --------- |
| Pobierz wszystko | `/api/state` → `.settings` | GET       |
| Zmień ustawienia | `/api/settings`            | PATCH/PUT |

**Payload do PATCH (tylko zmienione klucze, płaski JSON):**

```json
{
  "wpm": 350,
  "readerMode": "rsvp",
  "pauseMode": "sentence_end",
  "longWordMs": 150,
  "complexWordMs": 100,
  "punctuationMs": 200,
  "brightnessIndex": 3,
  "darkMode": true,
  "nightMode": false,
  "handedness": "right",
  "footerMetric": "percentage",
  "batteryLabel": "percent",
  "readingBattery": true,
  "readingChapter": true,
  "readingProgress": true,
  "language": 0,
  "phantomWords": true,
  "fontSizeIndex": 1,
  "typeface": "standard",
  "focusHighlight": true,
  "tracking": 0,
  "anchorPercent": 33,
  "guideWidth": 24,
  "guideGap": 5,
  "scrollFontSize": 4,
  "scrollLineSpacing": 1,
  "scrollMargin": 1,
  "devMode": false
}
```

**Graceful fallback:** Jeśli firmware odrzuci pole (np. stary firmware nie zna `scrollFontSize`), retry bez tego pola.

---

### 4. Pluginy

| Akcja                 | Endpoint                      | Metoda |
| --------------------- | ----------------------------- | ------ |
| Lista zainstalowanych | `/api/state` → `.plugins[]`   | GET    |
| Usuń plugin           | `/api/plugins?id=focus-timer` | DELETE |

**Sklep GitHub:** App czyta `https://raw.githubusercontent.com/GRKarol/czytnik01/main/public/plugins/index.json` i filtruje vs zainstalowane.

---

### 5. RSS

| Akcja        | Endpoint                      | Metoda | Body                        |
| ------------ | ----------------------------- | ------ | --------------------------- |
| Lista feedów | `/api/state` → `.rss.feeds[]` | GET    | —                           |
| Zapisz feedy | `/api/rss-feeds`              | PUT    | `{"feeds":["url1","url2"]}` |

---

### 6. OTA (firmware update)

| Krok | App robi                                                             |
| ---- | -------------------------------------------------------------------- |
| 1    | `GET https://api.github.com/repos/GRKarol/czytnik01/releases/latest` |
| 2    | Porównaj `tag_name` vs `firmwareVersion` z `/api/state`              |
| 3    | Pobierz asset `flower-firmware.bin` do cache                         |
| 4    | `POST /api/ota` multipart (pole `firmware`)                          |
| 5    | Czytnik się restartuje po 500ms                                      |

---

### 7. WiFi domowe

| Akcja    | Endpoint               | Metoda | Body                          |
| -------- | ---------------------- | ------ | ----------------------------- |
| Sprawdź  | `/api/state` → `.wifi` | GET    | —                             |
| Zapisz   | `/api/wifi`            | PUT    | `{"ssid":"X","password":"Y"}` |
| Zapomnij | `/api/wifi`            | DELETE | —                             |

---

### 8. Logi / Debug

| Akcja             | Endpoint             | Metoda |
| ----------------- | -------------------- | ------ |
| Ostatnie 50 linii | `/api/log/tail?n=50` | GET    |
| Wyczyść logi      | `/api/log`           | DELETE |

---

### 9. Języki

| Akcja     | Endpoint          | Metoda |
| --------- | ----------------- | ------ |
| Mapowanie | `/api/lang/codes` | GET    |

Odpowiedź: `{languages: [{code:"pl",id:0,name:"Polski"}, ...]}`. App wysyła `language: id` (int) w settings.

---

### 10. Timeout WiFi

| Akcja          | Endpoint                  | Metoda | Body                                  |
| -------------- | ------------------------- | ------ | ------------------------------------- |
| Ustaw auto-off | `/api/power/wifi-timeout` | POST   | `{"timeout":300}` (sekundy, 0=wyłącz) |

---

### Zasady niezawodności

1. **Jeden request na start:** `GET /api/state` zwraca WSZYSTKO — nie musisz robić 6 oddzielnych
2. **Keep-alive:** `GET /api/hello` co 8s — wykrywa disconnect w <16s
3. **Timeout:** Każdy request z timeout 3s — nie wieszaj UI
4. **Retry:** Max 2 retry na request, potem pokaż błąd
5. **Fallback settings:** Jeśli PATCH zwraca 400, wyślij ponownie bez problematycznego klucza
6. **Kolejka:** Nie wysyłaj 2 PATCH jednocześnie — ESP32 jest single-threaded
7. **Captive portal:** Firmware odpowiada 204 na WSZYSTKO co nie zaczyna się od /api/ — Android nie pokaże wykrzyknika
