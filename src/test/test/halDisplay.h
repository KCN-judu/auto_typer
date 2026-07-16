#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Wire.h>

#include "appConfig.h"
#include "appTypes.h"

namespace test_app {

class DisplayHal {
 public:
  explicit DisplayHal(const OledConfig& config)
      : config_(config),
        display_(config.width, config.height, &Wire, -1, config.preclkHz, config.postclkHz),
        ready_(false) {}

  bool init(uint8_t address) {
    ready_ = display_.begin(address, false);
    if (!ready_) {
      return false;
    }

    configurePanel();
    display_.setRotation(2);
    display_.clearDisplay();
    display_.setTextSize(1);
    display_.setTextColor(SH110X_WHITE);
    return true;
  }

  void show(const DisplayFrame& frame) {
    if (!ready_) {
      return;
    }

    display_.clearDisplay();
    drawLine(1, frame.line0);
    drawLine(9, frame.line1);
    drawLine(17, frame.line2);
    drawLine(25, frame.line3);
    display_.display();
  }

  bool ready() const {
    return ready_;
  }

  void markUnavailable() {
    ready_ = false;
  }

 private:
  void configurePanel() {
    if (config_.width == 128 && config_.height == 32) {
      static const uint8_t init128x32[] = {
          SH110X_DISPLAYOFF,
          SH110X_SETMULTIPLEX,
          0x1F,
          SH110X_SETDISPLAYOFFSET,
          0x00,
          SH110X_SETCOMPINS,
          0x02,
          SH110X_SETCONTRAST,
          0x8F,
          SH110X_DISPLAYALLON_RESUME,
          SH110X_NORMALDISPLAY,
          SH110X_DISPLAYON,
      };
      display_.oled_commandList(init128x32, sizeof(init128x32));
    }
  }

  void drawLine(int16_t y, const char* text) {
    display_.setCursor(0, y);
    if (text != nullptr) {
      display_.print(text);
    }
  }

  OledConfig config_;
  Adafruit_SH1106G display_;
  bool ready_;
};

}  // namespace test_app
