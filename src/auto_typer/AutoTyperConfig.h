#ifndef AUTO_TYPER_PUBLIC_CONFIG_H
#define AUTO_TYPER_PUBLIC_CONFIG_H

namespace auto_typer {

struct WifiSecrets {
  const char* ssid;
  const char* password;
};

struct FirmwareConfig {
  WifiSecrets wifi;
};

}  // namespace auto_typer

#endif  // AUTO_TYPER_PUBLIC_CONFIG_H
