# test

Arduino test sketch for the `ESP32-S3` controller board in this repository.

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
- `app_config.h`: pin and protocol configuration.
- `app_logic.h`: pure logic, status mapping, and default test plans.
- `app_runtime.h`: application-level test coordinator and rotating loop schedule.
- `hal_i2c.h`: I2C probing and scan side effects.
- `hal_display.h`: OLED side effects via `Adafruit SH110X`.
- `hal_servo.h`: PCA9685 side effects via `Adafruit PWM Servo Driver`.
- `hal_can_motor.h`: TWAI + `Emm_V5.0` command side effects.

## Notes

- The OLED is probed at `0x3C` first, then `0x3D`.
- The PCA9685 is expected at `0x40`.
- The servo test uses continuous-rotation semantics:
  - `1500 us` stop
  - `2000 us` forward
  - `1000 us` reverse
- The test drives `CH0` and `CH1` together to avoid 0-based/1-based port numbering ambiguity on the PCA9685 board.
- `loop()` rotates through I2C refresh, servo sweep, and CAN check instead of repeating only the servo test.
- The default CAN connectivity speed is `300 RPM`.
- The CAN test first saves motor control mode `2` (closed-loop) before enable and speed commands.

## Expected serial speed

- `115200`
