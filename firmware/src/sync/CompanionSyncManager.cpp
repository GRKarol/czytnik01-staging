#include "sync/CompanionSyncManager.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <Update.h>
#include <WiFi.h>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "sync/WifiQrCode.h"
#include "ble/BleApi.h"
#include "update/OtaUpdater.h"
#include <qrcode.h>

// Global BLE API pointer — set by App during init, used by CompanionSyncManager
// to generate BLE pairing QR code instead of WiFi QR.
BleApi *g_bleApiPtr = nullptr;

#ifndef RSVP_FIRMWARE_VERSION
#define RSVP_FIRMWARE_VERSION "dev"
#endif

#ifndef FLOWER_BLE_ENABLED
#define FLOWER_BLE_ENABLED 0
#endif

namespace {

constexpr const char *kMdnsName = "rsvp-nano";
constexpr const char *kBooksPath = "/books";
constexpr const char *kBookFilesPath = "/books/books";
constexpr const char *kArticleFilesPath = "/books/articles";
constexpr const char *kConfigPath = "/config";
constexpr const char *kRssConfigPath = "/config/rss.conf";
constexpr const char *kPrefsNamespace = "rsvp";
constexpr size_t kMaxMetadataLineChars = 160;
constexpr size_t kMaxSettingsPatchBytes = 2048;
constexpr size_t kMaxRssFeedsPatchBytes = 4096;
constexpr size_t kMaxRssFeeds = 24;
constexpr const char *kPrefWpm = "wpm";
constexpr const char *kPrefBrightness = "bright";
constexpr const char *kPrefDarkMode = "dark";
constexpr const char *kPrefNightMode = "night";
constexpr const char *kPrefUiLanguage = "ui_lang";
constexpr const char *kPrefReaderMode = "read_mode";
constexpr const char *kPrefHandedness = "handed";
constexpr const char *kPrefPhantomWords = "phantom_on";
constexpr const char *kPrefFooterMetricMode = "prog_md";
constexpr const char *kPrefBatteryLabelMode = "bat_md";
constexpr const char *kPrefReaderBatteryVisible = "read_bat";
constexpr const char *kPrefReaderChapterVisible = "read_ch";
constexpr const char *kPrefReaderProgressVisible = "read_pct";
constexpr const char *kPrefReaderFontSize = "font_size";
constexpr const char *kPrefReaderTypeface = "typeface";
constexpr const char *kPrefTypographyFocusHighlight = "type_hlt";
constexpr const char *kPrefPacingLongMs = "pace_lms";
constexpr const char *kPrefPacingComplexMs = "pace_cms";
constexpr const char *kPrefPacingPunctuationMs = "pace_pms";
constexpr const char *kPrefPauseMode = "pause_md";
constexpr const char *kPrefAccurateTime = "time_est_a";
constexpr const char *kPrefTypographyTracking = "type_trk";
constexpr const char *kPrefTypographyAnchor = "type_anc";
constexpr const char *kPrefTypographyGuideWidth = "type_wid";
constexpr const char *kPrefTypographyGuideGap = "type_gap";
constexpr const char *kPrefScrollFontSize = "sc_font";
constexpr const char *kPrefScrollLineSpacing = "sc_line_sp";
constexpr const char *kPrefScrollMargin = "sc_margin";
constexpr const char *kPrefWifiSsid = "wifi_ssid";
constexpr const char *kPrefWifiPass = "wifi_pass";
constexpr uint16_t kDefaultWpm = 230;
constexpr uint16_t kMinWpm = 10;
constexpr uint16_t kMaxWpm = 1000;
constexpr uint8_t kDefaultBrightness = 3;
constexpr uint8_t kMaxBrightness = 4;
constexpr uint8_t kMaxUiLanguage = 5;
constexpr uint8_t kMaxReaderMode = 1;
constexpr uint8_t kMaxHandedness = 1;
constexpr uint8_t kMaxFooterMetric = 2;
constexpr uint8_t kMaxBatteryLabel = 2;
constexpr uint8_t kMaxReaderFontSize = 2;
constexpr uint8_t kMaxReaderTypeface = 19;  // DisplayManager::ReaderTypeface::Count - 1
constexpr uint8_t kMaxPauseMode = 1;
constexpr uint16_t kDefaultPacingDelayMs = 100;
constexpr uint16_t kMaxPacingDelayMs = 600;
constexpr int8_t kMinTypographyTracking = -2;
constexpr int8_t kMaxTypographyTracking = 3;
constexpr uint8_t kMinTypographyAnchor = 30;
constexpr uint8_t kMaxTypographyAnchor = 40;
constexpr uint8_t kDefaultTypographyAnchor = 35;
constexpr uint8_t kMinTypographyGuideWidth = 12;
constexpr uint8_t kMaxTypographyGuideWidth = 30;
constexpr uint8_t kDefaultTypographyGuideWidth = 30;
constexpr uint8_t kMinTypographyGuideGap = 0;
constexpr uint8_t kMaxTypographyGuideGap = 8;
constexpr uint8_t kDefaultTypographyGuideGap = 0;
constexpr uint8_t kMaxScrollFontSize = 8;
constexpr uint8_t kDefaultScrollFontSize = 4;
constexpr uint8_t kMaxScrollLineSpacing = 2;
constexpr uint8_t kDefaultScrollLineSpacing = 1;
constexpr uint8_t kMaxScrollMargin = 2;
constexpr uint8_t kDefaultScrollMargin = 1;

const char kWebCompanionHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="pl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flower Companion</title>
<style>
:root{color-scheme:dark;--bg:#0f0f12;--fg:#f0ede6;--muted:#9a9d96;--line:#2a2d30;--card:#1a1b1f;--accent:#6ec9a8;--accentInk:#070f0c;--accent2:#ff9b73;--soft:#1c1e22;--radius:14px;--shadow:0 4px 24px rgba(0,0,0,.4)}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;min-height:100vh;min-height:100dvh}
header{position:sticky;top:0;z-index:10;background:rgba(15,15,18,.88);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);border-bottom:1px solid var(--line);padding:16px 16px 12px;padding-top:calc(16px + env(safe-area-inset-top))}
.brand{display:flex;align-items:center;gap:10px;margin-bottom:12px}.brand svg{color:var(--accent)}.brand h1{font-size:1.2rem;margin:0;letter-spacing:-.02em}.brand small{color:var(--muted);font-size:.75rem;font-weight:400}
.tabs{display:flex;gap:6px;overflow-x:auto;-webkit-overflow-scrolling:touch;scrollbar-width:none;padding-bottom:2px}.tabs::-webkit-scrollbar{display:none}
button,.button{border:1px solid var(--line);border-radius:var(--radius);background:var(--card);color:var(--fg);padding:10px 14px;font:inherit;cursor:pointer;transition:all .15s}
button:active{transform:scale(.97)}button.primary,.button.primary{background:var(--accent);border-color:var(--accent);color:var(--accentInk);font-weight:700;box-shadow:0 4px 14px rgba(110,201,168,.25)}button.danger{color:var(--accent2);border-color:var(--accent2)}
.tabs button{white-space:nowrap;padding:9px 14px;font-size:.88rem;font-weight:600;border-radius:999px}.tabs button.active{background:var(--fg);color:var(--bg);border-color:var(--fg)}
main{max-width:640px;margin:0 auto;padding:16px;padding-bottom:calc(16px + env(safe-area-inset-bottom))}.page{display:none}.page.active{display:block}
.card{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:16px;margin-bottom:14px;box-shadow:var(--shadow)}
h2{font-size:1.05rem;margin:0 0 12px;display:flex;align-items:center;gap:8px}h3{font-size:.95rem;margin:0 0 8px}.muted{color:var(--muted)}
.status{padding:12px 14px;border-radius:var(--radius);background:var(--soft);margin-bottom:14px;font-size:.9rem;border:1px solid var(--line)}
label{display:block;font-weight:600;margin:12px 0 6px;font-size:.9rem}input,textarea,select{width:100%;border:1px solid var(--line);border-radius:10px;background:var(--bg);color:var(--fg);font:inherit;padding:10px 12px}
input[type=range]{padding:0;background:transparent;accent-color:var(--accent)}input[type=checkbox]{width:auto;margin-right:8px}
textarea{min-height:140px;resize:vertical}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.row>*{flex:1}.row button{flex:0 0 auto}
.item{border-top:1px solid var(--line);padding:12px 0;display:flex;align-items:center;gap:10px}.item:first-child{border-top:0}.item-info{flex:1;min-width:0}.item-title{font-weight:700;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.item-meta{color:var(--muted);font-size:.82rem}
.link-banner{display:block;padding:14px 16px;border-radius:var(--radius);background:linear-gradient(135deg,#1a3a2f,#1a2a38);border:1px solid var(--accent);color:var(--fg);text-decoration:none;text-align:center;font-weight:700;margin-bottom:14px;transition:opacity .15s}.link-banner:active{opacity:.8}
.link-banner span{display:block;font-weight:400;font-size:.85rem;color:var(--muted);margin-top:4px}
code{background:var(--soft);border-radius:6px;padding:2px 6px;font-size:.88em}
.group{margin-bottom:14px;padding:14px;border:1px solid var(--line);border-radius:var(--radius);background:var(--soft)}.group-title{font-size:.78rem;font-weight:700;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:0 0 10px}
</style>
</head>
<body>
<header>
<div class="brand">
<svg width="28" height="28" viewBox="0 0 100 100"><g transform="translate(50 50)"><ellipse cx="0" cy="-18" rx="11" ry="18" fill="currentColor" opacity=".85"/><ellipse cx="0" cy="-18" rx="11" ry="18" fill="currentColor" opacity=".85" transform="rotate(60)"/><ellipse cx="0" cy="-18" rx="11" ry="18" fill="currentColor" opacity=".85" transform="rotate(120)"/><ellipse cx="0" cy="-18" rx="11" ry="18" fill="currentColor" opacity=".85" transform="rotate(180)"/><ellipse cx="0" cy="-18" rx="11" ry="18" fill="currentColor" opacity=".85" transform="rotate(240)"/><ellipse cx="0" cy="-18" rx="11" ry="18" fill="currentColor" opacity=".85" transform="rotate(300)"/><circle r="8" fill="#ffd66e"/></g></svg>
<h1>Flower <small>Companion</small></h1>
</div>
<nav class="tabs">
<button data-tab="books" class="active">Biblioteka</button>
<button data-tab="settings">Ustawienia</button>
<button data-tab="firmware">Aktualizacja</button>
<button data-tab="help">Pomoc</button>
</nav>
</header>
<main>
<div id="status" class="status">Łączenie z czytnikiem...</div>

<section id="books" class="page active">
<a class="link-banner" href="https://grkarol.github.io/czytnik01/app/" target="_blank" rel="noopener">
Konwerter EPUB / PDF / MOBI → .rsvp
<span>Otwórz w nowej karcie, przekonwertuj książkę, a potem wróć tu i wgraj plik.</span>
</a>
<div class="card"><h2>Wgraj książkę</h2>
<p class="muted">Wybierz plik <code>.rsvp</code>, <code>.txt</code> lub <code>.epub</code> — trafi prosto na czytnik.</p>
<label>Plik książki</label><input id="bookFileInput" type="file" accept=".rsvp,.txt,.epub">
<p><button class="primary" id="uploadBookButton">Wyślij na czytnik</button></p>
</div>
<div class="card"><h2>Na czytniku</h2><div id="infoBox" class="muted">Ładowanie...</div><p><button id="refreshBooksButton">Odśwież</button></p></div>
<div class="card"><h2>Twoje książki</h2><div id="booksList" class="muted">Ładowanie...</div></div>
</section>

<section id="settings" class="page">
<div class="group"><p class="group-title">Czytanie</p>
<label>Tryb czytnika</label><select id="readerMode"><option value="rsvp">RSVP</option><option value="scroll">Przewijanie</option></select>
<label>Pauza</label><select id="pauseMode"><option value="sentence_end">Koniec zdania</option><option value="instant">Natychmiastowa</option></select>
<label>Tempo <span id="wpmValue"></span></label><input id="wpm" type="range" min="10" max="1000" step="5">
<label>Długie słowa <span id="longWordMsValue"></span></label><input id="longWordMs" type="range" min="0" max="600" step="50">
<label>Złożone słowa <span id="complexWordMsValue"></span></label><input id="complexWordMs" type="range" min="0" max="600" step="50">
<label>Interpunkcja <span id="punctuationMsValue"></span></label><input id="punctuationMs" type="range" min="0" max="600" step="50">
</div>
<div class="group"><p class="group-title">Wyświetlanie</p>
<label>Motyw</label><select id="displayMode"><option value="dark">Ciemny</option><option value="light">Jasny</option><option value="night">Nocny</option></select>
<label>Jasność <span id="brightnessValue"></span></label><input id="brightnessIndex" type="range" min="0" max="4">
<label>Dłoń</label><select id="handedness"><option value="right">Prawa</option><option value="left">Lewa</option></select>
<label>Metryka stopki</label><select id="footerMetric"><option value="percentage">Procent</option><option value="chapter_time">Czas rozdziału</option><option value="book_time">Czas książki</option></select>
<label>Etykieta baterii</label><select id="batteryLabel"><option value="percent">Procent</option><option value="time_remaining">Czas</option><option value="voltage">Napięcie</option></select>
<label><input id="readingBattery" type="checkbox"> Bateria podczas czytania</label>
<label><input id="readingChapter" type="checkbox"> Rozdział podczas czytania</label>
<label><input id="readingProgress" type="checkbox"> Postęp podczas czytania</label>
</div>
<div class="group"><p class="group-title">Typografia</p>
<label>Krój czcionki</label><select id="typeface"><option value="standard">Standard</option><option value="open_dyslexic">OpenDyslexic</option><option value="atkinson">Atkinson</option></select>
<label>Rozmiar <span id="fontSizeValue"></span></label><input id="fontSizeIndex" type="range" min="0" max="2">
<label>Tracking <span id="trackingValue"></span></label><input id="tracking" type="range" min="-2" max="3">
<label>Kotwica <span id="anchorValue"></span></label><input id="anchorPercent" type="range" min="30" max="40">
<label>Prowadnica szer. <span id="guideWidthValue"></span></label><input id="guideWidth" type="range" min="12" max="30" step="2">
<label>Prowadnica przerwa <span id="guideGapValue"></span></label><input id="guideGap" type="range" min="2" max="8">
<label><input id="focusHighlight" type="checkbox"> Podświetlenie fokusowe</label>
<label><input id="phantomWords" type="checkbox"> Słowa widma</label>
</div>
<div class="group"><p class="group-title">Scroll</p>
<label>Rozmiar czcionki <span id="scrollFontSizeValue"></span></label><input id="scrollFontSize" type="range" min="0" max="8">
<label>Interlinia <span id="scrollLineSpacingValue"></span></label><input id="scrollLineSpacing" type="range" min="0" max="2">
<label>Marginesy <span id="scrollMarginValue"></span></label><input id="scrollMargin" type="range" min="0" max="2">
</div>
<div class="group"><p class="group-title">Wi-Fi domowe</p>
<p class="muted">Zapisz sieć domową dla OTA i RSS. Czytnik nie odsyła zapisanego hasła.</p>
<label>SSID</label><input id="wifiSsid" autocomplete="off" placeholder="Nazwa sieci">
<label>Hasło</label><input id="wifiPassword" type="password" autocomplete="new-password" placeholder="Puste = sieć otwarta">
<div class="row"><button class="primary" id="saveWifiButton">Zapisz WiFi</button><button class="danger" id="forgetWifiButton">Zapomnij</button></div>
<p id="wifiCurrent" class="muted"></p>
</div>
<p><button class="primary" id="saveSettingsButton">Zapisz ustawienia</button></p>
</section>

<section id="firmware" class="page">
<div class="card">
<h2>Aktualizacja firmware</h2>
<p class="muted">Pobierz najnowszy <code>.bin</code> z GitHub Releases, a potem wgraj go tutaj. Czytnik zrestartuje się po udanej aktualizacji.</p>
<a class="link-banner" href="https://github.com/GRKarol/czytnik01/releases/latest" target="_blank" rel="noopener">
Pobierz najnowszy firmware z GitHub
<span>Szukaj pliku flower-firmware.bin</span>
</a>
<label>Plik firmware (.bin)</label>
<input type="file" id="firmwareFile" accept=".bin">
<p><button class="primary" id="uploadFirmwareButton">Zainstaluj</button></p>
<div id="firmwareProgress" class="muted">Gotowy.</div>
</div>
</section>

<section id="help" class="page">
<div class="card"><h2>Jak korzystać</h2>
<p>1. Na czytniku włącz <strong>Sync z telefonem</strong>.</p>
<p>2. Połącz telefon z siecią WiFi <code>Flower-...</code>.</p>
<p>3. Otwórz w przeglądarce adres IP pokazany na ekranie czytnika.</p>
<p>4. Zarządzaj książkami i ustawieniami — zmiany synchronizują się natychmiast.</p>
<h3>Konwersja książek</h3>
<p>Kliknij link <em>Konwerter</em> u góry zakładki Biblioteka. Przekonwertuj EPUB/PDF/MOBI/TXT na format <code>.rsvp</code>, pobierz plik, wróć tu i wgraj go na czytnik.</p>
<h3>Artykuły</h3>
<p>Artykuły to krótkie teksty (newslettery, blogi) pobierane z RSS lub wklejane ręcznie. Pojawiają się w osobnym folderze na czytniku.</p>
</div>
</section>
</main>
<script>
const $=id=>document.getElementById(id);let settings=null;
function status(msg){$('status').textContent=msg}
async function api(path,opts){const r=await fetch(path,opts);const t=await r.text();let j={};try{j=t?JSON.parse(t):{}}catch(e){throw new Error(t||'Nieprawidłowa odpowiedź')}if(!r.ok||j.ok===false)throw new Error(j.error||r.statusText);return j}
function bytes(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB'}
function html(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function renderList(id,items){$(id).innerHTML=items.length?items.map(b=>'<div class="item"><div class="item-info"><div class="item-title">'+html(b.title||b.name)+'</div><div class="item-meta">'+html([b.author,bytes(b.bytes),b.progressPercent!=null?b.progressPercent+'%':''].filter(Boolean).join(' · '))+'</div></div><button class="danger" data-delete="'+html(encodeURIComponent(b.name))+'">Usuń</button></div>').join(''):'<span class="muted">Pusto — wgraj pierwszą książkę.</span>';document.querySelectorAll('[data-delete]').forEach(b=>b.onclick=()=>delBook(decodeURIComponent(b.dataset.delete)))}
async function refresh(){try{const info=await api('/api/info');$('infoBox').innerHTML='<strong>'+html(info.name)+'</strong><br><span class="muted">'+html(info.mode)+' · '+html(info.networkSsid||'')+'</span>';const data=await api('/api/books');renderList('booksList',data.books);status('Połączono z czytnikiem.')}catch(e){status('Problem z połączeniem: '+e.message)}}
async function delBook(name){if(!confirm('Usunąć „'+decodeURIComponent(name)+'"?'))return;try{await api('/api/books?name='+encodeURIComponent(name),{method:'DELETE'});await refresh();status('Usunięto.')}catch(e){status('Błąd usuwania: '+e.message)}}
async function uploadPicked(inputId,category){const f=$(inputId).files[0];if(!f){status('Najpierw wybierz plik.');return}const fd=new FormData();fd.append('file',f,f.name);const xhr=new XMLHttpRequest();xhr.open('POST','/api/books?name='+encodeURIComponent(f.name)+'&category='+encodeURIComponent(category));xhr.upload.onprogress=e=>{if(e.lengthComputable){const pct=Math.round(e.loaded*100/e.total);status('Wysylanie: '+pct+'% ('+Math.round(e.loaded/1024)+'/'+Math.round(e.total/1024)+' KB) - '+f.name)}};xhr.onload=()=>{if(xhr.status>=200&&xhr.status<300){$(inputId).value='';refresh();status('Wgrano: '+f.name)}else{status('Blad uploadu: '+xhr.responseText)}};xhr.onerror=()=>{status('Polaczenie zerwane podczas uploadu.')};status('Wysylanie: 0% - '+f.name+' ('+bytes(f.size)+')');xhr.send(fd)}
function val(id){const e=$(id);return e.type==='checkbox'?e.checked:e.value}
function setVal(id,v){const e=$(id);if(!e)return;if(e.type==='checkbox')e.checked=!!v;else e.value=v}
function updateLabels(){['wpm','longWordMs','complexWordMs','punctuationMs','brightnessIndex','fontSizeIndex','tracking','anchorPercent','guideWidth','guideGap','scrollFontSize','scrollLineSpacing','scrollMargin'].forEach(id=>{const l=$(id+'Value');if(l)l.textContent=$(id).value+(id==='wpm'?' WPM':id.includes('Ms')?' ms':'')})}
async function loadSettings(){try{settings=await api('/api/settings');const r=settings.reading||{},d=settings.display||{},t=settings.typography||{},sc=settings.scroll||{};setVal('readerMode',r.readerMode);setVal('pauseMode',r.pauseMode);setVal('wpm',r.wpm);setVal('longWordMs',(r.pacing||{}).longWordMs);setVal('complexWordMs',(r.pacing||{}).complexWordMs);setVal('punctuationMs',(r.pacing||{}).punctuationMs);setVal('displayMode',d.nightMode?'night':d.darkMode?'dark':'light');setVal('brightnessIndex',d.brightnessIndex);setVal('handedness',d.handedness);setVal('footerMetric',d.footerMetric);setVal('batteryLabel',d.batteryLabel);setVal('readingBattery',d.readingBattery);setVal('readingChapter',d.readingChapter);setVal('readingProgress',d.readingProgress);setVal('typeface',t.typeface);setVal('fontSizeIndex',d.fontSizeIndex);setVal('tracking',t.tracking);setVal('anchorPercent',t.anchorPercent);setVal('guideWidth',t.guideWidth);setVal('guideGap',t.guideGap);setVal('focusHighlight',t.focusHighlight);setVal('phantomWords',d.phantomWords);setVal('scrollFontSize',sc.scrollFontSize);setVal('scrollLineSpacing',sc.scrollLineSpacing);setVal('scrollMargin',sc.scrollMargin);updateLabels();status('Ustawienia wczytane.')}catch(e){status('Błąd wczytywania ustawień: '+e.message)}}
async function saveSettings(){const mode=val('displayMode');const payload={wpm:+val('wpm'),readerMode:val('readerMode'),pauseMode:val('pauseMode'),longWordMs:+val('longWordMs'),complexWordMs:+val('complexWordMs'),punctuationMs:+val('punctuationMs'),darkMode:mode==='dark'||mode==='night',nightMode:mode==='night',brightnessIndex:+val('brightnessIndex'),handedness:val('handedness'),footerMetric:val('footerMetric'),batteryLabel:val('batteryLabel'),readingBattery:val('readingBattery'),readingChapter:val('readingChapter'),readingProgress:val('readingProgress'),phantomWords:val('phantomWords'),fontSizeIndex:+val('fontSizeIndex'),typeface:val('typeface'),focusHighlight:val('focusHighlight'),tracking:+val('tracking'),anchorPercent:+val('anchorPercent'),guideWidth:+val('guideWidth'),guideGap:+val('guideGap'),scrollFontSize:+val('scrollFontSize'),scrollLineSpacing:+val('scrollLineSpacing'),scrollMargin:+val('scrollMargin')};try{await api('/api/settings',{method:'PATCH',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});status('Ustawienia zapisane! Wyłącz i włącz sync żeby zastosować.')}catch(e){status('Błąd zapisu: '+e.message)}}
async function loadWifi(){try{const w=await api('/api/wifi');$('wifiSsid').value=w.ssid||'';$('wifiPassword').value='';$('wifiCurrent').textContent=w.configured?'Zapisana sieć: '+w.ssid:'Brak zapisanej sieci.'}catch(e){}}
async function saveWifi(){const ssid=$('wifiSsid').value.trim();if(!ssid){status('Wpisz nazwę sieci.');return}try{await api('/api/wifi',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password:$('wifiPassword').value})});$('wifiPassword').value='';$('wifiCurrent').textContent='Zapisana sieć: '+ssid;status('WiFi zapisane.')}catch(e){status('Błąd zapisu WiFi: '+e.message)}}
async function forgetWifi(){if(!confirm('Zapomnieć zapisane WiFi?'))return;try{await api('/api/wifi',{method:'DELETE'});$('wifiSsid').value='';$('wifiPassword').value='';$('wifiCurrent').textContent='Brak zapisanej sieci.';status('WiFi usunięte.')}catch(e){status('Błąd: '+e.message)}}
function uploadFirmware(){const f=$('firmwareFile').files[0];if(!f){status('Wybierz plik .bin.');return}const fd=new FormData();fd.append('firmware',f,f.name);const xhr=new XMLHttpRequest();xhr.open('POST','/api/ota');xhr.upload.onprogress=e=>{if(e.lengthComputable){const pct=Math.round(e.loaded*100/e.total);$('firmwareProgress').textContent='Wysyłanie '+pct+'% ('+Math.round(e.loaded/1024)+' / '+Math.round(e.total/1024)+' kB)';status('Upload firmware '+pct+'%')}};xhr.onload=()=>{if(xhr.status>=200&&xhr.status<300){$('firmwareProgress').textContent='Zainstalowano. Czytnik się restartuje.';status('Firmware zainstalowany!')}else{$('firmwareProgress').textContent='Błąd: '+xhr.responseText;status('OTA nie powiodło się.')}};xhr.onerror=()=>{$('firmwareProgress').textContent='Połączenie zerwane.';status('Połączenie zerwane podczas OTA.')};status('Wysyłanie firmware...');xhr.send(fd)}
document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tabs button,.page').forEach(x=>x.classList.remove('active'));b.classList.add('active');$(b.dataset.tab).classList.add('active');if(b.dataset.tab==='settings'){loadSettings();loadWifi()}});
['wpm','longWordMs','complexWordMs','punctuationMs','brightnessIndex','fontSizeIndex','tracking','anchorPercent','guideWidth','guideGap','scrollFontSize','scrollLineSpacing','scrollMargin'].forEach(id=>{const e=$(id);if(e)e.oninput=updateLabels});
$('refreshBooksButton').onclick=refresh;$('uploadBookButton').onclick=()=>uploadPicked('bookFileInput','book');$('saveSettingsButton').onclick=saveSettings;$('saveWifiButton').onclick=saveWifi;$('forgetWifiButton').onclick=forgetWifi;$('uploadFirmwareButton').onclick=uploadFirmware;
refresh();
</script>
</body>
</html>)HTML";

bool isSafeFilenameChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == ' ';
}

String ipToString(IPAddress ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

String stripBom(String value) {
  if (value.length() >= 3 && static_cast<uint8_t>(value[0]) == 0xEF &&
      static_cast<uint8_t>(value[1]) == 0xBB && static_cast<uint8_t>(value[2]) == 0xBF) {
    value.remove(0, 3);
  }
  return value;
}

bool directiveMatches(const String &loweredLine, const char *directive) {
  if (!loweredLine.startsWith(directive)) {
    return false;
  }
  const size_t directiveLength = strlen(directive);
  return loweredLine.length() == directiveLength ||
         isspace(static_cast<unsigned char>(loweredLine[directiveLength]));
}

String directiveValue(const String &line, const char *directive) {
  String value = line.substring(strlen(directive));
  value.trim();
  return value;
}

bool isSupportedBookName(const String &loweredName) {
  return loweredName.endsWith(".rsvp") || loweredName.endsWith(".txt") ||
         loweredName.endsWith(".epub");
}

String displayNameForPath(const String &path) {
  const int separator = path.lastIndexOf('/');
  if (separator < 0) {
    return path;
  }
  return path.substring(separator + 1);
}

String relativeLibraryName(const String &path) {
  const String prefix = String(kBooksPath) + "/";
  if (path.startsWith(prefix)) {
    return path.substring(prefix.length());
  }
  return displayNameForPath(path);
}

String libraryCategoryForPath(const String &path) {
  const String relative = relativeLibraryName(path);
  if (relative.startsWith("articles/")) {
    return "article";
  }
  if (relative.startsWith("books/")) {
    return "book";
  }
  return "legacy";
}

uint16_t clampU16(uint16_t value, uint16_t minValue, uint16_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

int clampInt(int value, int minValue, int maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

String enumLabel(uint8_t value, const char *const *labels, size_t count, uint8_t fallback = 0) {
  if (value >= count) {
    value = fallback;
  }
  return labels[value];
}

int enumValue(const String &value, const char *const *labels, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (value == labels[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool findJsonKey(const String &body, const char *key, int &colonIndex) {
  const String needle = String("\"") + key + "\"";
  const int keyIndex = body.indexOf(needle);
  if (keyIndex < 0) {
    return false;
  }
  colonIndex = body.indexOf(':', keyIndex + needle.length());
  return colonIndex >= 0;
}

int skipJsonWhitespace(const String &body, int index) {
  while (index < static_cast<int>(body.length()) &&
         isspace(static_cast<unsigned char>(body[index]))) {
    ++index;
  }
  return index;
}

bool readJsonInt(const String &body, const char *key, int &value) {
  int colonIndex = -1;
  if (!findJsonKey(body, key, colonIndex)) {
    return false;
  }
  int index = skipJsonWhitespace(body, colonIndex + 1);
  bool negative = false;
  if (index < static_cast<int>(body.length()) && body[index] == '-') {
    negative = true;
    ++index;
  }
  if (index >= static_cast<int>(body.length()) || !isdigit(static_cast<unsigned char>(body[index]))) {
    return false;
  }
  int result = 0;
  while (index < static_cast<int>(body.length()) &&
         isdigit(static_cast<unsigned char>(body[index]))) {
    result = result * 10 + (body[index] - '0');
    ++index;
  }
  value = negative ? -result : result;
  return true;
}

bool readJsonBool(const String &body, const char *key, bool &value) {
  int colonIndex = -1;
  if (!findJsonKey(body, key, colonIndex)) {
    return false;
  }
  const int index = skipJsonWhitespace(body, colonIndex + 1);
  if (body.substring(index, index + 4) == "true") {
    value = true;
    return true;
  }
  if (body.substring(index, index + 5) == "false") {
    value = false;
    return true;
  }
  return false;
}

bool readJsonString(const String &body, const char *key, String &value) {
  int colonIndex = -1;
  if (!findJsonKey(body, key, colonIndex)) {
    return false;
  }
  int index = skipJsonWhitespace(body, colonIndex + 1);
  if (index >= static_cast<int>(body.length()) || body[index] != '"') {
    return false;
  }
  ++index;
  String result;
  while (index < static_cast<int>(body.length())) {
    const char c = body[index++];
    if (c == '"') {
      value = result;
      return true;
    }
    if (c == '\\' && index < static_cast<int>(body.length())) {
      const char escaped = body[index++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result += escaped;
          break;
        case 'n':
          result += '\n';
          break;
        case 'r':
          result += '\r';
          break;
        case 't':
          result += '\t';
          break;
        default:
          result += escaped;
          break;
      }
    } else {
      result += c;
    }
  }
  return false;
}

bool isHttpUrl(String value) {
  value.trim();
  value.toLowerCase();
  return value.startsWith("http://") || value.startsWith("https://");
}

bool nextJsonArrayString(const String &body, int &index, String &value) {
  index = skipJsonWhitespace(body, index);
  if (index >= static_cast<int>(body.length())) {
    return false;
  }
  if (body[index] == ',') {
    index = skipJsonWhitespace(body, index + 1);
  }
  if (index >= static_cast<int>(body.length()) || body[index] == ']') {
    return false;
  }
  if (body[index] != '"') {
    return false;
  }
  ++index;
  String result;
  while (index < static_cast<int>(body.length())) {
    const char c = body[index++];
    if (c == '"') {
      value = result;
      return true;
    }
    if (c == '\\' && index < static_cast<int>(body.length())) {
      const char escaped = body[index++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result += escaped;
          break;
        case 'n':
          result += '\n';
          break;
        case 'r':
          result += '\r';
          break;
        case 't':
          result += '\t';
          break;
        default:
          result += escaped;
          break;
      }
    } else {
      result += c;
    }
  }
  return false;
}

String rsvpMetadataValueFromLine(const String &line, const char *directive, bool &pastDirectives) {
  String trimmed = stripBom(line);
  trimmed.trim();
  if (trimmed.isEmpty()) {
    return "";
  }

  String lowered = trimmed;
  lowered.toLowerCase();
  if (directiveMatches(lowered, directive)) {
    return directiveValue(trimmed, directive);
  }

  if (!trimmed.startsWith("@")) {
    pastDirectives = true;
  }
  return "";
}

}  // namespace

CompanionSyncManager *CompanionSyncManager::instance_ = nullptr;

bool CompanionSyncManager::begin(const Config &config) {
  (void)config;
  if (active_) {
    return true;
  }

  instance_ = this;
  pairingCode_ = String(static_cast<uint32_t>(esp_random()) % 900000UL + 100000UL);
  statusLine1_ = "Starting sync";
  statusLine2_ = "Preparing Wi-Fi";
  preferences_.begin(kPrefsNamespace, false);

  const bool networkReady = startAccessPoint();
  if (!networkReady) {
    statusLine1_ = "Wi-Fi failed";
    statusLine2_ = "";
    end();
    return false;
  }

  if (!startServer()) {
    statusLine1_ = "HTTP failed";
    statusLine2_ = "";
    end();
    return false;
  }

  active_ = true;
  statusLine1_ = networkSsid_;
  statusLine2_ = baseUrl();
  Serial.printf("[sync] ready ssid=%s url=%s pairing=%s\n", networkSsid_.c_str(), baseUrl().c_str(),
                pairingCode_.c_str());
  return true;
}

void CompanionSyncManager::update() {
  if (!active_ || !serverStarted_) {
    return;
  }
  if (networkMode_ == NetworkMode::AccessPoint) {
    dnsServer_.processNextRequest();
  }

  // Process at most a few HTTP requests per update() call to prevent
  // display starvation when bombarded with captive portal checks.
  // WebServer::handleClient() processes one request; calling it a limited
  // number of times per frame keeps the loop responsive.
  for (int i = 0; i < 3; ++i) {
    server_.handleClient();
    yield();  // Give FreeRTOS idle task + watchdog a chance
  }

  // UDP broadcast co 2s — natywna app może nasłuchiwać zamiast pollingu HTTP.
  // Pakiet: "FLOWER|192.168.4.1|<firmwareVersion>|<pairingCode>"
  const uint32_t nowMs = millis();
  if (nowMs - lastBroadcastMs_ >= 2000) {
    lastBroadcastMs_ = nowMs;
    const IPAddress broadcastIp(192, 168, 4, 255);
    String packet = "FLOWER|" + ipToString(WiFi.softAPIP()) + "|" +
                    String(RSVP_FIRMWARE_VERSION) + "|" + pairingCode_;
    udpBroadcast_.beginPacket(broadcastIp, 5555);
    udpBroadcast_.write(reinterpret_cast<const uint8_t *>(packet.c_str()), packet.length());
    udpBroadcast_.endPacket();
  }
}

void CompanionSyncManager::end() {
  stopServer();

  // Same mutex as startAccessPoint() above — this teardown races the exact
  // same background download tasks the same way.
  xSemaphoreTake(wifiSessionMutex(), portMAX_DELAY);
  if (networkMode_ == NetworkMode::Station) {
    WiFi.disconnect(true, false);
  } else if (networkMode_ == NetworkMode::AccessPoint) {
    WiFi.softAPdisconnect(true);
  }
  WiFi.mode(WIFI_OFF);
  xSemaphoreGive(wifiSessionMutex());
  preferences_.end();

  networkMode_ = NetworkMode::None;
  active_ = false;
  statusLine1_ = "Idle";
  statusLine2_ = "";
  qrSize_ = 0;
  instance_ = nullptr;
}

bool CompanionSyncManager::active() const { return active_; }

String CompanionSyncManager::statusLine1() const { return statusLine1_; }

String CompanionSyncManager::statusLine2() const { return statusLine2_; }

String CompanionSyncManager::baseUrl() const {
  if (networkMode_ == NetworkMode::Station) {
    return "http://" + ipToString(WiFi.localIP());
  }
  if (networkMode_ == NetworkMode::AccessPoint) {
    return "http://" + ipToString(WiFi.softAPIP());
  }
  return "";
}

bool CompanionSyncManager::hasQrCode() const { return qrSize_ > 0; }

const bool *CompanionSyncManager::qrCodeData() const { return qrData_; }

uint8_t CompanionSyncManager::qrCodeSize() const { return qrSize_; }

void CompanionSyncManager::handleInfoStatic() {
  if (instance_ != nullptr) {
    instance_->handleInfo();
  }
}

void CompanionSyncManager::handleHelloStatic() {
  if (instance_ != nullptr) {
    instance_->handleHello();
  }
}

void CompanionSyncManager::handleHello() {
  sendCorsHeaders();
  String body;
  body.reserve(256);
  body += "{\"ok\":true,\"name\":\"Flower Reader\"";
  body += ",\"api\":1";
  body += ",\"ssid\":\"";
  body += jsonEscape(networkSsid_);
  body += "\",\"host\":\"";
  body += ipToString(networkMode_ == NetworkMode::Station ? WiFi.localIP() : WiFi.softAPIP());
  body += "\",\"wifiOpen\":true";
  body += ",\"firmwareVersion\":\"";
  body += RSVP_FIRMWARE_VERSION;
  body += "\"}";
  server_.send(200, "application/json", body);
}

void CompanionSyncManager::handleRootStatic() {
  if (instance_ != nullptr) {
    instance_->handleRoot();
  }
}

void CompanionSyncManager::handleBooksListStatic() {
  if (instance_ != nullptr) {
    instance_->handleBooksList();
  }
}

void CompanionSyncManager::handleSettingsStatic() {
  if (instance_ != nullptr) {
    instance_->handleSettings();
  }
}

void CompanionSyncManager::handleWifiStatic() {
  if (instance_ != nullptr) {
    instance_->handleWifi();
  }
}

void CompanionSyncManager::handleRssFeedsStatic() {
  if (instance_ != nullptr) {
    instance_->handleRssFeeds();
  }
}

void CompanionSyncManager::handleBookDeleteStatic() {
  if (instance_ != nullptr) {
    instance_->handleBookDelete();
  }
}

void CompanionSyncManager::handleBooksStatic() {
  if (instance_ != nullptr) {
    instance_->handleBooks();
  }
}

void CompanionSyncManager::handleBookUploadStatic() {
  if (instance_ != nullptr) {
    instance_->handleBookUpload();
  }
}

void CompanionSyncManager::handleOtaStatic() {
  if (instance_ != nullptr) {
    instance_->handleOta();
  }
}

void CompanionSyncManager::handleOtaUploadStatic() {
  if (instance_ != nullptr) {
    instance_->handleOtaUpload();
  }
}

void CompanionSyncManager::handleNotFoundStatic() {
  if (instance_ != nullptr) {
    instance_->handleNotFound();
  }
}

void CompanionSyncManager::handleCapabilitiesStatic() {
  if (instance_ != nullptr) {
    instance_->handleCapabilities();
  }
}

void CompanionSyncManager::handlePluginsStatic() {
  if (instance_ != nullptr) {
    instance_->handlePlugins();
  }
}

void CompanionSyncManager::handlePluginsDeleteStatic() {
  if (instance_ != nullptr) {
    instance_->handlePluginsDelete();
  }
}

void CompanionSyncManager::handlePowerWifiTimeoutStatic() {
  if (instance_ != nullptr) {
    instance_->handlePowerWifiTimeout();
  }
}

void CompanionSyncManager::handleOptionsStatic() {
  if (instance_ != nullptr) {
    instance_->handleOptions();
  }
}

void CompanionSyncManager::handleStateStatic() {
  if (instance_ != nullptr) {
    instance_->handleState();
  }
}

void CompanionSyncManager::handleLogTailStatic() {
  if (instance_ != nullptr) {
    instance_->handleLogTail();
  }
}

void CompanionSyncManager::handleLangCodesStatic() {
  if (instance_ != nullptr) {
    instance_->handleLangCodes();
  }
}

void CompanionSyncManager::handleBookPositionStatic() {
  if (instance_ != nullptr) {
    instance_->handleBookPosition();
  }
}

void CompanionSyncManager::handleLogClearStatic() {
  if (instance_ != nullptr) {
    instance_->handleLogClear();
  }
}

void CompanionSyncManager::setDeviceStatus(uint8_t batteryPercent, uint32_t sdFreeKb, uint32_t sdTotalKb) {
  deviceBatteryPercent_ = batteryPercent;
  deviceSdFreeKb_ = sdFreeKb;
  deviceSdTotalKb_ = sdTotalKb;
}

bool CompanionSyncManager::startAccessPoint() {
  const String ssid = "Flower-" + deviceSuffix();
  statusLine1_ = "Sync Wi-Fi";
  statusLine2_ = ssid;
  networkSsid_ = ssid;
  // Same WiFi-driver mutex OtaUpdater::connectWiFi()/disconnectWiFi() use —
  // without it, entering this screen while a font/book background download
  // (started earlier in the wizard) still holds an active STA/TLS session on
  // the other core raced WiFi.mode(WIFI_AP) against that session's own
  // WiFi.mode() calls, crashing with the same LoadProhibited PANIC that hit
  // the STA-side teardown race (see wifiSessionMutex()'s doc comment) — the
  // reboot-back-to-language-screen bug on "Connect your reader to the app".
  xSemaphoreTake(wifiSessionMutex(), portMAX_DELAY);
  WiFi.mode(WIFI_AP);
  const bool apStarted = WiFi.softAP(ssid.c_str());
  xSemaphoreGive(wifiSessionMutex());
  if (!apStarted) {
    Serial.println("[sync] softAP failed");
    return false;
  }

  networkMode_ = NetworkMode::AccessPoint;
  Serial.printf("[sync] softAP ssid=%s ip=%s\n", ssid.c_str(), ipToString(WiFi.softAPIP()).c_str());

  // Generate BLE pairing QR code (Protocol v2: flower://pair?t=...&n=...)
  // instead of WiFi QR (old format). App scans this to get token + BLE name.
  {
    extern BleApi *g_bleApiPtr;  // set by App during BLE init
    String qrPayload;
    if (g_bleApiPtr && g_bleApiPtr->hasToken()) {
      qrPayload = g_bleApiPtr->qrPayload();
    } else {
      // Fallback: WiFi QR if no BLE token available
      qrPayload = "WIFI:T:nopass;S:" + ssid + ";;";
    }
    // Generate QR using qrcode library directly
    QRCode qrcode;
    // Use version 6 for longer payloads (flower://pair URL is ~90 chars)
    uint8_t qrcodeData[qrcode_getBufferSize(6)];
    int8_t result = qrcode_initText(&qrcode, qrcodeData, 6, ECC_LOW, qrPayload.c_str());
    if (result == 0 && qrcode.size <= 64) {
      qrSize_ = qrcode.size;
      for (uint8_t y = 0; y < qrSize_; y++) {
        for (uint8_t x = 0; x < qrSize_; x++) {
          qrData_[y * qrSize_ + x] = qrcode_getModule(&qrcode, x, y);
        }
      }
      Serial.printf("[sync] BLE pairing QR generated: %dx%d payload=%s\n",
                    qrSize_, qrSize_, qrPayload.substring(0, 30).c_str());
    } else {
      qrSize_ = 0;
      Serial.printf("[sync] QR generation failed (result=%d size=%d)\n", result,
                    result == 0 ? qrcode.size : 0);
    }
  }

  return true;
}

bool CompanionSyncManager::startServer() {
  server_.on("/", HTTP_GET, handleRootStatic);
  // Captive portal detection endpoints — odpowiadamy 204 żeby Android/iOS
  // nie wyświetlał "sieć bez internetu" i nie rozłączał WiFi.
  server_.on("/generate_204", HTTP_GET, []() {
    instance_->logPortalHit("generate_204", instance_->server_.client().remoteIP().toString());
    instance_->server_.send(204, "text/plain", "");
  });
  server_.on("/gen_204", HTTP_GET, []() {
    instance_->logPortalHit("gen_204", instance_->server_.client().remoteIP().toString());
    instance_->server_.send(204, "text/plain", "");
  });
  server_.on("/hotspot-detect.html", HTTP_GET, []() {
    instance_->logPortalHit("hotspot-detect", instance_->server_.client().remoteIP().toString());
    instance_->server_.send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server_.on("/connecttest.txt", HTTP_GET, []() {
    instance_->logPortalHit("connecttest", instance_->server_.client().remoteIP().toString());
    instance_->server_.send(200, "text/plain", "Microsoft Connect Test");
  });
  // HyperOS / MIUI connectivity check
  server_.on("/ncsi.txt", HTTP_GET, []() {
    instance_->logPortalHit("ncsi", instance_->server_.client().remoteIP().toString());
    instance_->server_.send(200, "text/plain", "Microsoft NCSI");
  });
  // Xiaomi/HyperOS specific check
  server_.on("/redirect", HTTP_GET, []() {
    instance_->logPortalHit("redirect", instance_->server_.client().remoteIP().toString());
    instance_->server_.send(204, "text/plain", "");
  });
  server_.on("/check_network", HTTP_GET, []() {
    instance_->logPortalHit("check_network", instance_->server_.client().remoteIP().toString());
    instance_->server_.send(204, "text/plain", "");
  });
  // Mini-handshake dla aplikacji-towarzysza (Flower PWA). Wskazuje
  // jednoznacznie że to nasze urządzenie (a nie czyjeś AP o podobnej nazwie).
  server_.on("/api/hello", HTTP_GET, handleHelloStatic);
  server_.on("/api/info", HTTP_GET, handleInfoStatic);
  server_.on("/api/capabilities", HTTP_GET, handleCapabilitiesStatic);
  server_.on("/api/books", HTTP_GET, handleBooksListStatic);
  server_.on("/api/books", HTTP_DELETE, handleBookDeleteStatic);
  server_.on("/api/books", HTTP_POST, handleBooksStatic, handleBookUploadStatic);
  // OTA przez WiFi — PWA „Aktualizacje" wysyła tu pobranego asseta z
  // GitHub Releases (preferowany: flower-firmware.bin, sama aplikacja
  // pasująca do Update.h). Po sukcesie urządzenie się restartuje.
  server_.on("/api/ota", HTTP_POST, handleOtaStatic, handleOtaUploadStatic);
  server_.on("/api/settings", HTTP_GET, handleSettingsStatic);
  server_.on("/api/settings", HTTP_PATCH, handleSettingsStatic);
  server_.on("/api/settings", HTTP_PUT, handleSettingsStatic);
  server_.on("/api/wifi", HTTP_GET, handleWifiStatic);
  server_.on("/api/wifi", HTTP_PUT, handleWifiStatic);
  server_.on("/api/wifi", HTTP_DELETE, handleWifiStatic);
  server_.on("/api/rss-feeds", HTTP_GET, handleRssFeedsStatic);
  server_.on("/api/rss-feeds", HTTP_PUT, handleRssFeedsStatic);
  server_.on("/api/plugins", HTTP_GET, handlePluginsStatic);
  server_.on("/api/plugins", HTTP_DELETE, handlePluginsDeleteStatic);
  server_.on("/api/power/wifi-timeout", HTTP_POST, handlePowerWifiTimeoutStatic);
  server_.on("/api/state", HTTP_GET, handleStateStatic);
  server_.on("/api/log/tail", HTTP_GET, handleLogTailStatic);
  server_.on("/api/log", HTTP_DELETE, handleLogClearStatic);
  server_.on("/api/lang/codes", HTTP_GET, handleLangCodesStatic);
  server_.on("/api/books/position", HTTP_GET, handleBookPositionStatic);
  server_.on("/api/books/position", HTTP_PUT, handleBookPositionStatic);
  // CORS preflight dla wszystkich endpointów
  server_.on("/api/hello", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/info", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/capabilities", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/books", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/ota", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/settings", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/wifi", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/rss-feeds", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/plugins", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/power/wifi-timeout", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/state", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/log/tail", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/log", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/lang/codes", HTTP_OPTIONS, handleOptionsStatic);
  server_.on("/api/books/position", HTTP_OPTIONS, handleOptionsStatic);
  server_.onNotFound(handleNotFoundStatic);
  server_.begin();
  serverStarted_ = true;

  // W trybie AP startujemy DNS server który odpowiada na KAŻDE zapytanie
  // adresem 192.168.4.1 — to zapobiega captive portal detection na Androidzie
  // (telefon nie wyświetla "ta sieć nie ma internetu").
  if (networkMode_ == NetworkMode::AccessPoint) {
    dnsServer_.start(53, "*", WiFi.softAPIP());
  }

  if (networkMode_ == NetworkMode::Station && MDNS.begin(kMdnsName)) {
    MDNS.addService("http", "tcp", 80);
  }
  return true;
}

void CompanionSyncManager::stopServer() {
  if (serverStarted_) {
    server_.stop();
    dnsServer_.stop();
    MDNS.end();
  }
  finishUpload(false);
  serverStarted_ = false;
}

void CompanionSyncManager::handleInfo() {
  sendCorsHeaders();
  const String mode = networkMode_ == NetworkMode::Station ? "station" : "access_point";
  const String body = String("{") + "\"ok\":true," +
                      "\"name\":\"Flower\"," +
                      "\"mode\":\"" + mode + "\"," +
                      "\"baseUrl\":\"" + jsonEscape(baseUrl()) + "\"," +
                      "\"networkSsid\":\"" + jsonEscape(networkSsid_) + "\"," +
                      "\"pairingCode\":\"" + pairingCode_ + "\"," +
                      "\"uploadPath\":\"/api/books\"," +
                      "\"api\":1," +
                      "\"firmwareVersion\":\"" + jsonEscape(RSVP_FIRMWARE_VERSION) + "\"," +
                      "\"batteryPercent\":" + String(deviceBatteryPercent_) + "," +
                      "\"sdFreeKb\":" + String(deviceSdFreeKb_) + "," +
                      "\"sdTotalKb\":" + String(deviceSdTotalKb_) +
                      "}";
  server_.send(200, "application/json", body);
}

void CompanionSyncManager::handleRoot() {
  server_.sendHeader("Cache-Control", "no-store, max-age=0");
  server_.send_P(200, "text/html", kWebCompanionHtml);
}

void CompanionSyncManager::handleBooksList() {
  sendCorsHeaders();
  String body = "{\"ok\":true,\"books\":[";
  bool first = true;

  const auto appendDirectory = [&](const char *directoryPath) {
    File dir = SD_MMC.open(directoryPath);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      return;
    }

    File entry = dir.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        const String name = displayNameForPath(String(entry.name()));
        const String path = String(directoryPath) + "/" + name;
        String lowered = name;
        lowered.toLowerCase();
        if (isSupportedBookName(lowered)) {
          const RsvpMetadata metadata = readRsvpMetadata(path);
          uint8_t progressPercent = 0;
          const bool hasProgress = progressPercentForPath(path, progressPercent);
          if (!first) {
            body += ",";
          }
          first = false;
          body += "{\"name\":\"" + jsonEscape(relativeLibraryName(path)) + "\",\"category\":\"" +
                  libraryCategoryForPath(path) + "\",\"title\":\"" +
                  jsonEscape(metadata.title) + "\",\"author\":\"" + jsonEscape(metadata.author) +
                  "\",\"bytes\":" +
                  String(static_cast<uint32_t>(entry.size()));
          if (hasProgress) {
            body += ",\"progressPercent\":" + String(progressPercent);
          }
          // Include chapters if available (only for .rsvp files)
          if (lowered.endsWith(".rsvp")) {
            const std::vector<RsvpChapter> chapters = readRsvpChapters(path);
            if (!chapters.empty()) {
              body += ",\"chapters\":[";
              for (size_t i = 0; i < chapters.size(); ++i) {
                if (i > 0) body += ",";
                body += "{\"title\":\"" + jsonEscape(chapters[i].title) +
                        "\",\"startWord\":" + String(static_cast<uint32_t>(chapters[i].startWord)) + "}";
              }
              body += "]";
            }
          }
          body += "}";
        }
      }
      entry.close();
      entry = dir.openNextFile();
    }

    dir.close();
  };

  appendDirectory(kBooksPath);
  appendDirectory(kBookFilesPath);
  appendDirectory(kArticleFilesPath);

  body += "]}";
  server_.send(200, "application/json", body);
}

void CompanionSyncManager::handleSettings() {
  sendCorsHeaders();
  if (server_.method() == HTTP_GET) {
    server_.send(200, "application/json", settingsJson());
    return;
  }

  const String body = server_.arg("plain");
  if (body.length() > kMaxSettingsPatchBytes) {
    server_.send(413, "application/json", "{\"ok\":false,\"error\":\"Settings payload too large\"}");
    return;
  }

  // Detect changes that require restart (typeface, font size)
  String prevTypeface;
  readJsonString(body, "typeface", prevTypeface);
  int prevFontSize = -1;
  readJsonInt(body, "fontSizeIndex", prevFontSize);

  String error;
  if (!applySettingsJson(body, error)) {
    server_.send(400, "application/json",
                 String("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}");
    return;
  }

  // Check if restart-worthy settings were changed
  bool restartRequired = false;
  String restartReason;
  if (!prevTypeface.isEmpty()) {
    restartRequired = true;
    restartReason = "Typeface change requires display reload";
  }

  String response = settingsJson();
  // Inject restartRequired before final closing brace if needed
  if (restartRequired) {
    // settingsJson ends with "}" — insert before it
    response = response.substring(0, response.length() - 1) +
               ",\"restartRequired\":true,\"restartReason\":\"" +
               jsonEscape(restartReason) + "\"}";
  }

  server_.send(200, "application/json", response);
  logLine("Settings saved" + (restartRequired ? String(" (restart required)") : String("")));
}

void CompanionSyncManager::handleWifi() {
  sendCorsHeaders();
  if (server_.method() == HTTP_GET) {
    server_.send(200, "application/json", wifiJson());
    return;
  }

  if (server_.method() == HTTP_DELETE) {
    preferences_.remove(kPrefWifiSsid);
    preferences_.remove(kPrefWifiPass);
    statusLine1_ = "Wi-Fi cleared";
    statusLine2_ = "";
    server_.send(200, "application/json", wifiJson());
    return;
  }

  String error;
  if (!applyWifiJson(server_.arg("plain"), error)) {
    server_.send(400, "application/json",
                 String("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}");
    return;
  }

  statusLine1_ = "Wi-Fi saved";
  statusLine2_ = preferences_.getString(kPrefWifiSsid, "");
  server_.send(200, "application/json", wifiJson());
}

void CompanionSyncManager::handleRssFeeds() {
  sendCorsHeaders();
  if (server_.method() == HTTP_GET) {
    server_.send(200, "application/json", rssFeedsJson());
    return;
  }

  String error;
  if (!writeRssFeedsJson(server_.arg("plain"), error)) {
    server_.send(400, "application/json",
                 String("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}");
    return;
  }

  statusLine1_ = "RSS feeds saved";
  statusLine2_ = kRssConfigPath;
  server_.send(200, "application/json", rssFeedsJson());
}

void CompanionSyncManager::handleBooks() {
  sendCorsHeaders();
  finishUpload(uploadError_.isEmpty());
  if (!uploadError_.isEmpty()) {
    server_.send(400, "application/json",
                 String("{\"ok\":false,\"error\":\"") + jsonEscape(uploadError_) + "\"}");
    uploadError_ = "";
    return;
  }

  server_.send(201, "application/json",
               String("{\"ok\":true,\"path\":\"") + jsonEscape(uploadFinalPath_) + "\"}");
  uploadFinalPath_ = "";
}

void CompanionSyncManager::handleBookDelete() {
  sendCorsHeaders();
  String requested = server_.arg("name");
  requested.trim();
  if (requested.isEmpty()) {
    server_.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing filename\"}");
    return;
  }

  String filename = requested;
  String path;
  const int separator = requested.indexOf('/');
  if (separator >= 0) {
    const String directory = requested.substring(0, separator);
    filename = sanitizeFilename(requested.substring(separator + 1));
    if (filename.isEmpty() || requested.indexOf("..") >= 0 ||
        (directory != "books" && directory != "articles")) {
      server_.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid library path\"}");
      return;
    }
    path = String(kBooksPath) + "/" + directory + "/" + filename;
  } else {
    filename = sanitizeFilename(requested);
    path = String(kBooksPath) + "/" + filename;
  }

  String lowered = filename;
  lowered.toLowerCase();
  if (!isSupportedBookName(lowered)) {
    server_.send(400, "application/json", "{\"ok\":false,\"error\":\"Unsupported file type\"}");
    return;
  }

  File file = SD_MMC.open(path);
  if ((!file || file.isDirectory()) && separator < 0) {
    if (file) {
      file.close();
    }
    path = String(kBookFilesPath) + "/" + filename;
    file = SD_MMC.open(path);
  }
  if ((!file || file.isDirectory()) && separator < 0) {
    if (file) {
      file.close();
    }
    path = String(kArticleFilesPath) + "/" + filename;
    file = SD_MMC.open(path);
  }
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    server_.send(404, "application/json", "{\"ok\":false,\"error\":\"Book not found\"}");
    return;
  }
  file.close();

  if (!SD_MMC.remove(path)) {
    server_.send(500, "application/json", "{\"ok\":false,\"error\":\"Delete failed\"}");
    return;
  }

  statusLine1_ = "Book deleted";
  statusLine2_ = filename;
  Serial.printf("[sync] deleted %s\n", path.c_str());
  logLine("Deleted: " + filename);
  server_.send(200, "application/json",
               String("{\"ok\":true,\"path\":\"") + jsonEscape(path) + "\"}");
}

void CompanionSyncManager::handleBookUpload() {
  HTTPUpload &upload = server_.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = sanitizeFilename(server_.arg("name"));
    if (filename.isEmpty()) {
      filename = sanitizeFilename(upload.filename);
    }
    if (filename.isEmpty()) {
      uploadError_ = "Missing filename";
      return;
    }

    String lowered = filename;
    lowered.toLowerCase();
    if (!isSupportedBookName(lowered)) {
      filename += ".rsvp";
    }

    String category = server_.arg("category");
    category.toLowerCase();
    const char *targetDirectory = category == "article" ? kArticleFilesPath : kBookFilesPath;

    SD_MMC.mkdir(kBooksPath);
    SD_MMC.mkdir(targetDirectory);
    uploadFinalPath_ = String(targetDirectory) + "/" + filename;
    uploadTmpPath_ = uploadFinalPath_ + ".tmp";
    SD_MMC.remove(uploadTmpPath_);
    uploadFile_ = SD_MMC.open(uploadTmpPath_, FILE_WRITE);
    if (!uploadFile_) {
      uploadError_ = "Could not create file";
      return;
    }
    uploadError_ = "";
    statusLine1_ = "Receiving book";
    statusLine2_ = filename;
    Serial.printf("[sync] upload start %s\n", uploadFinalPath_.c_str());
    logLine("Upload start: " + filename);
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!uploadError_.isEmpty() || !uploadFile_) {
      return;
    }
    const size_t written = uploadFile_.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      uploadError_ = "Write failed";
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("[sync] upload end bytes=%u error=%s\n", upload.totalSize,
                  uploadError_.c_str());
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    uploadError_ = "Upload aborted";
    finishUpload(false);
  }
}

// ─── OTA over WiFi ───────────────────────────────────────────────────────────
//
// PWA „Aktualizacje" wysyła tu pobrane .bin (osobno: aplikacja, nie scalona
// binarka — patrz `flower-firmware.bin` w GitHub Releases). Update.h
// (esp_ota_*) zapisuje w nieaktywnej partycji OTA, po sukcesie ESP.restart()
// przełącza na nią.

void CompanionSyncManager::handleOta() {
  sendCorsHeaders();
  // Wywołane gdy całe multipart body zostało już zjedzone przez
  // handleOtaUpload. Tu tylko zwracamy status i restartujemy.
  if (!otaError_.isEmpty()) {
    server_.send(500, "application/json",
                 String("{\"ok\":false,\"error\":\"") + jsonEscape(otaError_) + "\"}");
    statusLine1_ = "OTA failed";
    statusLine2_ = otaError_;
    return;
  }
  server_.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  statusLine1_ = "OTA done";
  statusLine2_ = "Restarting…";
  Serial.println("[sync] OTA success — restart in 500ms");
  // Dajemy WebServer chwilę żeby wysłał response zanim restart.
  delay(500);
  ESP.restart();
}

void CompanionSyncManager::handleOtaUpload() {
  HTTPUpload &upload = server_.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[sync] OTA upload start: %s\n", upload.filename.c_str());
    otaError_ = "";
    // UPDATE_SIZE_UNKNOWN — wgrywamy do całej dostępnej partycji OTA,
    // rozmiar weryfikujemy dopiero na końcu (Update.end()).
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      otaError_ = "Update.begin failed";
      Serial.printf("[sync] %s\n", otaError_.c_str());
    } else {
      statusLine1_ = "Receiving OTA";
      statusLine2_ = upload.filename;
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaError_.isEmpty()) return;
    const size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      otaError_ = "Flash write failed";
      Update.abort();
      Serial.printf("[sync] %s (written=%u expected=%u)\n", otaError_.c_str(),
                    static_cast<unsigned>(written),
                    static_cast<unsigned>(upload.currentSize));
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    Serial.printf("[sync] OTA upload end bytes=%u\n", upload.totalSize);
    if (!otaError_.isEmpty()) return;
    // `true` = setBootPartition po sukcesie. Po tym restart przełącza.
    if (!Update.end(true)) {
      otaError_ = String("Update.end failed: ") + Update.errorString();
      Serial.printf("[sync] %s\n", otaError_.c_str());
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaError_ = "OTA aborted";
    Serial.println("[sync] OTA aborted by client");
  }
}

void CompanionSyncManager::handleNotFound() {
  sendCorsHeaders();

  // Captive portal catch-all: Android/iOS/Windows robi cykliczne sprawdzenie
  // internetu na losowych URL-ach (connectivitycheck.gstatic.com, itp.).
  // DNS resolve'uje te domeny na 192.168.4.1 (nasz AP), więc requesty
  // trafiają tutaj. Jeśli URI NIE zaczyna się od /api/, to najprawdopodobniej
  // jest to connectivity check — odpowiadamy 204 żeby system myślał
  // że internet działa i nie wyświetlał wykrzyknika.
  const String uri = server_.uri();
  if (!uri.startsWith("/api/")) {
    logPortalHit("catch-all:" + uri, server_.client().remoteIP().toString());

    // Host-aware captive portal response: Android/HyperOS connectivity check
    // domains are DNS-redirected to us. We must respond exactly as the real
    // servers would so the OS marks the network as "connected".
    const String host = server_.hostHeader();

    // Android standard connectivity checks (HTTP, not HTTPS)
    if (host == "connectivitycheck.gstatic.com" ||
        host == "connectivitycheck.android.com" ||
        host == "clients3.google.com" ||
        host == "www.google.com") {
      // Google returns 204 No Content on /generate_204, blank.html, etc.
      if (uri == "/generate_204" || uri == "/gen_204") {
        server_.send(204, "text/plain", "");
      } else {
        // /blank.html and other paths → 200 with empty body
        server_.sendHeader("Content-Length", "0");
        server_.send(200, "text/html", "");
      }
      return;
    }

    // Xiaomi / HyperOS connectivity check
    if (host == "connect.rom.miui.com" ||
        host == "api.market.xiaomi.com") {
      server_.send(204, "text/plain", "");
      return;
    }

    // Default: 204 (safe catch-all for any unknown connectivity checks)
    server_.send(204, "text/plain", "");
    return;
  }

  server_.send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
}

// ─── CORS preflight ──────────────────────────────────────────────────────────

void CompanionSyncManager::handleOptions() {
  sendCorsHeaders();
  server_.sendHeader("Access-Control-Max-Age", "86400");
  server_.send(204);
}

void CompanionSyncManager::sendCorsHeaders() {
  server_.sendHeader("Access-Control-Allow-Origin", "*");
  server_.sendHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
  server_.sendHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

// ─── /api/capabilities ───────────────────────────────────────────────────────

void CompanionSyncManager::handleCapabilities() {
  sendCorsHeaders();
  String body;
  body.reserve(512);
  body += "{\"ok\":true,\"api\":1";
  body += ",\"firmwareVersion\":\"";
  body += RSVP_FIRMWARE_VERSION;
  body += "\"";
  body += ",\"features\":{";
  body += "\"settings\":true";
  body += ",\"books\":true";
  body += ",\"ota\":true";
  body += ",\"pluginsList\":true";
  body += ",\"pluginsRemove\":true";
  body += ",\"pluginsInstallPackage\":false";
  body += ",\"bluetoothTransfer\":";
#if FLOWER_BLE_ENABLED
  body += "true";
#else
  body += "false";
#endif
  body += ",\"rss\":";
  // RSS is now a dynamic plugin — always report as available capability
  body += "true";
  body += ",\"focusTimer\":";
  // Timer is now a dynamic plugin — always report as available capability
  body += "true";
  body += ",\"wifiTimeout\":true";
  body += "}}";
  server_.send(200, "application/json", body);
}

// ─── /api/plugins ────────────────────────────────────────────────────────────

void CompanionSyncManager::handlePlugins() {
  sendCorsHeaders();
  String body;
  body.reserve(512);
  body += "{\"ok\":true,\"plugins\":[";

  // Timer plugin — now a dynamic SD-card plugin
  body += "{\"id\":\"focus-timer\",\"name\":\"Focus Timer\"";
  body += ",\"installed\":true,\"builtin\":false";
  body += ",\"active\":true}";

  // RSS plugin — now a dynamic SD-card plugin
  body += ",{\"id\":\"rss\",\"name\":\"RSS Feeds\"";
  body += ",\"installed\":true,\"builtin\":false";
  body += ",\"active\":true}";

  body += "]}";
  server_.send(200, "application/json", body);
}

void CompanionSyncManager::handlePluginsDelete() {
  sendCorsHeaders();
  String pluginId = server_.arg("id");
  pluginId.trim();
  if (pluginId.isEmpty()) {
    // Try to parse from URL path — /api/plugins?id=xxx
    pluginId = server_.arg("plain");
  }

  if (pluginId.isEmpty()) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"Missing plugin id\"}");
    return;
  }

  // Plugins are firmware variants — can't be truly "removed" at runtime.
  // We inform the app that a firmware variant switch (OTA) is needed.
  if (pluginId == "focus-timer") {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"Focus Timer is built-in and cannot be removed\"}");
    return;
  }

  if (pluginId == "rss") {
    // RSS can be "removed" by flashing a variant without it
    server_.send(200, "application/json",
                 "{\"ok\":true,\"requiresOta\":true,\"message\":\"Flash firmware variant without RSS to remove\"}");
    return;
  }

  server_.send(404, "application/json",
               "{\"ok\":false,\"error\":\"Unknown plugin\"}");
}

// ─── /api/power/wifi-timeout ─────────────────────────────────────────────────

void CompanionSyncManager::handlePowerWifiTimeout() {
  sendCorsHeaders();
  const String body = server_.arg("plain");
  int timeoutSeconds = 0;
  if (readJsonInt(body, "timeout", timeoutSeconds)) {
    if (timeoutSeconds < 0) timeoutSeconds = 0;
    if (timeoutSeconds > 3600) timeoutSeconds = 3600;
    wifiTimeoutMs_ = static_cast<uint32_t>(timeoutSeconds) * 1000UL;
  }
  server_.send(200, "application/json",
               String("{\"ok\":true,\"timeoutSeconds\":") +
               String(wifiTimeoutMs_ / 1000UL) + "}");
}

// ─── GET /api/state — zbiorczy endpoint (1 request zamiast 6) ────────────────

void CompanionSyncManager::handleState() {
  sendCorsHeaders();
  const String mode = networkMode_ == NetworkMode::Station ? "station" : "access_point";

  String body;
  body.reserve(4096);
  body += "{\"ok\":true";

  // info
  body += ",\"info\":{";
  body += "\"name\":\"Flower\"";
  body += ",\"mode\":\"" + mode + "\"";
  body += ",\"baseUrl\":\"" + jsonEscape(baseUrl()) + "\"";
  body += ",\"networkSsid\":\"" + jsonEscape(networkSsid_) + "\"";
  body += ",\"pairingCode\":\"" + pairingCode_ + "\"";
  body += ",\"firmwareVersion\":\"" + jsonEscape(RSVP_FIRMWARE_VERSION) + "\"";
  body += ",\"api\":1";
  body += ",\"batteryPercent\":" + String(deviceBatteryPercent_);
  body += ",\"sdFreeKb\":" + String(deviceSdFreeKb_);
  body += ",\"sdTotalKb\":" + String(deviceSdTotalKb_);
  body += "}";

  // capabilities
  body += ",\"capabilities\":{";
  body += "\"settings\":true,\"books\":true,\"ota\":true";
  body += ",\"pluginsList\":true,\"pluginsRemove\":true";
  body += ",\"pluginsInstallPackage\":false";
#if FLOWER_BLE_ENABLED
  body += ",\"bluetoothTransfer\":true";
#else
  body += ",\"bluetoothTransfer\":false";
#endif
  body += ",\"rss\":true,\"focusTimer\":true,\"wifiTimeout\":true}";

  // settings (reuse existing builder, strip outer {})
  String settingsBody = settingsJson();
  body += ",\"settings\":" + settingsBody;

  // books — inline the books list
  body += ",\"books\":[";
  {
    bool first = true;
    const auto appendDir = [&](const char *directoryPath) {
      File dir = SD_MMC.open(directoryPath);
      if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
      File entry = dir.openNextFile();
      while (entry) {
        if (!entry.isDirectory()) {
          const String name = displayNameForPath(String(entry.name()));
          const String path = String(directoryPath) + "/" + name;
          String lowered = name; lowered.toLowerCase();
          if (isSupportedBookName(lowered)) {
            const RsvpMetadata metadata = readRsvpMetadata(path);
            uint8_t progressPercent = 0;
            const bool hasProgress = progressPercentForPath(path, progressPercent);
            if (!first) body += ",";
            first = false;
            body += "{\"name\":\"" + jsonEscape(relativeLibraryName(path)) +
                    "\",\"category\":\"" + libraryCategoryForPath(path) +
                    "\",\"title\":\"" + jsonEscape(metadata.title) +
                    "\",\"author\":\"" + jsonEscape(metadata.author) +
                    "\",\"bytes\":" + String(static_cast<uint32_t>(entry.size()));
            if (hasProgress) body += ",\"progressPercent\":" + String(progressPercent);
            if (lowered.endsWith(".rsvp")) {
              const std::vector<RsvpChapter> chapters = readRsvpChapters(path);
              if (!chapters.empty()) {
                body += ",\"chapters\":[";
                for (size_t i = 0; i < chapters.size(); ++i) {
                  if (i > 0) body += ",";
                  body += "{\"title\":\"" + jsonEscape(chapters[i].title) +
                          "\",\"startWord\":" + String(static_cast<uint32_t>(chapters[i].startWord)) + "}";
                }
                body += "]";
              }
            }
            body += "}";
          }
        }
        entry.close();
        entry = dir.openNextFile();
      }
      dir.close();
    };
    appendDir(kBooksPath);
    appendDir(kBookFilesPath);
    appendDir(kArticleFilesPath);
  }
  body += "]";

  // plugins
  body += ",\"plugins\":[";
  body += "{\"id\":\"focus-timer\",\"name\":\"Focus Timer\",\"installed\":true,\"active\":true}";
  body += ",{\"id\":\"rss\",\"name\":\"RSS Feeds\",\"installed\":true,\"active\":true}";
  body += "]";

  // rss feeds
  body += ",\"rss\":{\"feeds\":[";
  {
    File rssFile = SD_MMC.open(kRssConfigPath);
    bool first = true;
    if (rssFile && !rssFile.isDirectory()) {
      String line;
      while (rssFile.available()) {
        const char c = static_cast<char>(rssFile.read());
        if (c == '\n' || c == '\r') {
          line.trim();
          if (!line.isEmpty()) {
            if (!first) body += ",";
            first = false;
            body += "\"" + jsonEscape(line) + "\"";
          }
          line = "";
        } else {
          line += c;
        }
      }
      line.trim();
      if (!line.isEmpty()) {
        if (!first) body += ",";
        body += "\"" + jsonEscape(line) + "\"";
      }
      rssFile.close();
    }
  }
  body += "]}";

  // wifi
  body += ",\"wifi\":{";
  const String ssid = preferences_.getString(kPrefWifiSsid, "");
  body += "\"configured\":" + String(ssid.isEmpty() ? "false" : "true");
  body += ",\"ssid\":\"" + jsonEscape(ssid) + "\"}";

  body += "}";
  server_.send(200, "application/json", body);
}

// ─── GET /api/log/tail — ring buffer firmware logów ──────────────────────────

void CompanionSyncManager::logLine(const String &line) {
  logRing_[logRingHead_] = line;
  logRingHead_ = (logRingHead_ + 1) % kLogRingSize;
  if (logRingCount_ < kLogRingSize) ++logRingCount_;
}

void CompanionSyncManager::logPortalHit(const String &endpoint, const String &clientIp) {
  ++portalHitCount_;
  const uint32_t now = millis();
  if (now - portalLastLogMs_ >= kPortalLogIntervalMs) {
    portalLastLogMs_ = now;
    logLine("[portal] " + endpoint + " from " + clientIp + " (x" + String(portalHitCount_) + " in 5s)");
    portalHitCount_ = 0;
  }
}

void CompanionSyncManager::handleLogTail() {
  sendCorsHeaders();
  int n = 50;
  if (server_.hasArg("n")) {
    n = server_.arg("n").toInt();
    if (n < 1) n = 1;
    if (n > static_cast<int>(kLogRingSize)) n = static_cast<int>(kLogRingSize);
  }
  const size_t count = std::min(static_cast<size_t>(n), logRingCount_);
  const size_t start = (logRingHead_ + kLogRingSize - count) % kLogRingSize;

  String body;
  body.reserve(count * 80 + 64);
  body += "{\"ok\":true,\"total\":" + String(static_cast<uint32_t>(logRingCount_));
  body += ",\"lines\":[";
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) body += ",";
    body += "\"" + jsonEscape(logRing_[(start + i) % kLogRingSize]) + "\"";
  }
  body += "]}";
  server_.send(200, "application/json", body);
}

