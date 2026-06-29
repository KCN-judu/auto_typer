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

    const StatusText text = textForStatus(status);
    display_.clearDisplay();
    drawCenteredAscii(3, text.english);
    drawCenteredChinese(15, text.chinese);
    display_.display();
  }

  bool ready() const {
    return ready_;
  }

  uint8_t address() const {
    return address_;
  }

 private:
  struct StatusText {
    const char* english;
    const char* chinese;
  };

  struct AsciiGlyph {
    char value;
    uint8_t width;
    uint8_t rows[7];
  };

  struct ChineseGlyph {
    const char* value;
    uint16_t rows[16];
  };

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

  static StatusText textForStatus(DisplayStatus status) {
    switch (status) {
      case DisplayStatus::Idle:
        return {"THE PAPER IS BLANK.", "纸还空着"};
      case DisplayStatus::Printing:
        return {"INK MEETS FIBER.", "墨,抵抗着纤维"};
      case DisplayStatus::Complete:
        return {"THE ORIGINAL HAS NO COPY.", "原件没有副本"};
      case DisplayStatus::Error:
      default:
        return {"HUMAN NEEDED.", "需要一双人手"};
    }
  }

  void drawCenteredAscii(int16_t y, const char* text) {
    const int16_t textWidth = asciiTextWidth(text);
    int16_t x = centeredX(textWidth);
    for (size_t i = 0; text[i] != '\0'; ++i) {
      const AsciiGlyph* glyph = findAsciiGlyph(text[i]);
      if (glyph == nullptr) {
        x += 5;
        continue;
      }
      drawAsciiGlyph(x, y, *glyph);
      x += glyph->width + 1;
    }
  }

  int16_t asciiTextWidth(const char* text) const {
    int16_t width = 0;
    for (size_t i = 0; text[i] != '\0'; ++i) {
      const AsciiGlyph* glyph = findAsciiGlyph(text[i]);
      width += glyph != nullptr ? glyph->width : 4;
      if (text[i + 1] != '\0') {
        ++width;
      }
    }
    return width;
  }

  void drawAsciiGlyph(int16_t x, int16_t y, const AsciiGlyph& glyph) {
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t col = 0; col < glyph.width; ++col) {
        if ((glyph.rows[row] & (1 << (glyph.width - 1 - col))) != 0) {
          display_.drawPixel(x + col, y + row, SH110X_WHITE);
        }
      }
    }
  }

  void drawCenteredChinese(int16_t y, const char* text) {
    const int16_t textWidth = chineseTextWidth(text);
    int16_t x = centeredX(textWidth);
    for (size_t i = 0; text[i] != '\0';) {
      if (text[i] == ',') {
        drawComma(x, y);
        x += 4;
        ++i;
        continue;
      }

      const ChineseGlyph* glyph = findChineseGlyph(text + i);
      if (glyph == nullptr) {
        x += 8;
        ++i;
        continue;
      }
      drawChineseGlyph(x, y, *glyph);
      x += 16;
      i += strlen(glyph->value);
    }
  }

  int16_t chineseTextWidth(const char* text) const {
    int16_t width = 0;
    for (size_t i = 0; text[i] != '\0';) {
      if (text[i] == ',') {
        width += 4;
        ++i;
        continue;
      }
      const ChineseGlyph* glyph = findChineseGlyph(text + i);
      if (glyph == nullptr) {
        width += 8;
        ++i;
        continue;
      }
      width += 16;
      i += strlen(glyph->value);
    }
    return width;
  }

  void drawChineseGlyph(int16_t x, int16_t y, const ChineseGlyph& glyph) {
    for (uint8_t row = 0; row < 16; ++row) {
      for (uint8_t col = 0; col < 16; ++col) {
        if ((glyph.rows[row] & (1 << (15 - col))) != 0) {
          display_.drawPixel(x + col, y + row, SH110X_WHITE);
        }
      }
    }
  }

  void drawComma(int16_t x, int16_t y) {
    display_.drawPixel(x + 1, y + 12, SH110X_WHITE);
    display_.drawPixel(x + 1, y + 13, SH110X_WHITE);
    display_.drawPixel(x, y + 14, SH110X_WHITE);
  }

  int16_t centeredX(int16_t width) const {
    return width >= static_cast<int16_t>(config_.width) ? 0 : (static_cast<int16_t>(config_.width) - width) / 2;
  }

  static const AsciiGlyph* findAsciiGlyph(char value) {
    if (value >= 'a' && value <= 'z') {
      value = static_cast<char>(value - 'a' + 'A');
    }
    for (size_t i = 0; i < kAsciiGlyphCount; ++i) {
      if (kAsciiGlyphs[i].value == value) {
        return &kAsciiGlyphs[i];
      }
    }
    return nullptr;
  }

  static const ChineseGlyph* findChineseGlyph(const char* value) {
    for (size_t i = 0; i < kChineseGlyphCount; ++i) {
      if (strncmp(value, kChineseGlyphs[i].value, strlen(kChineseGlyphs[i].value)) == 0) {
        return &kChineseGlyphs[i];
      }
    }
    return nullptr;
  }

  static const size_t kAsciiGlyphCount = 22;
  static const size_t kChineseGlyphCount = 21;
  static const AsciiGlyph kAsciiGlyphs[];
  static const ChineseGlyph kChineseGlyphs[];

  OledConfig config_;
  Adafruit_SH1106G display_;
  bool ready_;
  uint8_t address_;
};

