# test

Arduino test sketch for the `ESP32-S3` controller board in this repository.

This is a standalone hardware connectivity sketch, not the production Auto Typer firmware. For production flashing, SoftAP provisioning, desktop connection, and printing, use [`../docs/auto_typer_flash_and_usage_guide.typ`](../docs/auto_typer_flash_and_usage_guide.typ).

## Target

- MCU: `ESP32-S3`
- I2C: `SDA=GPIO6`, `SCL=GPIO7`
- CAN/TWAI: `TX=GPIO4`, `RX=GPIO5`
- CAN bitrate: `500 kbps`

## Covered devices

- `SH1106` 128x32 I2C OLED
- `PCA9685` servo driver board
- `MG996R` continuous rotation servo on connector `1`
- `Emm_V5.0` CAN motor driver at address `1`

## Required libraries

Install these with Arduino Library Manager or `arduino-cli`:

- `Adafruit GFX Library`
- `Adafruit SH110X`
- `Adafruit PWM Servo Driver Library`

## Architecture

- `test.ino`: Arduino lifecycle entry only.
- `appConfig.h`: pin and protocol configuration.
- `appLogic.h`: pure logic, status mapping, and default test plans.
- `appRuntime.h`: application-level test coordinator and rotating loop schedule.
- `halI2c.h`: I2C probing and scan side effects.
- `halDisplay.h`: OLED side effects via `Adafruit SH110X`.
- `halServo.h`: PCA9685 side effects via `Adafruit PWM Servo Driver`.
- `halCanMotor.h`: TWAI + `Emm_V5.0` command side effects.

## Notes

- The OLED is probed at `0x3C` first, then `0x3D`.
- The PCA9685 is expected at `0x40`.
- The servo test uses continuous-rotation semantics:
  - `1500 us` stop
  - `2000 us` forward
  - `1000 us` reverse
- The test drives `CH0` and `CH1` together to avoid 0-based/1-based port numbering ambiguity on the PCA9685 board.
- `loop()` rotates through I2C refresh, servo sweep, and CAN check instead of repeating only the servo test.
- The CAN test saves closed-loop mode, enables the motor, clears its pulse position, moves to the absolute target `+3200`, and returns to absolute `0` at `300 RPM`.
- Place the motor mechanism at its physical origin before starting the sketch; position clearing establishes the pulse coordinate but does not perform mechanical homing.

## Expected serial speed

- `115200`