void CompanionSyncManager::handleLogClear() {
  sendCorsHeaders();
  logRingHead_ = 0;
  logRingCount_ = 0;
  for (size_t i = 0; i < kLogRingSize; ++i) logRing_[i] = "";
  server_.send(200, "application/json", "{\"ok\":true,\"cleared\":true}");
}

// ─── GET /api/lang/codes — mapowanie języków ────────────────────────────────

void CompanionSyncManager::handleLangCodes() {
  sendCorsHeaders();
  server_.send(200, "application/json",
    "{\"ok\":true,\"languages\":["
    "{\"code\":\"pl\",\"id\":0,\"name\":\"Polski\"},"
    "{\"code\":\"en\",\"id\":1,\"name\":\"English\"},"
    "{\"code\":\"de\",\"id\":2,\"name\":\"Deutsch\"},"
    "{\"code\":\"es\",\"id\":3,\"name\":\"Español\"},"
    "{\"code\":\"fr\",\"id\":4,\"name\":\"Français\"},"
    "{\"code\":\"it\",\"id\":5,\"name\":\"Italiano\"}"
    "]}");
}

// ─── GET/PUT /api/books/position — pozycja czytania ──────────────────────────

void CompanionSyncManager::handleBookPosition() {
  sendCorsHeaders();
  String bookName = server_.arg("name");
  bookName.trim();
  if (bookName.isEmpty()) {
    server_.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing name parameter\"}");
    return;
  }

  const String posKey = bookPositionKey(bookName);
  const String wcKey = bookWordCountKey(bookName);

  if (server_.method() == HTTP_GET) {
    const uint32_t wordIndex = preferences_.getULong(posKey.c_str(), 0);
    const uint32_t wordCount = preferences_.getULong(wcKey.c_str(), 0);
    String body = "{\"ok\":true,\"name\":\"" + jsonEscape(bookName) + "\"";
    body += ",\"wordIndex\":" + String(wordIndex);
    body += ",\"wordCount\":" + String(wordCount);
    if (wordCount > 0) {
      body += ",\"percent\":" + String(static_cast<uint8_t>(
        std::min(100UL, (static_cast<unsigned long>(wordIndex) * 100UL) / wordCount)));
    } else {
      body += ",\"percent\":0";
    }
    body += "}";
    server_.send(200, "application/json", body);
    return;
  }

  // PUT — update position
  const String reqBody = server_.arg("plain");
  int wordIndex = 0;
  int wordCount = 0;
  if (readJsonInt(reqBody, "wordIndex", wordIndex)) {
    if (wordIndex < 0) wordIndex = 0;
    preferences_.putULong(posKey.c_str(), static_cast<uint32_t>(wordIndex));
  }
  if (readJsonInt(reqBody, "wordCount", wordCount)) {
    if (wordCount < 0) wordCount = 0;
    preferences_.putULong(wcKey.c_str(), static_cast<uint32_t>(wordCount));
  }

  const uint32_t storedIndex = preferences_.getULong(posKey.c_str(), 0);
  const uint32_t storedCount = preferences_.getULong(wcKey.c_str(), 0);
  String body = "{\"ok\":true,\"name\":\"" + jsonEscape(bookName) + "\"";
  body += ",\"wordIndex\":" + String(storedIndex);
  body += ",\"wordCount\":" + String(storedCount);
  if (storedCount > 0) {
    body += ",\"percent\":" + String(static_cast<uint8_t>(
      std::min(100UL, (static_cast<unsigned long>(storedIndex) * 100UL) / storedCount)));
  } else {
    body += ",\"percent\":0";
  }
  body += "}";
  server_.send(200, "application/json", body);
}

