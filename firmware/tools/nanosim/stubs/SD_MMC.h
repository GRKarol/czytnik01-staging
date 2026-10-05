#pragma once
#include <Arduino.h>
class File {
 public:
  explicit operator bool() const { return false; }
  size_t read(uint8_t *, size_t) { return 0; }
  size_t size() const { return 0; }
  void close() {}
  bool seek(size_t) { return false; }
  int available() { return 0; }
  bool isDirectory() const { return false; }
};
struct SdMmcStub {
  bool exists(const String &) { return false; }
  File open(const String &, const char * = "r") { return File(); }
  bool remove(const String &) { return false; }
};
extern SdMmcStub SD_MMC;