const DisplayHal::AsciiGlyph DisplayHal::kAsciiGlyphs[] = {
    {' ', 3, {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}},
    {'.', 1, {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1}},
    {'A', 4, {0x6, 0x9, 0x9, 0xF, 0x9, 0x9, 0x9}},
    {'B', 4, {0xE, 0x9, 0x9, 0xE, 0x9, 0x9, 0xE}},
    {'C', 4, {0x7, 0x8, 0x8, 0x8, 0x8, 0x8, 0x7}},
    {'D', 4, {0xE, 0x9, 0x9, 0x9, 0x9, 0x9, 0xE}},
    {'E', 4, {0xF, 0x8, 0x8, 0xE, 0x8, 0x8, 0xF}},
    {'F', 4, {0xF, 0x8, 0x8, 0xE, 0x8, 0x8, 0x8}},
    {'G', 4, {0x7, 0x8, 0x8, 0xB, 0x9, 0x9, 0x7}},
    {'H', 4, {0x9, 0x9, 0x9, 0xF, 0x9, 0x9, 0x9}},
    {'I', 3, {0x7, 0x2, 0x2, 0x2, 0x2, 0x2, 0x7}},
    {'K', 4, {0x9, 0xA, 0xC, 0x8, 0xC, 0xA, 0x9}},
    {'L', 4, {0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0xF}},
    {'M', 5, {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', 4, {0x9, 0xD, 0xD, 0xB, 0xB, 0x9, 0x9}},
    {'O', 4, {0x6, 0x9, 0x9, 0x9, 0x9, 0x9, 0x6}},
    {'P', 4, {0xE, 0x9, 0x9, 0xE, 0x8, 0x8, 0x8}},
    {'R', 4, {0xE, 0x9, 0x9, 0xE, 0xC, 0xA, 0x9}},
    {'S', 4, {0x7, 0x8, 0x8, 0x6, 0x1, 0x1, 0xE}},
    {'T', 5, {0x1F, 0x4, 0x4, 0x4, 0x4, 0x4, 0x4}},
    {'U', 4, {0x9, 0x9, 0x9, 0x9, 0x9, 0x9, 0x6}},
    {'Y', 5, {0x11, 0x11, 0xA, 0x4, 0x4, 0x4, 0x4}},
};

const DisplayHal::ChineseGlyph DisplayHal::kChineseGlyphs[] = {
    {"纸", {0x0000, 0x1018, 0x33F8, 0x2360, 0x6F60, 0x7B20, 0x13FC, 0x3320, 0x7F20, 0x6320, 0x0330, 0x0B94, 0x7B1C, 0x0208, 0x0000, 0x0000}},
    {"还", {0x0000, 0x2000, 0x37FC, 0x1060, 0x00C0, 0x70E0, 0x31F0, 0x22D8, 0x7CCC, 0x10C0, 0x10C0, 0x30C0, 0x7FFC, 0x43FC, 0x0000, 0x0000}},
    {"空", {0x0000, 0x0100, 0x0180, 0x7FFC, 0x644C, 0x6E48, 0x1840, 0x3078, 0x0000, 0x1FF0, 0x0100, 0x0100, 0x0100, 0x7FFC, 0x0000, 0x0000}},
    {"着", {0x0000, 0x0440, 0x0440, 0x7FF8, 0x0100, 0x3FF8, 0x7FFC, 0x0400, 0x0FF0, 0x1810, 0x3FF0, 0x4FF0, 0x0810, 0x0FF0, 0x0810, 0x0000}},
    {"墨", {0x0000, 0x0000, 0x3FF8, 0x3558, 0x3FF8, 0x0100, 0x3FF8, 0x0100, 0x7FFC, 0x24D8, 0x2108, 0x1FF8, 0x0100, 0x7FFC, 0x0000, 0x0000}},
    {"抵", {0x0000, 0x3000, 0x3038, 0x33F8, 0x7A60, 0x3260, 0x3260, 0x33FC, 0x3A60, 0x7220, 0x33A0, 0x3330, 0x3614, 0x77FC, 0x6008, 0x0000}},
    {"抗", {0x0000, 0x1040, 0x1040, 0x1000, 0x7BFC, 0x1000, 0x11F0, 0x1D30, 0x7930, 0x7130, 0x1130, 0x1330, 0x1334, 0x763C, 0x7400, 0x0000}},
    {"纤", {0x0000, 0x1808, 0x19F8, 0x31E0, 0x2620, 0x7C20, 0x5820, 0x11FC, 0x3420, 0x7C20, 0x0020, 0x3E20, 0x7020, 0x0020, 0x0000, 0x0000}},
    {"维", {0x0000, 0x11A0, 0x11B0, 0x3100, 0x27FC, 0x6F20, 0x7FF8, 0x1320, 0x3120, 0x3DF8, 0x0120, 0x0120, 0x7DFC, 0x0100, 0x0300, 0x0000}},
    {"原", {0x0000, 0x3FFC, 0x3080, 0x37F8, 0x3418, 0x37F8, 0x3418, 0x3418, 0x37F8, 0x20C0, 0x26D8, 0x6CCC, 0x68C0, 0x0180, 0x0000, 0x0000}},
    {"件", {0x0000, 0x0860, 0x1960, 0x1360, 0x3360, 0x33FC, 0x7660, 0x5460, 0x1060, 0x17FC, 0x1060, 0x1060, 0x1060, 0x1060, 0x1060, 0x0000}},
    {"没", {0x0000, 0x2200, 0x3BE0, 0x1320, 0x0230, 0x663C, 0x7400, 0x07F0, 0x0230, 0x3320, 0x31E0, 0x20C0, 0x61E0, 0x6F3C, 0x0C08, 0x0000}},
    {"有", {0x0000, 0x0300, 0x0200, 0x7FFC, 0x0400, 0x0FF0, 0x1810, 0x3810, 0x6FF0, 0x0810, 0x0FF0, 0x0810, 0x0830, 0x0870, 0x0000, 0x0000}},
    {"副", {0x0000, 0x0008, 0x7F88, 0x0068, 0x3F68, 0x2168, 0x3F68, 0x0068, 0x7F68, 0x6D68, 0x7F68, 0x6D08, 0x7F08, 0x6138, 0x0018, 0x0000}},
    {"本", {0x0000, 0x0180, 0x0100, 0x0100, 0x7FFC, 0x03C0, 0x05C0, 0x0540, 0x0960, 0x1930, 0x311C, 0x6FEC, 0x4100, 0x0100, 0x0100, 0x0000}},
    {"需", {0x0000, 0x1FF8, 0x0100, 0x7FFC, 0x5D7C, 0x4100, 0x0100, 0x1D70, 0x7FFC, 0x0300, 0x3FF8, 0x3498, 0x3498, 0x34B8, 0x0030, 0x0000}},
    {"要", {0x0000, 0x7FFC, 0x0480, 0x3FF8, 0x3498, 0x3498, 0x3FF8, 0x0200, 0x7FFC, 0x0460, 0x0FC0, 0x03E0, 0x3E38, 0x3008, 0x0000, 0x0000}},
    {"一", {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x7FFC, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000}},
    {"双", {0x0000, 0x0000, 0x7FF8, 0x0688, 0x2498, 0x3498, 0x1C90, 0x1CD0, 0x0C70, 0x1C60, 0x1660, 0x33F8, 0x639C, 0x0204, 0x0000, 0x0000}},
    {"人", {0x0000, 0x0000, 0x0380, 0x0100, 0x0100, 0x0300, 0x0300, 0x0380, 0x0380, 0x06C0, 0x04C0, 0x0C60, 0x1838, 0x701C, 0x600C, 0x0000}},
    {"手", {0x0000, 0x0078, 0x3FF8, 0x0180, 0x0180, 0x1FF0, 0x0180, 0x0180, 0x7FFC, 0x0180, 0x0180, 0x0180, 0x0180, 0x0780, 0x0700, 0x0000}},
};

}  // namespace auto_typer
