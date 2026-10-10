// Card set-up over the USB cable, driven by the web flasher right after it
// installs the firmware (src/flasher in the repo root): format the card and
// write the font pack, so a new reader's wizard has nothing left to fetch
// but the starter books. main.cpp hands every console line starting with
// "prov" here. Answers start with "@prov" so the page can pick them out of
// the log lines around them.
//
//   prov hello                    -> @prov hello <version> card=<0|1> fonts=<0|1>
//   prov format                   -> @prov format ok|fail
//   prov put <path> <size> <crc>  -> @prov ready, then <size> bytes in chunks
//                                    of kProvisionChunk, each answered by
//                                    "@prov k <bytes so far>"; at the end
//                                    @prov put ok|fail <reason>
//   prov done                     -> @prov done fonts=<0|1>
// Included at the end of App.cpp.

namespace {

// The page sends at most this much before waiting for "@prov k": the USB
// serial receive queue (main.cpp sets it larger) never overflows while the
// card is being written.
constexpr size_t kProvisionChunk = 8192;
constexpr uint32_t kProvisionChunkTimeoutMs = 5000;

uint32_t provisionCrc32(uint32_t crc, const uint8_t *data, size_t length) {
  crc = ~crc;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

// Every folder on the way to `path` (the card may be freshly formatted).
void provisionMakeParents(const String &path) {
  int slash = path.indexOf('/', 1);
  while (slash > 0) {
    const String dir = path.substring(0, slash);
    if (!SD_MMC.exists(dir)) {
      SD_MMC.mkdir(dir);
    }
    slash = path.indexOf('/', slash + 1);
  }
}

}  // namespace

void App::renderProvisionStatus(const String &line1, const String &line2) {
  display_.setBrightnessPercent(currentBrightnessPercent());
  display_.renderStatus(tr4(TrKey4::ProvTitle), line1, line2);
}

void App::handleProvisionCommand(const String &line) {
  const String command = line.substring(4);  // after "prov"
  String args = command;
  args.trim();
  PowerGuard::mainLoopAlive(millis());
  lastActivityMs_ = millis();

  if (args == "hello") {
    if (!storageReady_) {
      storageReady_ = storage_.begin();
    }
    const bool fonts = storageReady_ && refreshFontPackComplete();
    Serial.printf("\n@prov hello %s card=%d fonts=%d\n", RSVP_FIRMWARE_VERSION, storageReady_ ? 1 : 0,
                  fonts ? 1 : 0);
    provisionActive_ = true;
    renderProvisionStatus(tr4(TrKey4::ProvConnected), "");
    return;
  }

  if (args == "format") {
    if (fontDownloadInProgress_ || bookDownloadInProgress_) {
      Serial.println("\n@prov format fail busy");
      return;
    }
    renderProvisionStatus(tr4(TrKey4::WizSdFormatting), tr4(TrKey4::WizSdFormattingSub));
    activeBookStore_.close();
    storageReady_ = storage_.formatCard();
    Serial.printf("\n@prov format %s\n", storageReady_ ? "ok" : "fail");
    return;
  }

  if (args.startsWith("put ")) {
    // put <path> <size> <crc32 hex>
    String rest = args.substring(4);
    rest.trim();
    const int firstSpace = rest.indexOf(' ');
    const int secondSpace = firstSpace > 0 ? rest.indexOf(' ', firstSpace + 1) : -1;
    if (firstSpace <= 0 || secondSpace <= 0) {
      Serial.println("\n@prov put fail syntax");
      return;
    }
    const String path = rest.substring(0, firstSpace);
    const uint32_t size = static_cast<uint32_t>(rest.substring(firstSpace + 1, secondSpace).toInt());
    const uint32_t expectedCrc = static_cast<uint32_t>(strtoul(rest.substring(secondSpace + 1).c_str(), nullptr, 16));
    if (!storageReady_ || !path.startsWith("/") || path.indexOf("..") >= 0) {
      Serial.println("\n@prov put fail card");
      return;
    }
    ++provisionFilesDone_;
    renderProvisionStatus(String(tr4(TrKey4::ProvWritingFonts)) + "  " + String(provisionFilesDone_),
                          path.substring(path.lastIndexOf('/') + 1));
    provisionMakeParents(path);
    const String partPath = path + ".part";
    SD_MMC.remove(partPath);
    File out = SD_MMC.open(partPath, FILE_WRITE);
    uint8_t *buffer = static_cast<uint8_t *>(malloc(kProvisionChunk));
    if (!out || buffer == nullptr) {
      if (out) out.close();
      free(buffer);
      Serial.println("\n@prov put fail open");
      return;
    }
    Serial.println("\n@prov ready");
    uint32_t received = 0;
    uint32_t crc = 0;
    bool ok = true;
    const char *reason = "";
    while (received < size) {
      const size_t want = std::min<size_t>(kProvisionChunk, size - received);
      size_t got = 0;
      uint32_t lastByteMs = millis();
      while (got < want) {
        const int available = Serial.available();
        if (available > 0) {
          got += Serial.readBytes(buffer + got, std::min<size_t>(want - got, static_cast<size_t>(available)));
          lastByteMs = millis();
        } else if (millis() - lastByteMs > kProvisionChunkTimeoutMs) {
          break;
        } else {
          delay(1);
        }
      }
      if (got < want) {
        ok = false;
        reason = "timeout";
        break;
      }
      if (out.write(buffer, got) != got) {
        ok = false;
        reason = "write";
        break;
      }
      crc = provisionCrc32(crc, buffer, got);
      received += got;
      PowerGuard::mainLoopAlive(millis());
      Serial.printf("\n@prov k %lu\n", static_cast<unsigned long>(received));
    }
    out.close();
    free(buffer);
    if (ok && crc != expectedCrc) {
      ok = false;
      reason = "crc";
    }
    if (ok) {
      SD_MMC.remove(path);
      ok = SD_MMC.rename(partPath, path);
      reason = ok ? "" : "rename";
    }
    if (!ok) {
      SD_MMC.remove(partPath);
    }
    Serial.printf("\n@prov put %s %s\n", ok ? "ok" : "fail", reason);
    return;
  }

  if (args == "done") {
    const bool fonts = storageReady_ && refreshFontPackComplete();
    provisionActive_ = false;
    provisionFilesDone_ = 0;
    Serial.printf("\n@prov done fonts=%d\n", fonts ? 1 : 0);
    if (storageReady_) {
      storage_.refreshBooks();
      applyTypographySettings(millis(), false);
    }
    renderProvisionStatus(tr4(TrKey4::ProvDone), tr4(TrKey4::ProvDoneSub));
    delay(1500);
    if (state_ == AppState::Menu) {
      renderMenu();
    } else if (state_ == AppState::Paused || state_ == AppState::Playing) {
      setState(AppState::Paused, millis());
      renderActiveReader(millis());
    }
    return;
  }

  Serial.printf("\n@prov unknown %s\n", args.c_str());
}
