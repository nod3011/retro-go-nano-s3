# Retro-Go Nano-S3 Project Brief

## ภาพรวมโปรเจค (Project Overview)
โปรเจค Retro-Go เป็น firmware สำหรับเล่นเกม retro บนอุปกรณ์ ESP32-based โดยเฉพาะ hardware ESP32-S3 ที่พัฒนาขึ้นมาเพื่อใช้งานบนเครื่องเกม emulator

## เป้าหมายหลัก (Main Targets)
- **nano-s3**: จอแสดงผล 300x240 pixels
- **nano-s3-sq**: จอแสดงผล 240x240 pixels (square)
- **nano-s3-lcd**: รุ่นที่ใช้ LCD แบบพิเศษ

## โครงสร้างโปรเจค (Project Structure)

### โฟลเดอร์สำคัญ
- `components/retro-go/targets/`: คอนฟิก hardware สำหรับแต่ละ target
- `REF/`: เอกสารอ้างอิงและข้อมูลทางเทคนิค
- `rg_tool.py`: เครื่องมือสำหรับ build โปรเจค (ไม่สามารถ build ตรงจาก ESP-IDF ได้)

### Emulators ที่รองรับ
- **Nintendo**: NES, SNES (ช้า), Gameboy, Gameboy Color, Game & Watch
- **Sega**: SG-1000, Master System, Mega Drive/Genesis, Game Gear
- **Coleco**: Colecovision
- **NEC**: PC Engine
- **Atari**: Lynx
- **Others**: DOOM (รวม mods)

## Hardware Specifications

### จอแสดงผล (Display)
- **nano-s3**: ILI9341, 300x240, SPI2_HOST, 40MHz
- **nano-s3-sq**: ILI9341, 240x240, SPI2_HOST, 40MHz, ST7789_240X240

### ปุ่มกด (Gamepad)
- ปุ่มทิศทาง: UP (GPIO9), DOWN (GPIO2), LEFT (GPIO3), RIGHT (GPIO13)
- ปุ่มควบคุม: SELECT (GPIO17), START (GPIO10), MENU (GPIO18), OPTION (GPIO46)
- ปุ่มแอคชัน: A (GPIO7), B (GPIO8)

### พอร์ตการเชื่อมต่อ
- **SPI Display**: MISO (GPIO47), MOSI (GPIO38), CLK (GPIO48), CS (GPIO5), DC (GPIO21), BCKL (GPIO14)
- **SPI SD Card**: MISO (GPIO47), MOSI (GPIO38), CLK (GPIO48), CS (GPIO6)
- **I2S DAC**: BCK (GPIO4), WS (GPIO12), DATA (GPIO11)
- **Battery**: ADC1_CH0, GPIO43 (Status LED)

## การ Build และทดสอบ

### คำสั่ง Build
```bash
# Build ทั้งหมด
python rg_tool.py build-fw

# Build เฉพาะ emulator บางตัว
python rg_tool.py build-fw launcher retro-core gnuboy

# Build สำหรับ target ที่กำหนด
python rg_tool.py --target nano-s3 build-fw
```

### ข้อควรระวัง
- **ต้อง build ผ่าน rg_tool.py เท่านั้น** - ไม่สามารถ build ตรงจาก ESP-IDF
- **ทุกครั้งที่ทดสอบ emulator** - อาจต้อง build launcher ด้วยเพื่อการทดสอบที่สมบูรณ์
- **การทดสอบบน hardware** - ต้องแจ้งผู้ใช้ก่อนเสมอเนื่องจากต้องทดสอบบนอุปกรณ์จริง

## การแก้ไขปัญหา
- ปัญหาการ build: ต้องแก้ไขที่ `rg_tool.py`
- ปัญหา hardware: ตรวจสอบ `config.h` ในแต่ละ target
- การเปลี่ยนแปลงใน target ใดๆ: ต้องอัปเดตทุก target (nano-s3, nano-s3-sq, nano-s3-lcd)

## Environment Variables
- `RG_TOOL_TARGET`: กำหนด target (default: odroid-go)
- `RG_TOOL_BAUD`: ความเร็วในการ flash (default: 1152000)
- `RG_TOOL_PORT`: พอร์ต serial (default: COM3)
- `RG_TOOL_APPS`: รายการ apps ที่ต้องการ build
- `PROJECT_NAME`: ชื่อโปรเจค (default: Retro-Go)
- `PROJECT_ICON`: ไอคอนโปรเจค (default: assets/icon.raw)

## ข้อมูลอ้างอิง
- เอกสารประกอบ: `REF/` folder
- คู่มือการ build: `BUILDING.md`
- คู่มือการพอร์ต: `PORTING.md`
- License: GPLv2 (มีข้อยกเว้นสำหรับ fmsx และ handy-go)

---
**หมายเหตุ**: โปรเจคนี้เป็นเวอร์ชันพัฒนาเฉพาะสำหรับ ESP32-S3 และ target nano-s3 series
