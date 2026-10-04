#include "display/SdFontLoader.h"

#include <SD_MMC.h>
#include <cstring>
#include <esp_heap_caps.h>

namespace {

constexpr char kFntMagic[4] = {'F', 'N', 'T', '1'};
constexpr uint16_t kFntVersion = 1;

// Mirrors _FNT_HEADER in tools/generate_embedded_font.py: 16 bytes,
// little-endian, no compiler padding relied upon — read as raw bytes below.
struct FntHeader {
  char magic[4];
  uint16_t version;
  uint8_t firstChar;
  uint8_t lastChar;
  uint8_t height;
  uint8_t reserved;
  uint16_t glyphCount;
  uint32_t bitmapLength;
};

static_assert(sizeof(EmbeddedFontGlyph) == 8,
              "SdFontLoader points the glyph table straight into the file "
              "buffer — the .fnt glyph record layout only matches this "
              "struct's in-memory layout if it is exactly 8 bytes.");

bool readHeader(File &file, FntHeader &header) {
  uint8_t raw[16];
  if (file.read(raw, sizeof(raw)) != sizeof(raw)) {
    return false;
  }
  memcpy(header.magic, raw, 4);
  header.version = static_cast<uint16_t>(raw[4] | (raw[5] << 8));
  header.firstChar = raw[6];
  header.lastChar = raw[7];
  header.height = raw[8];
  header.reserved = raw[9];
  header.glyphCount = static_cast<uint16_t>(raw[10] | (raw[11] << 8));
  header.bitmapLength = static_cast<uint32_t>(raw[12]) | (static_cast<uint32_t>(raw[13]) << 8) |
                         (static_cast<uint32_t>(raw[14]) << 16) |
                         (static_cast<uint32_t>(raw[15]) << 24);
  return true;
}

void *allocateFontBuffer(size_t bytes) {
  void *buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buffer == nullptr) {
    buffer = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  return buffer;
}

}  // namespace

SdFontLoader::~SdFontLoader() { unload(); }

void SdFontLoader::unload() {
  if (buffer_ != nullptr) {
    heap_caps_free(buffer_);
    buffer_ = nullptr;
  }
  variant_ = {};
  status_ = Status::NotLoaded;
}

bool SdFontLoader::load(const String &path) {
  unload();

  File file = SD_MMC.open(path);
  if (!file && SD_MMC.exists(path)) {
    // There but not opened: every file handle busy for a moment (a download
    // writing, the app sending a book). One more try before giving up.
    delay(30);
    file = SD_MMC.open(path);
  }
  if (!file || file.isDirectory()) {
    Serial.printf("[sdfont] not found: %s\n", path.c_str());
    status_ = Status::FileNotFound;
    return false;
  }

  FntHeader header;
  const bool headerOk = readHeader(file, header);
  const size_t glyphTableBytes = static_cast<size_t>(header.glyphCount) * sizeof(EmbeddedFontGlyph);
  const size_t dataBytes = glyphTableBytes + header.bitmapLength;
  const uint32_t expectedFileSize = 16U + static_cast<uint32_t>(dataBytes);

  const bool formatOk = headerOk && memcmp(header.magic, kFntMagic, 4) == 0 &&
                        header.version == kFntVersion &&
                        header.glyphCount ==
                            static_cast<uint16_t>(header.lastChar - header.firstChar + 1) &&
                        file.size() == expectedFileSize;
  if (!formatOk) {
    Serial.printf("[sdfont] invalid .fnt: %s\n", path.c_str());
    file.close();
    status_ = Status::InvalidFormat;
    return false;
  }

  uint8_t *buffer = static_cast<uint8_t *>(allocateFontBuffer(dataBytes));
  if (buffer == nullptr) {
    Serial.printf("[sdfont] out of memory (%u bytes): %s\n", static_cast<unsigned int>(dataBytes),
                  path.c_str());
    file.close();
    status_ = Status::OutOfMemory;
    return false;
  }

  const size_t bytesRead = file.read(buffer, dataBytes);
  file.close();
  if (bytesRead != dataBytes) {
    Serial.printf("[sdfont] short read: %s\n", path.c_str());
    heap_caps_free(buffer);
    status_ = Status::InvalidFormat;
    return false;
  }

  buffer_ = buffer;
  variant_.glyphs = reinterpret_cast<const EmbeddedFontGlyph *>(buffer_);
  variant_.bitmaps = buffer_ + glyphTableBytes;
  variant_.firstChar = header.firstChar;
  variant_.lastChar = header.lastChar;
  variant_.height = header.height;
  status_ = Status::Loaded;
  Serial.printf("[sdfont] loaded %s (%u bytes, %u glyphs)\n", path.c_str(),
                static_cast<unsigned int>(dataBytes),
                static_cast<unsigned int>(header.glyphCount));
  return true;
}
