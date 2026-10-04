#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

#include "reader/BookContent.h"
#include "storage/IndexedBookStore.h"

class StorageManager {
 public:
  using StatusCallback = void (*)(void *context, const char *title, const char *line1,
                                  const char *line2, int progressPercent);

  struct DiagnosticResult {
    bool mounted = false;
    bool booksDirectory = false;
    bool bookFilesDirectory = false;
    bool articleFilesDirectory = false;
    bool configDirectory = false;
    bool pluginsDirectory = false;
    bool writable = false;
    bool booksWritable = false;
    bool articlesWritable = false;
    bool configWritable = false;
    bool foldersRepaired = false;
    size_t bookCount = 0;
    size_t unsupportedCount = 0;
    uint64_t sizeMb = 0;
    String cardType;
    String summary;
    String detail;
  };

  void setStatusCallback(StatusCallback callback, void *context);
  bool begin();
  void end();
  void listBooks();
  void refreshBooks(bool includeMetadata = true);
  bool loadFirstBookWords(std::vector<String> &words, String *loadedPath = nullptr);
  bool loadBookContent(size_t index, BookContent &book, String *loadedPath = nullptr,
                       size_t *loadedIndex = nullptr);
  bool loadIndexedBook(size_t index, IndexedBookStore &store, BookMetadata &metadata,
                       String *loadedPath = nullptr, size_t *loadedIndex = nullptr,
                       bool allowIndexBuild = true, bool allowEpubConversion = true);
  size_t bookCount() const;
  String bookPath(size_t index) const;
  bool bookIsArticle(size_t index) const;
  String bookDisplayName(size_t index) const;
  String bookAuthorName(size_t index) const;
  bool loadBookWords(size_t index, std::vector<String> &words, String *loadedPath = nullptr,
                     size_t *loadedIndex = nullptr);
  DiagnosticResult diagnoseSdCard();
  bool repairSdCardFolders();
  // Why begin() failed: no card answering at all, or a card that answers but
  // holds no FAT volume (new, exFAT, or damaged). Unmounts first.
  enum class CardProbe : uint8_t { Missing, Unreadable };
  CardProbe probeCard();
  // Erases the whole card: one MBR partition, FAT32 (FAT16 under 2 GB) with
  // 32 KB clusters, then mounts it and creates the library folders.
  bool formatCard();
  bool deleteBook(size_t index);
  // Same clean-up by path (the Flower app deletes by name, and the library
  // list the reader works from must not shift under it). No list refresh.
  bool deleteBookAtPath(const String &path);
  // Opens a library file by path for the Flower app's chapter editor without
  // touching the library list: an EPUB is converted first, a missing word
  // index is built. `readingPath` gets the file actually read (.rsvp).
  bool openIndexedBookAtPath(const String &path, IndexedBookStore &store, BookMetadata &metadata,
                             String *readingPath = nullptr);
  // True if `path` is a current library entry, OR is the .rsvp cache
  // sibling of one (an EPUB's save points are keyed by its converted cache
  // path — see epubCacheRsvpPath() — so a returned .epub source needs to
  // match against that, not its own source path).
  bool bookExistsAtPath(const String &path) const;
  // For an EPUB source path, the on-device cache/reading path is the
  // sibling ".rsvp" file — save points are keyed by whichever path was
  // actually open for reading, so callers matching against a library entry
  // need both candidates. A no-op sibling swap for non-EPUB paths.
  String epubCacheRsvpPath(const String &epubPath) const;

  // Hidden SD-side archive for save points whose book was deleted from the
  // library — never surfaced in any menu. If the same book path reappears
  // later (re-added to the library), the caller restores matching entries
  // out of this list; otherwise they just stay here, invisible.
  std::vector<String> readSavePointTrashLines();
  bool writeSavePointTrashLines(const std::vector<String> &lines);
  bool appendSavePointTrashLines(const std::vector<String> &lines);

 private:
  bool ensureIndexedBook(const String &path, BookMetadata &metadata, bool rsvpFormat,
                         bool allowIndexBuild);
  bool buildIndexedBook(const String &path, BookMetadata &metadata, bool rsvpFormat);
  bool readIndexedMetadata(const String &path, BookMetadata &metadata,
                           IndexedBookStore::Header *header = nullptr);
  bool parseFile(File &file, BookContent &book, bool rsvpFormat);
  bool ensureEpubConverted(const String &epubPath, String &rsvpPath);
  void refreshBookPaths(bool includeMetadata = true);
  void rebuildBookMetadataCache();
  void clearBookCache();
  void notifyStatus(const char *title, const char *line1 = "", const char *line2 = "",
                    int progressPercent = -1);

  bool mounted_ = false;
  bool listedOnce_ = false;
  StatusCallback statusCallback_ = nullptr;
  void *statusContext_ = nullptr;
  std::vector<String> bookPaths_;
  // Filled by rebuildBookMetadataCache(); bookDisplayName() fills a title
  // that came back empty (file busy during a download) on first use.
  mutable std::vector<String> bookTitles_;
  mutable std::vector<bool> bookTitleRetried_;
  mutable std::vector<String> bookAuthors_;
};
