#include "storage/BookExtras.h"

#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <algorithm>

namespace BookExtras {
namespace {

constexpr const char *kConfigDir = "/config";
constexpr const char *kLiveRoot = "/config/books";
constexpr const char *kArchiveRoot = "/config/archive";
constexpr uint32_t kPictureMagic = 0x31494246UL;  // "FBI1"
constexpr const char *kChaptersMagic = "FCH1";
constexpr const char *kNamesMagic = "FNM1";
constexpr unsigned int kMaxNameBytes = 120;
constexpr size_t kPictureHeaderBytes = 8;
constexpr size_t kCacheSlots = 32;

// Path without the extension, lower case: "/books/books/Pan Tadeusz.epub"
// and its converted "/books/books/Pan Tadeusz.rsvp" are the same book.
String keySource(const String &bookPath) {
  String key = bookPath;
  const int slash = key.lastIndexOf('/');
  const int dot = key.lastIndexOf('.');
  if (dot > slash) {
    key.remove(dot);
  }
  key.toLowerCase();
  return key;
}

String keyFor(const String &bookPath) {
  const String source = keySource(bookPath);
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < source.length(); ++i) {
    hash = (hash ^ static_cast<uint8_t>(source[i])) * 16777619UL;
  }
  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%08lx", static_cast<unsigned long>(hash));
  return String(buffer);
}

const char *pictureName(Picture kind) { return kind == Picture::Cover ? "cover.img" : "spine.img"; }

void expectedSize(Picture kind, uint16_t &width, uint16_t &height) {
  width = kind == Picture::Cover ? kCoverWidth : kSpineWidth;
  height = kind == Picture::Cover ? kCoverHeight : kSpineHeight;
}

bool ensureFolder(const String &bookPath) {
  SD_MMC.mkdir(kConfigDir);
  SD_MMC.mkdir(kLiveRoot);
  const String folder = folderFor(bookPath);
  if (!SD_MMC.exists(folder)) {
    if (!SD_MMC.mkdir(folder)) {
      return false;
    }
  }
  const String note = folder + "/path.txt";
  if (!SD_MMC.exists(note)) {
    File file = SD_MMC.open(note, FILE_WRITE);
    if (file) {
      file.print(bookPath);
      file.close();
    }
  }
  return true;
}

void removeFolder(const String &folder) {
  File dir = SD_MMC.open(folder);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return;
  }
  std::vector<String> files;
  File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) {
      name = name.substring(slash + 1);
    }
    files.push_back(folder + "/" + name);
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  for (const String &file : files) {
    SD_MMC.remove(file);
  }
  SD_MMC.rmdir(folder);
}

// Folder is empty apart from path.txt: nothing worth keeping.
bool folderIsEmpty(const String &folder) {
  File dir = SD_MMC.open(folder);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return true;
  }
  bool empty = true;
  File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    entry.close();
    if (!name.endsWith("path.txt")) {
      empty = false;
      break;
    }
    entry = dir.openNextFile();
  }
  dir.close();
  return empty;
}

struct CacheSlot {
  String key;
  Picture kind = Picture::Cover;
  bool present = false;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t *pixels = nullptr;
  uint32_t lastUse = 0;
};

CacheSlot gCache[kCacheSlots];
uint32_t gCacheClock = 0;

void releaseSlot(CacheSlot &slot) {
  if (slot.pixels != nullptr) {
    heap_caps_free(slot.pixels);
  }
  slot = CacheSlot();
}

