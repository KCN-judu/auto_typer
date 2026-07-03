#pragma once

#include "auto_typer_types.h"

namespace auto_typer {

static const uint32_t kFeiyu200KeymapLayoutVersion = 8;

struct Feiyu200LayoutConfig {
  MachinePointMm origin;
  float keyPitchX;
  float rowY[5];
  float rowOffsets[5];
};

inline Feiyu200LayoutConfig defaultFeiyu200LayoutConfig() {
  Feiyu200LayoutConfig config{};
  config.origin = {28.775f, 0.0f};
  config.keyPitchX = 19.25f;
  config.rowY[0] = 108.925f;
  config.rowY[1] = 89.9625f;
  config.rowY[2] = 68.0f;
  config.rowY[3] = 52.4625f;
  config.rowY[4] = 30.0f;
  config.rowOffsets[0] = 19.0f;
  config.rowOffsets[1] = 22.5f;
  config.rowOffsets[2] = 25.0f;
  config.rowOffsets[3] = 37.5f;
  config.rowOffsets[4] = 137.5f;
  return config;
}

inline bool appendKey(KeyBinding* bindings,
                      size_t capacity,
                      size_t& count,
                      char key,
                      MachinePointMm point) {
  if (count >= capacity) {
    return false;
  }
  bindings[count] = {key, point};
  ++count;
  return true;
}

inline bool appendRow(KeyBinding* bindings,
                      size_t capacity,
                      size_t& count,
                      const char* keys,
                      uint8_t row,
                      const Feiyu200LayoutConfig& config) {
  for (size_t i = 0; keys[i] != '\0'; ++i) {
    const MachinePointMm point = {
      config.origin.xMm + config.rowOffsets[row] + static_cast<float>(i) * config.keyPitchX,
      config.origin.yMm + config.rowY[row],
    };
    if (!appendKey(bindings, capacity, count, keys[i], point)) {
      return false;
    }
  }
  return true;
}

inline bool isPoetryFeiyu200Key(char key) {
  static const char kKeys[] = "1234567890-qwertyuiopasdfghjkl;'zxcvbnm,.- ";
  for (size_t i = 0; kKeys[i] != '\0'; ++i) {
    if (kKeys[i] == key) {
      return true;
    }
  }
  return false;
}

inline bool appendPoetryRow(KeyBinding* bindings,
                            size_t capacity,
                            size_t& count,
                            const char* physicalKeys,
                            uint8_t row,
                            const Feiyu200LayoutConfig& config) {
  for (size_t i = 0; physicalKeys[i] != '\0'; ++i) {
    if (!isPoetryFeiyu200Key(physicalKeys[i])) {
      continue;
    }
    const MachinePointMm point = {
      config.origin.xMm + config.rowOffsets[row] + static_cast<float>(i) * config.keyPitchX,
      config.origin.yMm + config.rowY[row],
    };
    if (!appendKey(bindings, capacity, count, physicalKeys[i], point)) {
      return false;
    }
  }
  return true;
}

inline size_t buildFeiyu200Keymap(KeyBinding* bindings,
                                  size_t capacity,
                                  const Feiyu200LayoutConfig& config = defaultFeiyu200LayoutConfig()) {
  size_t count = 0;
  appendPoetryRow(bindings, capacity, count, "1234567890-=", 0, config);
  appendPoetryRow(bindings, capacity, count, "qwertyuiop[]", 1, config);
  appendPoetryRow(bindings, capacity, count, "asdfghjkl;'", 2, config);
  appendPoetryRow(bindings, capacity, count, "zxcvbnm,./", 3, config);

  const MachinePointMm spacePoint = {
    config.origin.xMm + config.rowOffsets[4],
    config.origin.yMm + config.rowY[4],
  };
  appendKey(bindings, capacity, count, ' ', spacePoint);

  return count;
}

}  // namespace auto_typer
