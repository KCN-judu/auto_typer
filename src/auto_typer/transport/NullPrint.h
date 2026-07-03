#pragma once

#include <Arduino.h>

namespace auto_typer {

class NullPrint : public Print {
 public:
  size_t write(uint8_t) override {
    return 1;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    (void)buffer;
    return size;
  }
};

}  // namespace auto_typer
