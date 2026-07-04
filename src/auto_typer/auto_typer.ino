#include "AutoTyperFirmware.h"

#if defined(__has_include)
#if __has_include("config/Secrets.h")
#include "config/Secrets.h"
#define AUTO_TYPER_HAS_SECRETS_H 1
#endif
#endif

#ifndef AUTO_TYPER_HAS_SECRETS_H
#define AUTO_TYPER_WIFI_SSID ""
#define AUTO_TYPER_WIFI_PASSWORD ""
#endif

void setup() {
  const auto_typer::FirmwareConfig config = {
      {
          AUTO_TYPER_WIFI_SSID,
          AUTO_TYPER_WIFI_PASSWORD,
      },
  };
  auto_typer::autoTyperSetup(config);
}

void loop() {
  auto_typer::autoTyperLoop();
}