// ─── Chapter reading for /api/books ──────────────────────────────────────────

std::vector<CompanionSyncManager::RsvpChapter> CompanionSyncManager::readRsvpChapters(
    const String &path) const {
  std::vector<RsvpChapter> chapters;
  String loweredPath = path;
  loweredPath.toLowerCase();
  if (!loweredPath.endsWith(".rsvp")) {
    return chapters;
  }

  File file = SD_MMC.open(path);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return chapters;
  }

  String line;
  size_t wordCount = 0;
  bool inBody = false;

  while (file.available()) {
    const char c = static_cast<char>(file.read());
    if (c == '\r') {
      continue;
    }

    if (c != '\n') {
      line += c;
      if (line.length() > 256) {
        // Skip overly long lines
        while (file.available()) {
          const char skip = static_cast<char>(file.read());
          if (skip == '\n') break;
        }
        if (inBody) {
          // Count approximate words in this long line
          wordCount += line.length() / 5;
        }
        line = "";
        continue;
      }
      continue;
    }

    // Process line
    String trimmed = line;
    trimmed.trim();

    if (trimmed.startsWith("@chapter")) {
      String title = trimmed.substring(8);
      title.trim();
      if (title.isEmpty()) title = "Chapter " + String(chapters.size() + 1);
      RsvpChapter ch;
      ch.title = title;
      ch.startWord = wordCount;
      chapters.push_back(ch);
      inBody = true;
    } else if (!trimmed.startsWith("@") && inBody) {
      // Count words in body lines
      bool inWord = false;
      for (size_t i = 0; i < trimmed.length(); ++i) {
        const char wc = trimmed[i];
        if (wc == ' ' || wc == '\t') {
          if (inWord) {
            ++wordCount;
            inWord = false;
          }
        } else {
          inWord = true;
        }
      }
      if (inWord) ++wordCount;
    } else if (!trimmed.isEmpty() && !trimmed.startsWith("@")) {
      inBody = true;
      // Count words
      bool inWord = false;
      for (size_t i = 0; i < trimmed.length(); ++i) {
        const char wc = trimmed[i];
        if (wc == ' ' || wc == '\t') {
          if (inWord) {
            ++wordCount;
            inWord = false;
          }
        } else {
          inWord = true;
        }
      }
      if (inWord) ++wordCount;
    }

    line = "";

    // Limit chapters to prevent excessive memory use
    if (chapters.size() >= 200) break;
  }

  file.close();
  return chapters;
}

