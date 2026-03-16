# GBSP GBA Emulator Optimization Plan

## วิเคราะห์ประสิทธิภาพปัจจุบัน (Current Performance Analysis)

### 🔍 ปัญหาหลักที่พบ:
1. **CPU Execution Bottleneck** - `execute_arm()` เป็น main loop ที่ใช้เวลามากที่สุด
2. **Video Rendering Overhead** - การ render scanline ทีละบรรทัดใน `video.cpp`
3. **Audio Processing** - การคำนวณ audio samples ทุก frame
4. **Memory Access Pattern** - การเข้าถึง VRAM และ memory registers

### 📊 สถานะปัจจุบัน:
- **Optimization Level**: `-O3` (สูงสุดแล้ว)
- **Overclock**: `OVERCLOCK_60FPS` ถูกเปิดอยู่
- **Frameskip**: สามารถปรับได้ 0-3 (60fps ถึง 15fps)
- **Clock Rate**: 16MHz (standard) หรือ overclock สูงสุด

---

## 🚀 แผนการปรับปรุงประสิทธิภาพ (Optimization Plan)

### **ระดับ 1: การปรับแต่งที่ง่ายและปลอดภัย (Easy & Safe)**

#### 1.1 ปรับ Clock Rate และ FPS
```c
// ใน sound.h
#define OVERCLOCK_60FPS  // อยู่แล้ว ✓
#define GBC_BASE_RATE ((float)(18 * 1024 * 1024))  // เพิ่มจาก 16MHz เป็น 18MHz
```

#### 1.2 ปรับ Buffer Sizes
```c
// ใน CMakeLists.txt
-DROM_BUFFER_SIZE=16  // เพิ่มจาก 8 เป็น 16
```

#### 1.3 ปรับ Frameskip Default
```c
// ใน main.c
app->frameskip = rg_settings_get_number(NS_APP, "Frameskip", 1); // เปลี่ยนจาก 0 เป็น 1
```

---

### **ระดับ 2: การปรับแต่งปานกลาง (Medium Impact)**

#### 2.1 ปรับ Video Rendering Optimization
- **Skip Empty Scanlines**: ตรวจสอบและข้าม scanline ว่าง
- **Reduce Color Depth**: ใช้ 16-bit color แทน 32-bit ในการคำนวณ
- **Cache Palette Lookups**: จัดเก็บ palette ที่ใช้บ่อย

#### 2.2 Audio Buffer Optimization
- **Increase Buffer Size**: ลดการคำนวณ audio บ่อยๆ
- **Precompute Audio Tables**: จัดเก็บค่าที่คำนวณซ้ำๆ

#### 2.3 CPU Execution Tuning
- **Dynamic Cycle Adjustment**: ปรับ execute_cycles ตามเกม
- **Skip Idle Loops**: ตรวจจับและข้าม idle loops

---

### **ระดับ 3: การปรับแต่งขั้นสูง (Advanced)**

#### 3.1 Memory Optimization
- **Align Memory Access**: จัดเรียง memory ให้ aligned
- **Reduce Memory Copies**: ใช้ pointer แทนการ copy
- **Optimize VRAM Access**: จัดการ VRAM access pattern

#### 3.2 Video Pipeline Optimization
- **Parallel Processing**: ใช้ dual core สำหรับ video rendering
- **Hardware Acceleration**: ใช้ ESP32-S3 hardware features
- **Sprite Culling**: ข้าม sprites ที่ไม่แสดง

#### 3.3 CPU Emulation Optimization
- **JIT Compilation**: ใช้ dynamic recompilation
- **Instruction Caching**: จัดเก็บการแปลคำสั่งที่ใช้บ่อย
- **Branch Prediction**: ทำนายการกระโดด branch

---

## 🛠️ ขั้นตอนการดำเนินงาน (Implementation Steps)

### **Phase 1: Quick Wins (1-2 วัน)**
1. ✅ วิเคราะห์ปัญหาเบื้องต้น
2. 🔄 ปรับ clock rate เป็น 18MHz
3. 🔄 เพิ่ม ROM buffer size
4. 🔄 ปรับ default frameskip

### **Phase 2: Medium Optimizations (3-5 วัน)**
1. 🔄 Optimize video rendering
2. 🔄 Improve audio processing
3. 🔄 Tune CPU execution parameters
4. 🔄 Add performance monitoring

### **Phase 3: Advanced Optimizations (1-2 สัปดาห์)**
1. ⏳ Memory alignment optimization
2. ⏳ Video pipeline improvements
3. ⏳ CPU emulation enhancements
4. ⏳ Hardware acceleration features

---

## 📈 การวัดผล (Performance Metrics)

### **Key Performance Indicators:**
- **Frame Rate**: เป้าหมาย 60fps (stable)
- **Frame Time**: เป้าหมาย <16.67ms per frame
- **CPU Usage**: เป้าหมาย <80% ของ ESP32-S3
- **Memory Usage**: เป้าหมาย <80% ของ available RAM
- **Audio Latency**: เป้าหมาย <50ms

### **Tools for Measurement:**
- Built-in performance counters
- Frame time logging
- Memory usage tracking
- Audio buffer monitoring

---

## ⚠️ ข้อควรระวัง (Important Considerations)

### **Compatibility Issues:**
- บางเกมอาจมีปัญหากับ clock rate สูงเกินไป
- Frameskip อาจทำให้เกมบางเกมเล่นยากขึ้น
- Audio sync อาจมีปัญหากับการปรับ buffer

### **Hardware Limitations:**
- ESP32-S3 มี RAM จำกัด
- SPI bandwidth จำกัดสำหรับ display
- Dual core ต้องจัดการอย่างระมัดระวัง

### **Testing Requirements:**
- ต้องทดสอบบน hardware จริงทุกครั้ง
- ต้องทดสอบกับเกมหลากหลายประเภท
- ต้องตรวจสอบความเสถียรระยะยาว

---

## 🎯 ผลลัพธ์ที่คาดหวัง (Expected Results)

### **After Phase 1:**
- FPS เพิ่มขึ้น 10-20%
- ลด frame drops ประมาณ 30%
- เกมส่วนใหญ่ทำงานได้ลื่นไหลขึ้น

### **After Phase 2:**
- FPS เพิ่มขึ้น 20-40% จากเดิม
- ลด audio stuttering
- ปรับปรุงความตอบสนองของ controls

### **After Phase 3:**
- FPS เพิ่มขึ้น 40-60% จากเดิม
- รองรับเกมที่ซับซ้อนมากขึ้น
- ประหยัดพลังงานมากขึ้น

---

## 📝 บันทึกการเปลี่ยนแปลง (Change Log)

### **Version 1.0 (Current):**
- Base optimization with -O3
- OVERCLOCK_60FPS enabled
- Basic frameskip support

### **Version 1.1 (Planned):**
- Increased clock rate to 18MHz
- Larger ROM buffer
- Optimized default settings

### **Version 1.2 (Future):**
- Video rendering optimizations
- Audio improvements
- CPU tuning

### **Version 2.0 (Future):**
- Advanced memory optimizations
- Hardware acceleration
- JIT compilation (if feasible)

---

**หมายเหตุ**: ทุกการเปลี่ยนแปลงต้องทดสอบบน hardware จริงก่อน deploy
