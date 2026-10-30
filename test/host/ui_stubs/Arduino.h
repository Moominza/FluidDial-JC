// Host stub for the UI renderer (test/host/ui_render.cpp). Provides only what the JC pages use.
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pgmspace.h"

extern uint32_t g_stub_millis;     // the harness controls time
extern bool g_stub_serial_echo;    // print Serial output (off by default)
inline uint32_t millis() { return g_stub_millis; }

struct StubSerial {
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!g_stub_serial_echo) return;
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    void println(const char* s) {
        if (g_stub_serial_echo) puts(s);
    }
};
extern StubSerial Serial;

inline size_t strlcpy(char* dst, const char* src, size_t size) {
    const size_t n = strlen(src);
    if (size > 0) {
        const size_t c = n >= size ? size - 1 : n;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}