String CompanionSyncManager::settingsJson() {
  static const char *const readerModeLabels[] = {"rsvp", "scroll"};
  static const char *const handednessLabels[] = {"right", "left"};
  static const char *const footerMetricLabels[] = {"percentage", "chapter_time", "book_time"};
  static const char *const batteryLabelLabels[] = {"percent", "time_remaining", "voltage"};
  static const char *const typefaceLabels[] = {"standard", "open_dyslexic", "atkinson"};
  static const char *const pauseModeLabels[] = {"sentence_end", "instant"};

  const uint16_t wpm =
      clampU16(preferences_.getUShort(kPrefWpm, kDefaultWpm), kMinWpm, kMaxWpm);
  const uint8_t readerMode =
      static_cast<uint8_t>(clampInt(preferences_.getUChar(kPrefReaderMode, 0), 0, kMaxReaderMode));
  const uint8_t pauseMode =
      static_cast<uint8_t>(clampInt(preferences_.getUChar(kPrefPauseMode, 1), 0, kMaxPauseMode));
  const uint16_t longDelay =
      clampU16(preferences_.getUShort(kPrefPacingLongMs, kDefaultPacingDelayMs), 0,
               kMaxPacingDelayMs);
  const uint16_t complexDelay =
      clampU16(preferences_.getUShort(kPrefPacingComplexMs, kDefaultPacingDelayMs), 0,
               kMaxPacingDelayMs);
  const uint16_t punctuationDelay =
      clampU16(preferences_.getUShort(kPrefPacingPunctuationMs, kDefaultPacingDelayMs), 0,
               kMaxPacingDelayMs);
  const uint8_t brightness = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefBrightness, kDefaultBrightness), 0, kMaxBrightness));
  const uint8_t handedness =
      static_cast<uint8_t>(clampInt(preferences_.getUChar(kPrefHandedness, 0), 0, kMaxHandedness));
  const uint8_t footerMetric = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefFooterMetricMode, 0), 0, kMaxFooterMetric));
  const uint8_t batteryLabel = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefBatteryLabelMode, 0), 0, kMaxBatteryLabel));
  const uint8_t language =
      static_cast<uint8_t>(clampInt(preferences_.getUChar(kPrefUiLanguage, 0), 0, kMaxUiLanguage));
  const uint8_t fontSize = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefReaderFontSize, 0), 0, kMaxReaderFontSize));
  const uint8_t typeface =
      static_cast<uint8_t>(clampInt(preferences_.getUChar(kPrefReaderTypeface, 0), 0,
                                    kMaxReaderTypeface));
  const int tracking =
      clampInt(preferences_.getChar(kPrefTypographyTracking, 0), kMinTypographyTracking,
               kMaxTypographyTracking);
  const uint8_t anchor = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefTypographyAnchor, kDefaultTypographyAnchor),
               kMinTypographyAnchor, kMaxTypographyAnchor));
  const uint8_t guideWidth = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefTypographyGuideWidth, kDefaultTypographyGuideWidth),
               kMinTypographyGuideWidth, kMaxTypographyGuideWidth));
  const uint8_t guideGap = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefTypographyGuideGap, kDefaultTypographyGuideGap),
               kMinTypographyGuideGap, kMaxTypographyGuideGap));

  // Read scroll settings from NVS
  const uint8_t scrollFontSize = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefScrollFontSize, kDefaultScrollFontSize), 0, kMaxScrollFontSize));
  const uint8_t scrollLineSpacing = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefScrollLineSpacing, kDefaultScrollLineSpacing), 0, kMaxScrollLineSpacing));
  const uint8_t scrollMargin = static_cast<uint8_t>(
      clampInt(preferences_.getUChar(kPrefScrollMargin, kDefaultScrollMargin), 0, kMaxScrollMargin));

  String body;
  body.reserve(1250);
  body += "{\"ok\":true,\"version\":1";
  body += ",\"reading\":{";
  body += "\"wpm\":" + String(wpm);
  body += ",\"readerMode\":\"";
  body += enumLabel(readerMode, readerModeLabels, 2);
  body += "\"";
  body += ",\"pauseMode\":\"";
  body += enumLabel(pauseMode, pauseModeLabels, 2);
  body += "\"";
  body += ",\"accurateTimeEstimate\":true";
  body += ",\"pacing\":{\"longWordMs\":" + String(longDelay) +
          ",\"complexWordMs\":" + String(complexDelay) +
          ",\"punctuationMs\":" + String(punctuationDelay) + "}";
  body += "}";
  body += ",\"display\":{";
  body += "\"brightnessIndex\":" + String(brightness);
  body += ",\"darkMode\":" + String(preferences_.getBool(kPrefDarkMode, false) ? "true" : "false");
  body += ",\"nightMode\":" +
          String(preferences_.getBool(kPrefNightMode, false) ? "true" : "false");
  body += ",\"handedness\":\"";
  body += enumLabel(handedness, handednessLabels, 2);
  body += "\"";
  body += ",\"footerMetric\":\"";
  body += enumLabel(footerMetric, footerMetricLabels, 3);
  body += "\"";
  body += ",\"batteryLabel\":\"";
  body += enumLabel(batteryLabel, batteryLabelLabels, 3);
  body += "\"";
  body += ",\"readingBattery\":" +
          String(preferences_.getBool(kPrefReaderBatteryVisible, false) ? "true" : "false");
  body += ",\"readingChapter\":" +
          String(preferences_.getBool(kPrefReaderChapterVisible, true) ? "true" : "false");
  body += ",\"readingProgress\":" +
          String(preferences_.getBool(kPrefReaderProgressVisible, false) ? "true" : "false");
  body += ",\"language\":" + String(language);
  body += ",\"phantomWords\":" +
          String(preferences_.getBool(kPrefPhantomWords, true) ? "true" : "false");
  body += ",\"fontSizeIndex\":" + String(fontSize);
  body += "}";
  body += ",\"typography\":{";
  body += "\"typeface\":\"";
  body += enumLabel(typeface, typefaceLabels, 3);
  body += "\"";
  body += ",\"focusHighlight\":" +
          String(preferences_.getBool(kPrefTypographyFocusHighlight, true) ? "true" : "false");
  body += ",\"tracking\":" + String(tracking);
  body += ",\"anchorPercent\":" + String(anchor);
  body += ",\"guideWidth\":" + String(guideWidth);
  body += ",\"guideGap\":" + String(guideGap);
  body += "}";
  body += ",\"limits\":{";
  body += "\"wpm\":{\"min\":" + String(kMinWpm) + ",\"max\":" + String(kMaxWpm) + "}";
  body += ",\"brightnessIndex\":{\"min\":0,\"max\":" + String(kMaxBrightness) + "}";
  body += ",\"pacingMs\":{\"min\":0,\"max\":" + String(kMaxPacingDelayMs) + "}";
  body += ",\"tracking\":{\"min\":" + String(kMinTypographyTracking) +
          ",\"max\":" + String(kMaxTypographyTracking) + "}";
  body += ",\"anchorPercent\":{\"min\":" + String(kMinTypographyAnchor) +
          ",\"max\":" + String(kMaxTypographyAnchor) + "}";
  body += ",\"guideWidth\":{\"min\":" + String(kMinTypographyGuideWidth) +
          ",\"max\":" + String(kMaxTypographyGuideWidth) + "}";
  body += ",\"guideGap\":{\"min\":" + String(kMinTypographyGuideGap) +
          ",\"max\":" + String(kMaxTypographyGuideGap) + "}";
  body += "}";
  body += ",\"scroll\":{";
  body += "\"scrollFontSize\":" + String(scrollFontSize);
  body += ",\"scrollLineSpacing\":" + String(scrollLineSpacing);
  body += ",\"scrollMargin\":" + String(scrollMargin);
  body += "}";
  // Developer mode — preferowana flaga ukrywająca advanced ustawienia na
  // urządzeniu i odsłaniająca je w aplikacji-towarzyszu. Dorzucamy na
  // końcu żeby nie ruszać istniejących sekcji.
  body += ",\"developer\":{\"devMode\":";
  body += preferences_.getBool("dev_mode", false) ? "true" : "false";
  body += "}}";
  return body;
}