bool readPictureHeader(File &file, uint16_t &width, uint16_t &height) {
  uint8_t header[kPictureHeaderBytes];
  if (file.read(header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  const uint32_t magic = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                         (static_cast<uint32_t>(header[2]) << 16) | (static_cast<uint32_t>(header[3]) << 24);
  width = static_cast<uint16_t>(header[4] | (header[5] << 8));
  height = static_cast<uint16_t>(header[6] | (header[7] << 8));
  return magic == kPictureMagic && width > 0 && height > 0 && width <= 256 && height <= 256;
}

void invalidate(const String &bookPath) {
  const String key = keyFor(bookPath);
  for (CacheSlot &slot : gCache) {
    if (slot.key == key) {
      releaseSlot(slot);
    }
  }
}

}  // namespace

String folderFor(const String &bookPath) { return String(kLiveRoot) + "/" + keyFor(bookPath); }

String archiveFolderFor(const String &bookPath) { return String(kArchiveRoot) + "/" + keyFor(bookPath); }

String picturePath(const String &bookPath, Picture kind) { return folderFor(bookPath) + "/" + pictureName(kind); }

bool installPicture(const String &bookPath, Picture kind, const String &uploadedPath, String &error) {
  File file = SD_MMC.open(uploadedPath, FILE_READ);
  if (!file) {
    error = "Upload missing";
    return false;
  }
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t wantWidth = 0;
  uint16_t wantHeight = 0;
  expectedSize(kind, wantWidth, wantHeight);
  const bool headerOk = readPictureHeader(file, width, height);
  const size_t bytes = file.size();
  file.close();
  if (!headerOk || width != wantWidth || height != wantHeight ||
      bytes != kPictureHeaderBytes + static_cast<size_t>(width) * height * 2) {
    SD_MMC.remove(uploadedPath);
    error = "Picture must be " + String(wantWidth) + "x" + String(wantHeight) + " FBI1";
    return false;
  }
  if (!ensureFolder(bookPath)) {
    SD_MMC.remove(uploadedPath);
    error = "Cannot create book folder";
    return false;
  }
  const String target = picturePath(bookPath, kind);
  SD_MMC.remove(target);
  if (!SD_MMC.rename(uploadedPath, target)) {
    SD_MMC.remove(uploadedPath);
    error = "Cannot store picture";
    return false;
  }
  invalidate(bookPath);
  return true;
}

bool removePicture(const String &bookPath, Picture kind) {
  const String target = picturePath(bookPath, kind);
  const bool existed = SD_MMC.exists(target);
  if (existed) {
    SD_MMC.remove(target);
  }
  invalidate(bookPath);
  if (folderIsEmpty(folderFor(bookPath))) {
    removeFolder(folderFor(bookPath));
  }
  return existed;
}

bool hasPicture(const String &bookPath, Picture kind) { return SD_MMC.exists(picturePath(bookPath, kind)); }

NanoImage picture(const String &bookPath, Picture kind) {
  NanoImage image;
  if (bookPath.isEmpty()) {
    return image;
  }
  const String key = keyFor(bookPath);
  ++gCacheClock;
  for (CacheSlot &slot : gCache) {
    if (slot.key == key && slot.kind == kind && !slot.key.isEmpty()) {
      slot.lastUse = gCacheClock;
      if (slot.present) {
        image.width = slot.width;
        image.height = slot.height;
        image.pixels = slot.pixels;
      }
      return image;
    }
  }

  // Not cached: take the free or least recently used slot.
  CacheSlot *target = &gCache[0];
  for (CacheSlot &slot : gCache) {
    if (slot.key.isEmpty()) {
      target = &slot;
      break;
    }
    if (slot.lastUse < target->lastUse) {
      target = &slot;
    }
  }
  releaseSlot(*target);
  target->key = key;
  target->kind = kind;
  target->lastUse = gCacheClock;

  File file = SD_MMC.open(picturePath(bookPath, kind), FILE_READ);
  if (!file) {
    return image;  // remembered as "no picture"
  }
  uint16_t width = 0;
  uint16_t height = 0;
  if (!readPictureHeader(file, width, height)) {
    file.close();
    return image;
  }
  const size_t bytes = static_cast<size_t>(width) * height * 2;
  uint16_t *pixels = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (pixels == nullptr) {
    pixels = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  }
  if (pixels == nullptr) {
    file.close();
    target->key = "";  // try again later
    return image;
  }
  const size_t read = file.read(reinterpret_cast<uint8_t *>(pixels), bytes);
  file.close();
  if (read != bytes) {
    heap_caps_free(pixels);
    return image;
  }
  target->present = true;
  target->width = width;
  target->height = height;
  target->pixels = pixels;
  image.width = width;
  image.height = height;
  image.pixels = pixels;
  return image;
}

void forgetCachedPictures() {
  for (CacheSlot &slot : gCache) {
    releaseSlot(slot);
  }
}

bool hasChapters(const String &bookPath) { return SD_MMC.exists(folderFor(bookPath) + "/chapters.txt"); }

bool readChapters(const String &bookPath, std::vector<ChapterMarker> &chapters) {
  chapters.clear();
  File file = SD_MMC.open(folderFor(bookPath) + "/chapters.txt", FILE_READ);
  if (!file) {
    return false;
  }
  String line = file.readStringUntil('\n');
  line.trim();
  if (line != kChaptersMagic) {
    file.close();
    return false;
  }
  while (file.available() && chapters.size() < kMaxChapters) {
    line = file.readStringUntil('\n');
    line.replace("\r", "");
    const int tab = line.indexOf('\t');
    if (tab <= 0) {
      continue;
    }
    ChapterMarker marker;
    marker.wordIndex = static_cast<size_t>(line.substring(0, tab).toInt());
    marker.title = line.substring(tab + 1);
    chapters.push_back(marker);
  }
  file.close();
  std::sort(chapters.begin(), chapters.end(),
            [](const ChapterMarker &a, const ChapterMarker &b) { return a.wordIndex < b.wordIndex; });
  return true;
}

bool writeChapters(const String &bookPath, const std::vector<ChapterMarker> &chapters) {
  if (!ensureFolder(bookPath)) {
    return false;
  }
  const String target = folderFor(bookPath) + "/chapters.txt";
  const String temp = target + ".tmp";
  SD_MMC.remove(temp);
  File file = SD_MMC.open(temp, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print(kChaptersMagic);
  file.print('\n');
  for (size_t i = 0; i < chapters.size() && i < kMaxChapters; ++i) {
    String title = chapters[i].title;
    title.replace("\t", " ");
    title.replace("\n", " ");
    title.replace("\r", " ");
    title.trim();
    if (title.length() > kMaxChapterTitle) {
      title.remove(kMaxChapterTitle);
    }
    file.print(static_cast<unsigned long>(chapters[i].wordIndex));
    file.print('\t');
    file.print(title);
    file.print('\n');
  }
  file.close();
  SD_MMC.remove(target);
  return SD_MMC.rename(temp, target);
}

bool removeChapters(const String &bookPath) {
  const String target = folderFor(bookPath) + "/chapters.txt";
  const bool existed = SD_MMC.exists(target);
  SD_MMC.remove(target);
  if (folderIsEmpty(folderFor(bookPath))) {
    removeFolder(folderFor(bookPath));
  }
  return existed;
}

void applyChapters(const String &bookPath, BookMetadata &metadata) {
  std::vector<ChapterMarker> chapters;
  if (!readChapters(bookPath, chapters)) {
    return;
  }
  std::vector<ChapterMarker> valid;
  valid.reserve(chapters.size());
  for (const ChapterMarker &marker : chapters) {
    if (metadata.wordCount > 0 && marker.wordIndex >= metadata.wordCount) {
      continue;
    }
    if (!valid.empty() && valid.back().wordIndex == marker.wordIndex) {
      valid.back() = marker;
      continue;
    }
    valid.push_back(marker);
  }
  Serial.printf("[extras] %u chapters from the Flower app replace %u found in %s\n",
                static_cast<unsigned>(valid.size()), static_cast<unsigned>(metadata.chapters.size()),
                bookPath.c_str());
  metadata.chapters = valid;
}

bool readNames(const String &bookPath, String &title, String &author) {
  const String path = folderFor(bookPath) + "/names.txt";
  if (!SD_MMC.exists(path)) {
    return false;
  }
  File file = SD_MMC.open(path, FILE_READ);
  if (!file) {
    return false;
  }
  String line = file.readStringUntil('\n');
  line.trim();
  if (line != kNamesMagic) {
    file.close();
    return false;
  }
  String ownTitle = file.readStringUntil('\n');
  String ownAuthor = file.readStringUntil('\n');
  file.close();
  ownTitle.trim();
  ownAuthor.trim();
  if (!ownTitle.isEmpty()) {
    title = ownTitle;
  }
  if (!ownAuthor.isEmpty()) {
    author = ownAuthor;
  }
  return true;
}

bool writeNames(const String &bookPath, const String &title, const String &author) {
  if (!ensureFolder(bookPath)) {
    return false;
  }
  const auto oneLine = [](String text) {
    text.replace("\r", " ");
    text.replace("\n", " ");
    text.trim();
    if (text.length() > kMaxNameBytes) {
      text.remove(kMaxNameBytes);
    }
    return text;
  };
  const String target = folderFor(bookPath) + "/names.txt";
  const String temp = target + ".tmp";
  SD_MMC.remove(temp);
  File file = SD_MMC.open(temp, FILE_WRITE);
  if (!file) {
    return false;
  }
  file.print(kNamesMagic);
  file.print('\n');
  file.print(oneLine(title));
  file.print('\n');
  file.print(oneLine(author));
  file.print('\n');
  file.close();
  SD_MMC.remove(target);
  return SD_MMC.rename(temp, target);
}

bool removeNames(const String &bookPath) {
  const String target = folderFor(bookPath) + "/names.txt";
  const bool existed = SD_MMC.exists(target);
  SD_MMC.remove(target);
  if (folderIsEmpty(folderFor(bookPath))) {
    removeFolder(folderFor(bookPath));
  }
  return existed;
}

void archive(const String &bookPath) {
  const String live = folderFor(bookPath);
  if (!SD_MMC.exists(live)) {
    return;
  }
  invalidate(bookPath);
  SD_MMC.mkdir(kConfigDir);
  SD_MMC.mkdir(kArchiveRoot);
  const String archived = archiveFolderFor(bookPath);
  removeFolder(archived);  // an older copy of the same book
  if (SD_MMC.rename(live, archived)) {
    Serial.printf("[extras] archived %s -> %s\n", live.c_str(), archived.c_str());
  } else {
    Serial.printf("[extras] archive rename failed for %s\n", live.c_str());
  }
}

bool restore(const String &bookPath) {
  const String archived = archiveFolderFor(bookPath);
  if (!SD_MMC.exists(archived)) {
    return false;
  }
  const String live = folderFor(bookPath);
  if (SD_MMC.exists(live)) {
    // The book already got new extras after it came back; those win.
    removeFolder(archived);
    return false;
  }
  SD_MMC.mkdir(kConfigDir);
  SD_MMC.mkdir(kLiveRoot);
  const bool moved = SD_MMC.rename(archived, live);
  invalidate(bookPath);
  Serial.printf("[extras] %s %s\n", moved ? "restored" : "restore failed for", live.c_str());
  return moved;
}

void restoreAll(const std::vector<String> &bookPaths) {
  // One directory listing instead of an exists() per book.
  File dir = SD_MMC.open(kArchiveRoot);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return;
  }
  std::vector<String> archivedKeys;
  File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    const int slash = name.lastIndexOf('/');
    if (slash >= 0) {
      name = name.substring(slash + 1);
    }
    archivedKeys.push_back(name);
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  if (archivedKeys.empty()) {
    return;
  }
  for (const String &path : bookPaths) {
    if (std::find(archivedKeys.begin(), archivedKeys.end(), keyFor(path)) != archivedKeys.end()) {
      restore(path);
    }
  }
}

}  // namespace BookExtras
