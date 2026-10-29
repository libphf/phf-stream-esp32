#ifndef PHF_ESP32_MEM_H
#define PHF_ESP32_MEM_H

#include <stdlib.h>
#include <stddef.h>

#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void* esp32_ps_malloc(size_t sz) {
    void* ptr = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = malloc(sz);
    return ptr;
}

static inline void* esp32_ps_calloc(size_t n, size_t sz) {
    void* ptr = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(n, sz);
    return ptr;
}

static inline void* esp32_ps_realloc(void* p, size_t sz) {
    void* ptr = heap_caps_realloc(p, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = realloc(p, sz);
    return ptr;
}

#ifdef __cplusplus
}
#endif

#define malloc(s)      esp32_ps_malloc(s)
#define calloc(n, s)   esp32_ps_calloc(n, s)
#define realloc(p, s)  esp32_ps_realloc(p, s)

#endif // ESP32

#endif // PHF_ESP32_MEM_H
