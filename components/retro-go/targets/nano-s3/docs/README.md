# Retro-Go Nano-S3 Custom Build

Custom build of Retro-Go for ESP32 Nano-S3 (ESP32-S3 + PSRAM).

This repository contains a modified target configuration for Nano-S3
with custom GPIO mapping and hardware wiring.

Based on upstream Retro-Go project.

------------------------------------------------------------------------

## Hardware

Board: Waveshare ESP32-S3-Nano Development Board\
Chip: ESP32-S3\
PSRAM: Required\
Flash: On-board QSPI

Display: SPI TFT (ST7789)\
Audio: I2S DAC (MAX98357A)

------------------------------------------------------------------------

## GPIO Mapping

### TFT ST7789 (SPI)

  Signal   GPIO
  -------- --------
  GND      GND
  VCC      3V3
  SCLK     D13
  SDA      D11
  RES      RST
  DC       D10
  CS       D2
  BLK      A7

------------------------------------------------------------------------

### SD Card

  Signal   GPIO
  -------- --------
  3V3      3V3
  CS       D3
  MOSI     D11
  CLK      D13
  MISO     D12
  GND      GND

------------------------------------------------------------------------

### Buttons

  Button   GPIO
  -------- -------
  UP       D6
  DOWN     A1
  LEFT     A2
  RIGHT    A6
  A        D4
  B        D5
  START    D7
  SELECT   D8
  MENU     D9
  VOL      3V3 - R10k - B0 - Switch - GND

------------------------------------------------------------------------

### Audio (MAX98357A - I2S)

  Signal   GPIO
  -------- --------
  LRC      A5
  BCLK     A3
  DIN      A4

### Battery Voltage Divider

BAT+ --[100k]--+-- GPIO1
               |
             [100k]
               |
              GND


------------------------------------------------------------------------

## Build Instructions

This project must be built using rg_tool.py.
Do not run idf.py directly from the component directory.

### Build firmware

python rg_tool.py --target nano-s3 build

### Build image

python rg_tool.py --target nano-s3 --port COMx build-img

### Flash to device

python rg_tool.py --target nano-s3 --port COMx flash launcher


### Full rebuild (if needed)

python rg_tool.py --target nano-s3 clean
python rg_tool.py --target nano-s3 build




------------------------------------------------------------------------

## Target

Custom target located at:

components/retro-go/targets/nano-s3/

------------------------------------------------------------------------

## Notes

-   Requires PSRAM enabled
-   Designed for 300x240 ST7789 display
-   Optimized for Nano-S3 form factor

------------------------------------------------------------------------

## Upstream

Original project: https://github.com/ducalex/retro-go