bool CompanionSyncManager::applySettingsJson(const String &body, String &error) {
  if (body.isEmpty()) {
    error = "Missing settings JSON";
    return false;
  }

  static const char *const readerModeLabels[] = {"rsvp", "scroll"};
  static const char *const handednessLabels[] = {"right", "left"};
  static const char *const footerMetricLabels[] = {"percentage", "chapter_time", "book_time"};
  static const char *const batteryLabelLabels[] = {"percent", "time_remaining", "voltage"};
  static const char *const typefaceLabels[] = {"standard", "open_dyslexic", "atkinson"};
  static const char *const pauseModeLabels[] = {"sentence_end", "instant"};

  int intValue = 0;
  bool boolValue = false;
  String stringValue;

  if (readJsonInt(body, "wpm", intValue)) {
    if (intValue < kMinWpm || intValue > kMaxWpm) {
      error = "wpm must be between 10 and 1000";
      return false;
    }
    preferences_.putUShort(kPrefWpm, static_cast<uint16_t>(intValue));
  }
  if (readJsonString(body, "readerMode", stringValue)) {
    const int value = enumValue(stringValue, readerModeLabels, 2);
    if (value < 0) {
      error = "readerMode must be rsvp or scroll";
      return false;
    }
    preferences_.putUChar(kPrefReaderMode, static_cast<uint8_t>(value));
  }
  if (readJsonString(body, "pauseMode", stringValue)) {
    const int value = enumValue(stringValue, pauseModeLabels, 2);
    if (value < 0) {
      error = "pauseMode must be sentence_end or instant";
      return false;
    }
    preferences_.putUChar(kPrefPauseMode, static_cast<uint8_t>(value));
  }
  preferences_.putBool(kPrefAccurateTime, true);
  if (readJsonInt(body, "longWordMs", intValue)) {
    if (intValue < 0 || intValue > kMaxPacingDelayMs) {
      error = "longWordMs must be between 0 and 600";
      return false;
    }
    preferences_.putUShort(kPrefPacingLongMs, static_cast<uint16_t>(intValue));
  }
  if (readJsonInt(body, "complexWordMs", intValue)) {
    if (intValue < 0 || intValue > kMaxPacingDelayMs) {
      error = "complexWordMs must be between 0 and 600";
      return false;
    }
    preferences_.putUShort(kPrefPacingComplexMs, static_cast<uint16_t>(intValue));
  }
  if (readJsonInt(body, "punctuationMs", intValue)) {
    if (intValue < 0 || intValue > kMaxPacingDelayMs) {
      error = "punctuationMs must be between 0 and 600";
      return false;
    }
    preferences_.putUShort(kPrefPacingPunctuationMs, static_cast<uint16_t>(intValue));
  }
  if (readJsonInt(body, "brightnessIndex", intValue)) {
    if (intValue < 0 || intValue > kMaxBrightness) {
      error = "brightnessIndex must be between 0 and 4";
      return false;
    }
    preferences_.putUChar(kPrefBrightness, static_cast<uint8_t>(intValue));
  }
  if (readJsonBool(body, "darkMode", boolValue)) {
    preferences_.putBool(kPrefDarkMode, boolValue);
  }
  if (readJsonBool(body, "nightMode", boolValue)) {
    preferences_.putBool(kPrefNightMode, boolValue);
  }
  if (readJsonString(body, "handedness", stringValue)) {
    const int value = enumValue(stringValue, handednessLabels, 2);
    if (value < 0) {
      error = "handedness must be right or left";
      return false;
    }
    preferences_.putUChar(kPrefHandedness, static_cast<uint8_t>(value));
  }
  if (readJsonString(body, "footerMetric", stringValue)) {
    const int value = enumValue(stringValue, footerMetricLabels, 3);
    if (value < 0) {
      error = "footerMetric must be percentage, chapter_time, or book_time";
      return false;
    }
    preferences_.putUChar(kPrefFooterMetricMode, static_cast<uint8_t>(value));
  }
  if (readJsonString(body, "batteryLabel", stringValue)) {
    const int value = enumValue(stringValue, batteryLabelLabels, 3);
    if (value < 0) {
      error = "batteryLabel must be percent, time_remaining, or voltage";
      return false;
    }
    preferences_.putUChar(kPrefBatteryLabelMode, static_cast<uint8_t>(value));
  }
  if (readJsonBool(body, "readingBattery", boolValue)) {
    preferences_.putBool(kPrefReaderBatteryVisible, boolValue);
  }
  if (readJsonBool(body, "readingChapter", boolValue)) {
    preferences_.putBool(kPrefReaderChapterVisible, boolValue);
  }
  if (readJsonBool(body, "readingProgress", boolValue)) {
    preferences_.putBool(kPrefReaderProgressVisible, boolValue);
  }
  if (readJsonInt(body, "language", intValue)) {
    if (intValue < 0 || intValue > kMaxUiLanguage) {
      error = "language is out of range";
      return false;
    }
    preferences_.putUChar(kPrefUiLanguage, static_cast<uint8_t>(intValue));
  }
  if (readJsonBool(body, "phantomWords", boolValue)) {
    preferences_.putBool(kPrefPhantomWords, boolValue);
  }
  if (readJsonInt(body, "fontSizeIndex", intValue)) {
    if (intValue < 0 || intValue > kMaxReaderFontSize) {
      error = "fontSizeIndex must be between 0 and 2";
      return false;
    }
    preferences_.putUChar(kPrefReaderFontSize, static_cast<uint8_t>(intValue));
  }
  if (readJsonString(body, "typeface", stringValue)) {
    const int value = enumValue(stringValue, typefaceLabels, 3);
    if (value < 0) {
      error = "typeface must be standard, open_dyslexic, or atkinson";
      return false;
    }
    preferences_.putUChar(kPrefReaderTypeface, static_cast<uint8_t>(value));
  }
  if (readJsonBool(body, "focusHighlight", boolValue)) {
    preferences_.putBool(kPrefTypographyFocusHighlight, boolValue);
  }
  if (readJsonInt(body, "tracking", intValue)) {
    if (intValue < kMinTypographyTracking || intValue > kMaxTypographyTracking) {
      error = "tracking is out of range";
      return false;
    }
    preferences_.putChar(kPrefTypographyTracking, static_cast<int8_t>(intValue));
  }
  if (readJsonInt(body, "anchorPercent", intValue)) {
    if (intValue < kMinTypographyAnchor || intValue > kMaxTypographyAnchor) {
      error = "anchorPercent is out of range";
      return false;
    }
    preferences_.putUChar(kPrefTypographyAnchor, static_cast<uint8_t>(intValue));
  }
  if (readJsonInt(body, "guideWidth", intValue)) {
    if (intValue < kMinTypographyGuideWidth || intValue > kMaxTypographyGuideWidth) {
      error = "guideWidth is out of range";
      return false;
    }
    preferences_.putUChar(kPrefTypographyGuideWidth, static_cast<uint8_t>(intValue));
  }
  if (readJsonInt(body, "guideGap", intValue)) {
    if (intValue < kMinTypographyGuideGap || intValue > kMaxTypographyGuideGap) {
      error = "guideGap is out of range";
      return false;
    }
    preferences_.putUChar(kPrefTypographyGuideGap, static_cast<uint8_t>(intValue));
  }

  if (readJsonInt(body, "scrollFontSize", intValue)) {
    if (intValue < 0 || intValue > kMaxScrollFontSize) {
      error = "scrollFontSize must be between 0 and 8";
      return false;
    }
    preferences_.putUChar(kPrefScrollFontSize, static_cast<uint8_t>(intValue));
  }
  if (readJsonInt(body, "scrollLineSpacing", intValue)) {
    if (intValue < 0 || intValue > kMaxScrollLineSpacing) {
      error = "scrollLineSpacing must be between 0 and 2";
      return false;
    }
    preferences_.putUChar(kPrefScrollLineSpacing, static_cast<uint8_t>(intValue));
  }
  if (readJsonInt(body, "scrollMargin", intValue)) {
    if (intValue < 0 || intValue > kMaxScrollMargin) {
      error = "scrollMargin must be between 0 and 2";
      return false;
    }
    preferences_.putUChar(kPrefScrollMargin, static_cast<uint8_t>(intValue));
  }

  // Developer mode toggle — odbierane przez PUT/PATCH z PWA.
  // Klucz preferencji jest dzielony z App.cpp (`kPrefDevMode = "dev_mode"`).
  if (readJsonBool(body, "devMode", boolValue)) {
    preferences_.putBool("dev_mode", boolValue);
  }

  return true;
}

