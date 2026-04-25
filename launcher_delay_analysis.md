# Analysis of Launcher Delay (Stutter) on Nano S3

## Root Cause
The significant lag (approx. 1 second) experienced when switching tabs in the Launcher for the first time was caused by forcing the **LodePNG decoder** to use **PSRAM (External RAM)** for its internal work buffers.

### Technical Details
1. **Memory Latency:** The ESP32-S3 accesses PSRAM via the SPI/OPI bus. While PSRAM provides a large capacity (8MB), it has much higher latency compared to Internal RAM (IRAM).
2. **Decoding Bottleneck:** PNG decoding is a CPU-intensive process involving thousands of small memory accesses for Huffman trees, bit-stream processing, and LZ77 decompression. 
3. **The Regression:** In commit `aa169397`, the `lodepng_malloc` function was overridden to use `MEM_SLOW`. This forced every single bit-manipulation and tree-traversal in the decoder to go through the slower SPI bus.
4. **Shared Bus Contention:** On the Nano S3, the LCD and SD Card share the same SPI host (`SPI2`). Continuous PSRAM access during decoding competed for bus priority, further exacerbating the UI freeze.

## Why it appeared as a "First-Round" delay
The Launcher in `gui.c` caches decoded background surfaces in memory after they are first loaded. 
- **First Visit:** The PNG must be decoded. If decoding is slow, the UI freezes.
- **Subsequent Visits:** The already-decoded surface is drawn directly from memory, so no lag is felt.

## Solution
We have reverted the LodePNG memory allocation logic to use the standard system `malloc`. 
- **Internal RAM Usage:** Small-to-medium allocations (below 32KB) now automatically stay in fast Internal RAM (as per `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`).
- **Performance:** This allows the decoder to run at full speed, restoring the smooth 60FPS feel when switching tabs.
- **Stability:** For very large images that might exceed Internal RAM, the system will still automatically fall back to PSRAM, preventing Crashes/OOM while maintaining speed for standard UI assets.

## Conclusion
For performance-critical tasks like image decoding on the ESP32-S3, keeping active work buffers in **Internal RAM** is essential. PSRAM should be reserved for large, static data structures or final framebuffers where latency is less of a factor than capacity.
