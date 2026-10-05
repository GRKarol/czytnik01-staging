#pragma once

#include <Arduino.h>

#include "board/BoardConfig.h"

enum class TouchPhase {
  Start,
  Move,
  End,
};

struct TouchEvent {
  bool touched = false;
  uint16_t x = 0;
  uint16_t y = 0;
  uint8_t gesture = 0;
  TouchPhase phase = TouchPhase::Move;
};

class TouchHandler {
 public:
  bool begin();
  void end();
  bool poll(TouchEvent &event);
  void cancel();
  // Drops the touch going on now and reports nothing until the controller
  // shows no finger once: a finger (or a stale reading) left from before
  // this point can't become a press.
  void ignoreUntilReleased();
  void setUiOrientation(BoardConfig::UiOrientation orientation);
  void setUiRotated180(bool rotated180);

 private:
  static constexpr uint8_t kAddress = 0x3B;  // AXS15231B touch endpoint on the 3.49" board.
  bool initialized_ = false;
  uint32_t lastPollMs_ = 0;
  uint32_t backoffUntilMs_ = 0;
  uint32_t lastTouchSampleMs_ = 0;
  uint8_t consecutiveReadFailures_ = 0;
  uint8_t emptyTouchSamples_ = 0;
  bool touchActive_ = false;
  bool waitForRelease_ = false;
  BoardConfig::UiOrientation uiOrientation_ =
      BoardConfig::UI_ROTATED_180 ? BoardConfig::UiOrientation::LandscapeFlipped
                                  : BoardConfig::UiOrientation::Landscape;
  uint16_t lastX_ = 0;
  uint16_t lastY_ = 0;

  bool readTouchPacket(uint8_t *buffer, size_t len);
};