String CompanionSyncManager::wifiJson() {
  const String ssid = preferences_.getString(kPrefWifiSsid, "");
  return String("{\"ok\":true,\"configured\":") + (ssid.isEmpty() ? "false" : "true") +
         ",\"ssid\":\"" + jsonEscape(ssid) + "\",\"passwordSet\":" +
         (preferences_.getString(kPrefWifiPass, "").isEmpty() ? "false" : "true") + "}";
}

bool CompanionSyncManager::applyWifiJson(const String &body, String &error) {
  if (body.length() > 512) {
    error = "Wi-Fi payload too large";
    return false;
  }

  String ssid;
  if (!readJsonString(body, "ssid", ssid)) {
    error = "Missing Wi-Fi SSID";
    return false;
  }
  ssid.trim();
  if (ssid.isEmpty()) {
    error = "Wi-Fi SSID is required";
    return false;
  }
  if (ssid.length() > 32) {
    error = "Wi-Fi SSID is too long";
    return false;
  }

  String password;
  readJsonString(body, "password", password);
  if (password.length() > 64) {
    error = "Wi-Fi password is too long";
    return false;
  }

  preferences_.putString(kPrefWifiSsid, ssid);
  preferences_.putString(kPrefWifiPass, password);
  return true;
}

String CompanionSyncManager::rssFeedsJson() {
  String body = "{\"ok\":true,\"feeds\":[";
  File file = SD_MMC.open(kRssConfigPath);
  bool first = true;
  if (file && !file.isDirectory()) {
    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.isEmpty() || line.startsWith("#")) {
        continue;
      }
      if (line.startsWith("feed=")) {
        line = line.substring(5);
        line.trim();
      }
      if (!isHttpUrl(line)) {
        continue;
      }
      if (!first) {
        body += ",";
      }
      first = false;
      body += "\"" + jsonEscape(line) + "\"";
    }
  }
  if (file) {
    file.close();
  }
  body += "]}";
  return body;
}

