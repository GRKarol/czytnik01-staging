#pragma once

#include <Arduino.h>
#include <vector>

#include "display/DisplayManager.h"
#include "reader/BookContent.h"

// What the Flower app adds to a book on top of its text: a cover picture, a
// spine picture for the library shelf and a hand-made chapter list. They live
// on the SD card next to the library, one folder per book:
//
//   /config/books/<key>/cover.img     92x116 RGB565 ("FBI1" header)
//   /config/books/<key>/spine.img     36x72 RGB565
//   /config/books/<key>/chapters.txt  "FCH1" then "wordIndex<TAB>title" lines
//   /config/books/<key>/path.txt      the book path, for people reading the card
//
// <key> is a hash of the book path without its extension, so an EPUB and the
// .rsvp the reader converts it to share one folder. When a book is deleted
// its folder moves to /config/archive/<key>/ (with the book's save points,
// see App::archiveSavePointsForDeletedBook) and moves back as soon as a book
// with the same path is in the library again.
namespace BookExtras {

enum class Picture : uint8_t { Cover, Spine };

constexpr uint16_t kCoverWidth = 92;
constexpr uint16_t kCoverHeight = 116;
constexpr uint16_t kSpineWidth = 36;
constexpr uint16_t kSpineHeight = 72;
constexpr size_t kMaxChapterTitle = 60;
constexpr size_t kMaxChapters = 400;

String folderFor(const String &bookPath);
String archiveFolderFor(const String &bookPath);
String picturePath(const String &bookPath, Picture kind);

// Checks a freshly written picture file (header, size) and moves it into
// place; removes it and returns false when it is not a picture we can draw.
bool installPicture(const String &bookPath, Picture kind, const String &uploadedPath, String &error);
bool removePicture(const String &bookPath, Picture kind);
bool hasPicture(const String &bookPath, Picture kind);

// The picture ready to draw, or an invalid image when the book has none.
// Kept in a small cache (PSRAM), so the shelf can ask on every frame.
NanoImage picture(const String &bookPath, Picture kind);
void forgetCachedPictures();

bool hasChapters(const String &bookPath);
bool readChapters(const String &bookPath, std::vector<ChapterMarker> &chapters);
bool writeChapters(const String &bookPath, const std::vector<ChapterMarker> &chapters);
bool removeChapters(const String &bookPath);
// Replaces the chapters the reader found in the text with the hand-made list
// when the book has one; markers past the end of the book are dropped.
void applyChapters(const String &bookPath, BookMetadata &metadata);

// Title and author set in the Flower app over the ones in the file
// (names.txt: "FNM1", then the title line and the author line, UTF-8). An
// empty field keeps the file's own. readNames() leaves the arguments alone
// when the book has no names file.
bool readNames(const String &bookPath, String &title, String &author);
bool writeNames(const String &bookPath, const String &title, const String &author);
bool removeNames(const String &bookPath);

// Book deleted: its folder goes to the archive. Book back: folder returns.
void archive(const String &bookPath);
bool restore(const String &bookPath);
void restoreAll(const std::vector<String> &bookPaths);

}  // namespace BookExtras
