# แผนผังการปรับปรุงประสิทธิภาพ (Performance Optimization Roadmap)

เอกสารฉบับนี้สรุปแนวทางการรีดประสิทธิภาพสูงสุดสำหรับ Emulator ต่างๆ บนแฟลตฟอร์ม ESP32-S3 (Retro-Go) โดยเน้นไปที่การลดภาระ CPU และการบริหารจัดการหน่วยความจำ (RAM/PSRAM)

---

## 1. กลยุทธ์พื้นฐาน (General Strategies)

เพื่อให้ทุก Emu ทำงานได้ลื่นไหลที่สุด เราควรทำสิ่งเหล่านี้ให้เป็นมาตรฐาน:

- **การใช้ PSRAM (RG_ATTR_EXT_RAM):** ย้าย Buffer ขนาดใหญ่ (Video, Sound, ROM) ไปไว้ใน PSRAM เพื่อคืนพื้นที่ DRAM (Internal RAM) ให้กับระบบหลัก
- **16-bit Pipeline:** ลดขั้นตอนการแปลงสีจาก 8-bit เป็น 16-bit ใน Main Loop โดยพยายามให้ Core เขียนข้อมูลลง Surface ตรงๆ (ถ้าเป็นไปได้)
- **Compiler Flags:** ตรวจสอบว่าใช้ `-O3` และ `-ffast-math` ในจุดที่สำคัญ และหลีกเลี่ยงการใช้ `-mlongcalls` หากไม่จำเป็น
- **Dual Core Utilization:** แยกงานหนักๆ เช่น Audio Synthesis หรือ Disk I/O ออกไปไว้ที่ Core 1 (ESP32-S3 มี 2 คอร์)

---

## 2. แผนราย Core (Core-Specific Roadmap)

### 🎮 NES (FCEUMM) - *สถานะ: มีแผนแล้ว*
- **แผน:** ปิด `FCEU_LOW_RAM` เพื่อใช้ตาราง Lookup (O(1)) แทนระบบ linear search
- **ผลลัพธ์ที่คาดหวัง:** FPS เพิ่มขึ้น 3-5% และลดอาการหน่วงเมื่อมี Object เยอะๆ

### 🎮 SNES (Snes9x) - *สถานะ: มีแผนแล้ว*
- **แผน:** ย้าย Buffers (SubScreen/Z-Buffer) ไป PSRAM และปิด `InterpolatedSound`
- **เป้าหมาย:** พยายามลด Frameskip จาก 3 ให้เหลือ 1 หรือ 2 เพื่อความสมูท

### 🎮 Game Gear / Master System (SMSPlus)
- **การวิเคราะห์:** ใช้ทรัพยากรน้อย แต่สามารถทำให้เร็วขึ้นได้อีก
- **แผน:** ปรับปรุงระบบ **Dirty Palette** (อัปเดตพาเลตเฉพาะตอนที่มีการเปลี่ยนแปลง) เพื่อลดการอ่าน/เขียน Register ของ Video

### 🎮 Atari Lynx (Handy)
- **การวิเคราะห์:** Lynx เป็นเครื่องที่จำลองยากเนื่องจากมี Hardware Sprite Scaling
- **แผน:** ค้นหาจุดที่เป็น **Assembly Optimization** ใน Core โดยเฉพาะส่วนการวาด Sprite (Scanline rendering)

### 🎮 PC Engine (PCE-Go)
- **การวิเคราะห์:** มักจะมีปัญหาเมื่อเล่นเกมที่เป็น CD-ROM เนื่องจาก Buffer อ่านแผ่น
- **แผน:** ใช้ PSRAM สำหรับ CD-ROM Cache ขนาดใหญ่ (2MB+) เพื่อลดการรอ Disk I/O จาก SD Card

### 🎮 Game Boy / GBC (Gnuboy)
- **การวิเคราะห์:** ทำงานได้ดีอยู่แล้ว แต่อาจเพิ่มฟีเจอร์ **Fast Forward** ให้ลื่นขึ้น
- **แผน:** ใช้ระบบ Skip Audio Frame เมื่ออยู่ในโหมด Turbo เพื่อลดภาระการคำนวณเสียงที่ไม่จำเป็น

---

## 3. ขั้นตอนการดำเนินการ (Next Steps)

1. **Baseline Measurement:** บันทึก FPS และการใช้ RAM ของแต่ละ Core ในปัจจุบันก่อนเริ่มแก้
2. **Phase 1 (Memory):** ย้าย Buffer ทั่วไปของทุก Core ไป PSRAM เพื่อความเสถียรของระบบ
3. **Phase 2 (CPU):** เริ่มเจาะจงแก้ Logic หนักๆ (อย่างกรณี `LOW_RAM` ของ NES)
4. **Phase 3 (Display):** พัฒนาระบบ "Zero-copy" สำหรับการส่งภาพออกจอ

---
*จัดทำขึ้นเพื่อเป็นแนวทางสำหรับการพัฒนา Retro-Go Nano S3 ในอนาคต*
