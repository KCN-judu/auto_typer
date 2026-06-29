#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "auto_typer_types.h"

namespace auto_typer {

class KeymapStore {
 public:
  KeymapStore() : version_(1), layoutVersion_(0) {}

  bool load(KeyBinding* bindings, size_t capacity, size_t& count) {
    Preferences prefs;
    if (!prefs.begin("auto-typer", true)) {
      return false;
    }
    version_ = prefs.getUInt("keymapVer", 1);
    layoutVersion_ = prefs.getUInt("keymapLayoutVer", 0);
    const String encoded = prefs.getString("keymap", "");
    prefs.end();

    if (encoded.length() == 0) {
      return false;
    }

    size_t nextCount = 0;
    int offset = 0;
    while (offset < encoded.length() && nextCount < capacity) {
      const int lineEnd = encoded.indexOf('\n', offset);
      const String line = lineEnd < 0 ? encoded.substring(offset) : encoded.substring(offset, lineEnd);
      offset = lineEnd < 0 ? encoded.length() : lineEnd + 1;
      if (line.length() < 5) {
        continue;
      }
      const int firstComma = line.indexOf(',');
      const int secondComma = line.indexOf(',', firstComma + 1);
      if (firstComma <= 0 || secondComma <= firstComma) {
        continue;
      }
      bindings[nextCount] = {
        static_cast<char>(line.substring(0, firstComma).toInt()),
        {line.substring(firstComma + 1, secondComma).toFloat(), line.substring(secondComma + 1).toFloat()},
      };
      ++nextCount;
    }

    if (nextCount == 0) {
      return false;
    }
    count = nextCount;
    return true;
  }

  bool save(const KeyBinding* bindings, size_t count, uint32_t layoutVersion = 0) {
    if (layoutVersion != 0) {
      layoutVersion_ = layoutVersion;
    }

    String encoded;
    for (size_t i = 0; i < count; ++i) {
      encoded += static_cast<int>(bindings[i].key);
      encoded += ",";
      encoded += String(bindings[i].point.xMm, 3);
      encoded += ",";
      encoded += String(bindings[i].point.yMm, 3);
      encoded += "\n";
    }

    Preferences prefs;
    if (!prefs.begin("auto-typer", false)) {
      return false;
    }
    ++version_;
    const bool ok = prefs.putString("keymap", encoded) > 0 && prefs.putUInt("keymapVer", version_) > 0 &&
                    prefs.putUInt("keymapLayoutVer", layoutVersion_) > 0;
    prefs.end();
    return ok;
  }

  uint32_t version() const {
    return version_;
  }

  uint32_t layoutVersion() const {
    return layoutVersion_;
  }

 private:
  uint32_t version_;
  uint32_t layoutVersion_;
};

}  // namespace auto_typer
