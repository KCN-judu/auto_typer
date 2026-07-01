#pragma once

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "auto_typer_types.h"

namespace auto_typer {

enum class DisplayStatus : uint8_t {
  Idle,
  Printing,
  Complete,
  Error,
};

class DisplayHal {
 public:
  explicit DisplayHal(const OledConfig& config)
      : config_(config),
        display_(config.width, config.height, &Wire, -1, config.preclkHz, config.postclkHz),
        ready_(false),
        address_(0) {}

  bool begin() {
    if (tryBegin(config_.primaryAddress)) {
      return true;
    }
    if (tryBegin(config_.fallbackAddress)) {
      return true;
    }
    ready_ = false;
    address_ = 0;
    return false;
  }

  void showStatus(DisplayStatus status) {
    if (!ready_) {
      return;
    }

    const char* text = textForStatus(status);
    display_.clearDisplay();
    drawStatusText(text);
    display_.display();
  }

  bool ready() const {
    return ready_;
  }

  uint8_t address() const {
    return address_;
  }

 private:
  bool tryBegin(uint8_t address) {
    ready_ = display_.begin(address, false);
    if (!ready_) {
      return false;
    }

    address_ = address;
    configurePanel();
    display_.setRotation(2);
    display_.setTextColor(SH110X_WHITE);
    display_.clearDisplay();
    display_.display();
    return true;
  }

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

  static const char* textForStatus(DisplayStatus status) {
    switch (status) {
      case DisplayStatus::Idle:
        return "The paper is blank.";
      case DisplayStatus::Printing:
        return "Ink meets fiber.";
      case DisplayStatus::Complete:
        return "The original has no copy.";
      case DisplayStatus::Error:
      default:
        return "Human needed.";
    }
  }

  void drawStatusText(const char* text) {
    display_.setFont();
    display_.setTextSize(1);

    TextLayout layout{};
    layoutText(text, layout);

    int16_t maxLineHeight = 0;
    for (uint8_t i = 0; i < layout.lineCount; ++i) {
      measureText(layout.lines[i], layout.bounds[i]);
      if (static_cast<int16_t>(layout.bounds[i].height) > maxLineHeight) {
        maxLineHeight = static_cast<int16_t>(layout.bounds[i].height);
      }
    }
    const int16_t blockHeight = maxLineHeight + (layout.lineCount - 1) * kLineHeight;

    int16_t y = kMarginY;
    if (blockHeight < availableHeight()) {
      y += (availableHeight() - blockHeight) / 2;
    }

    for (uint8_t i = 0; i < layout.lineCount; ++i) {
      drawCenteredLine(layout.lines[i], layout.bounds[i], y);
      y += kLineHeight;
    }
    display_.setFont();
  }

  struct TextBounds {
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
  };

  struct TextLayout {
    char lines[2][32];
    TextBounds bounds[2];
    uint8_t lineCount;
  };

  void layoutText(const char* text, TextLayout& layout) {
    layout.lineCount = 1;
    copyLine(layout.lines[0], text, 0, strlen(text));
    layout.lines[1][0] = '\0';

    if (measureTextWidth(layout.lines[0]) <= availableWidth()) {
      return;
    }

    if (splitText(text, layout)) {
      return;
    }

    layout.lineCount = 1;
  }

  bool splitText(const char* text, TextLayout& layout) {
    const size_t length = strlen(text);
    size_t bestSplit = 0;
    int16_t bestBalance = INT16_MAX;

    for (size_t i = 0; i < length; ++i) {
      if (text[i] != ' ') {
        continue;
      }
      const int16_t firstWidth = measureTextWidth(text, 0, i);
      const int16_t secondWidth = measureTextWidth(text, i + 1, length);
      if (firstWidth > availableWidth() || secondWidth > availableWidth()) {
        continue;
      }
      const int16_t balance = abs(firstWidth - secondWidth);
      if (balance < bestBalance) {
        bestBalance = balance;
        bestSplit = i;
      }
    }

    if (bestSplit == 0) {
      return false;
    }

    copyLine(layout.lines[0], text, 0, bestSplit);
    copyLine(layout.lines[1], text, bestSplit + 1, length);
    layout.lineCount = 2;
    return true;
  }

  int16_t measureTextWidth(const char* text, size_t start, size_t end) {
    char line[32];
    copyLine(line, text, start, end);
    return measureTextWidth(line);
  }

  int16_t measureTextWidth(const char* text) {
    TextBounds bounds{};
    measureText(text, bounds);
    return static_cast<int16_t>(bounds.width);
  }

  void measureText(const char* text, TextBounds& bounds) {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    display_.getTextBounds(text, 0, 0, &x, &y, &width, &height);
    bounds = {x, y, width, height};
  }

  void drawCenteredLine(const char* text, const TextBounds& bounds, int16_t top) {
    const int16_t x = kMarginX + (availableWidth() - static_cast<int16_t>(bounds.width)) / 2 - bounds.x;
    const int16_t y = top - bounds.y;
    display_.setCursor(x, y);
    display_.print(text);
  }

  static void copyLine(char* target, const char* source, size_t start, size_t end) {
    size_t out = 0;
    for (size_t i = start; i < end && out < 31; ++i) {
      target[out] = source[i];
      ++out;
    }
    target[out] = '\0';
  }

  int16_t availableWidth() const {
    const int16_t width = static_cast<int16_t>(config_.width) - 2 * kMarginX;
    return width > 0 ? width : static_cast<int16_t>(config_.width);
  }

  int16_t availableHeight() const {
    const int16_t height = static_cast<int16_t>(config_.height) - 2 * kMarginY;
    return height > 0 ? height : static_cast<int16_t>(config_.height);
  }

  static const int16_t kMarginX = 4;
  static const int16_t kMarginY = 1;
  static const int16_t kLineHeight = 8;

  OledConfig config_;
  Adafruit_SH1106G display_;
  bool ready_;
  uint8_t address_;
};

}  // namespace auto_typer
