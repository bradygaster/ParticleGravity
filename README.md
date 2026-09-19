# ParticleGravity

A touch-configurable kinetic particle toy for the Waveshare ESP32-S3-Touch-AMOLED-1.75.

Particles respond to the onboard QMI8658 accelerometer, collide and settle within the round 466 x 466 CO5300 AMOLED display. The setup wizard controls seed color, color variation, particle count, and gravity sensitivity. Touch the running simulation to return to setup and recalibrate.

## Hardware

- Waveshare ESP32-S3-Touch-AMOLED-1.75
- 466 x 466 CO5300 AMOLED
- QMI8658 accelerometer
- CST9217 touch controller
- 8 MB OPI PSRAM

## Requirements

- Arduino CLI
- Arduino-ESP32 core 3.3.10

Install the core:

```sh
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.10
```

The hardware-specific Arduino_GFX 1.6.4 and SensorLib 0.3.1 sources are vendored under `libraries/` so the project builds independently of the original Waveshare repository.

## Build

OPI PSRAM must be enabled because the wizard uses a full-screen off-screen framebuffer.

```sh
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi" \
  --libraries libraries \
  .
```

## Upload

Replace the port if the board enumerates under a different device name:

```sh
arduino-cli upload \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi" \
  --port /dev/cu.usbmodem101 \
  .
```

## Use

1. Choose a seed color.
2. Set color variation from a solid seed color to the full rainbow.
3. Select 1 to 500 particles.
4. Select gravity sensitivity from 1 to 10.
5. Press `START`, put the device down, and keep it still during calibration.
6. Touch the running simulation to return to setup.

Serial telemetry is emitted at 115200 baud approximately once per second.
