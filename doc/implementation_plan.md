# Future Optimization Roadmap: FCEUMM & SNES

> [!IMPORTANT]
> This is an analysis-only document for future implementation. No files have been modified.

## 1. FCEUMM Performance Analysis

### Current Bottleneck: Memory Handler Overhead
Currently, FCEUMM is built with `FCEU_LOW_RAM`. This saves ~128KB of RAM but forces the CPU to perform a **linear search** through a list of memory handlers for every read/write operation. On the ESP32-S3, this is a significant CPU cycle waste.

### Optimized Approach (Future)
- **Disable `FCEU_LOW_RAM`**: Switching to the full `ARead[0x10000]` and `BWrite[0x10000]` tables provides $O(1)$ (direct) memory access.
- **PSRAM Allocation**: To avoid exhausting internal DRAM, these tables must be decorated with `RG_ATTR_EXT_RAM` to live in PSRAM.
- **Benefits**: Estimated 3-7% increase in global FPS for NES emulation.

---

## 2. SNES (Snes9x) Performance Analysis

### Current Status
The SNES core is extremely demanding. It currently defaults to `frameskip = 3`, meaning only 1 out of 4 frames is rendered, and yet it still struggles with internal DRAM management.

### Optimization Opportunities
#### A. Memory Relief
- **GFX Buffers**: `SubScreen`, `ZBuffer`, and `SubZBuffer` are currently in internal RAM. Moving these to PSRAM via `rg_alloc(..., RG_ALLOC_EXTERNAL)` will free up critical internal memory.
- **ROM & RAM Pools**: Large allocations for `Memory.ROM` and `Memory.RAM` should also be strictly offloaded to PSRAM.

#### B. CPU Efficiency
- **Sound Interpolation**: Settings.InterpolatedSound is on by default. Turning this OFF will reduce CPU load during audio mixing.
- **Wait States**: Adjusting internal timings and sync points to better handle the ESP32-S3's dual-core vs the Snes9x single-threaded model.

---

## 3. Implementation Checklist (For Later)
- [ ] Mod `components/fceumm/CMakeLists.txt`: Remove `-DFCEU_LOW_RAM` and `-DTARGET_GNW`.
- [ ] Mod `components/fceumm/src/fceu.c`: Add `RG_ATTR_EXT_RAM` to handlers.
- [ ] Mod `main_snes.c`: Relocate display buffers and disable sound interpolation.
- [ ] Mod `memmap.c`: Use PSRAM for all major core structures.