bool CompanionSyncManager::writeRssFeedsJson(const String &body, String &error) {
  if (body.length() > kMaxRssFeedsPatchBytes) {
    error = "RSS feed payload too large";
    return false;
  }

  int colonIndex = -1;
  if (!findJsonKey(body, "feeds", colonIndex)) {
    error = "Missing feeds array";
    return false;
  }
  int index = skipJsonWhitespace(body, colonIndex + 1);
  if (index >= static_cast<int>(body.length()) || body[index] != '[') {
    error = "feeds must be an array";
    return false;
  }
  ++index;

  std::vector<String> feeds;
  feeds.reserve(8);
  while (true) {
    index = skipJsonWhitespace(body, index);
    if (index < static_cast<int>(body.length()) && body[index] == ']') {
      break;
    }

    String feed;
    if (!nextJsonArrayString(body, index, feed)) {
      error = "Invalid feeds array";
      return false;
    }
    feed.trim();
    if (feed.isEmpty()) {
      continue;
    }
    if (!isHttpUrl(feed)) {
      error = "Feeds must start with http:// or https://";
      return false;
    }
    if (feeds.size() >= kMaxRssFeeds) {
      error = "Too many RSS feeds";
      return false;
    }
    bool duplicate = false;
    for (const String &existing : feeds) {
      if (existing == feed) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      feeds.push_back(feed);
    }
  }

  SD_MMC.mkdir(kConfigPath);
  const String tmpPath = String(kRssConfigPath) + ".tmp";
  SD_MMC.remove(tmpPath);
  File file = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!file) {
    error = "Could not write RSS config";
    return false;
  }
  file.println("# Flower RSS feeds");
  for (const String &feed : feeds) {
    file.print("feed=");
    file.println(feed);
  }
  file.close();

  SD_MMC.remove(kRssConfigPath);
  if (!SD_MMC.rename(tmpPath, kRssConfigPath)) {
    SD_MMC.remove(tmpPath);
    error = "Could not save RSS config";
    return false;
  }
  return true;
}

