#ifndef GW_MALLOC_H
#define GW_MALLOC_H

#include <stdlib.h>
#include <string.h>

/**
 * Compatibility layer for Retro-Go (ESP32-S3)
 * These functions were originally for the Game & Watch port of FCEUMM.
 * On Retro-Go, we map them to standard silver heap allocations.
 */

static inline void *ahb_calloc(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}

static inline void *itc_calloc(size_t nmemb, size_t size) {
  return calloc(nmemb, size);
}

static inline void *ahb_malloc(size_t size) { return malloc(size); }

static inline void *itc_malloc(size_t size) { return malloc(size); }

static inline void itc_free(void *ptr) { free(ptr); }

static inline void ahb_free(void *ptr) { free(ptr); }

#endif