String CompanionSyncManager::deviceSuffix() const {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%06X", static_cast<unsigned int>(mac & 0xFFFFFF));
  return String(suffix);
}

String CompanionSyncManager::jsonEscape(const String &value) const {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (c < 0x20) {
          char code[7];
          std::snprintf(code, sizeof(code), "\\u%04x", c);
          escaped += code;
        } else {
          escaped += static_cast<char>(c);
        }
        break;
    }
  }
  return escaped;
}

String CompanionSyncManager::sanitizeFilename(const String &name) const {
  String sanitized;
  sanitized.reserve(name.length());
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    sanitized += isSafeFilenameChar(c) ? c : '-';
  }
  sanitized.trim();
  while (sanitized.startsWith(".")) {
    sanitized.remove(0, 1);
  }
  return sanitized;
}

CompanionSyncManager::RsvpMetadata CompanionSyncManager::readRsvpMetadata(
    const String &path) const {
  RsvpMetadata metadata;
  String loweredPath = path;
  loweredPath.toLowerCase();
  if (!loweredPath.endsWith(".rsvp")) {
    return metadata;
  }

  File file = SD_MMC.open(path);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return metadata;
  }

  String line;
  bool pastDirectives = false;
  while (file.available()) {
    const char c = static_cast<char>(file.read());
    if (c == '\r') {
      continue;
    }

    if (c != '\n') {
      line += c;
      if (line.length() > kMaxMetadataLineChars) {
        pastDirectives = true;
        line = "";
        break;
      }
      continue;
    }

    if (metadata.title.isEmpty()) {
      metadata.title = rsvpMetadataValueFromLine(line, "@title", pastDirectives);
    }
    if (metadata.author.isEmpty() && !pastDirectives) {
      metadata.author = rsvpMetadataValueFromLine(line, "@author", pastDirectives);
    }
    if (!metadata.title.isEmpty() && !metadata.author.isEmpty()) {
      break;
    }

    if (pastDirectives) {
      break;
    }
    line = "";
  }

  if (!line.isEmpty() && !pastDirectives) {
    if (metadata.title.isEmpty()) {
      metadata.title = rsvpMetadataValueFromLine(line, "@title", pastDirectives);
    }
    if (metadata.author.isEmpty() && !pastDirectives) {
      metadata.author = rsvpMetadataValueFromLine(line, "@author", pastDirectives);
    }
  }

  file.close();
  return metadata;
}

bool CompanionSyncManager::progressPercentForPath(const String &path, uint8_t &percent) {
  const String positionKey = bookPositionKey(path);
  const String countKey = bookWordCountKey(path);
  if (!preferences_.isKey(positionKey.c_str()) || !preferences_.isKey(countKey.c_str())) {
    return false;
  }

  const size_t wordCount = preferences_.getUInt(countKey.c_str(), 0);
  if (wordCount <= 1) {
    return false;
  }

  size_t wordIndex = preferences_.getUInt(positionKey.c_str(), 0);
  wordIndex = std::min(wordIndex, wordCount - 1);
  const size_t progress = (wordIndex * static_cast<size_t>(100)) / (wordCount - 1);
  percent = static_cast<uint8_t>(std::min(static_cast<size_t>(100), progress));
  return true;
}

String CompanionSyncManager::bookPositionKey(const String &bookPath) const {
  char key[10];
  std::snprintf(key, sizeof(key), "p%08lx", static_cast<unsigned long>(hashBookPath(bookPath)));
  return String(key);
}

String CompanionSyncManager::bookWordCountKey(const String &bookPath) const {
  char key[10];
  std::snprintf(key, sizeof(key), "c%08lx", static_cast<unsigned long>(hashBookPath(bookPath)));
  return String(key);
}

uint32_t CompanionSyncManager::hashBookPath(const String &path) const {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < path.length(); ++i) {
    hash ^= static_cast<uint8_t>(path[i]);
    hash *= 16777619UL;
  }
  return hash;
}

void CompanionSyncManager::finishUpload(bool success) {
  if (uploadFile_) {
    uploadFile_.close();
  }

  if (uploadTmpPath_.isEmpty()) {
    return;
  }

  if (success && uploadError_.isEmpty()) {
    SD_MMC.remove(uploadFinalPath_);
    if (!SD_MMC.rename(uploadTmpPath_, uploadFinalPath_)) {
      uploadError_ = "Rename failed";
      SD_MMC.remove(uploadTmpPath_);
    } else {
      statusLine1_ = "Book received";
      statusLine2_ = uploadFinalPath_;
      Serial.printf("[sync] upload ready %s\n", uploadFinalPath_.c_str());
    }
  } else {
    SD_MMC.remove(uploadTmpPath_);
  }

  uploadTmpPath_ = "";
}
