// Host stub: flash-resident data is ordinary memory on the host.
#pragma once
#include <stdint.h>
#include <string.h>
#define PROGMEM
#define pgm_read_byte(addr) (*reinterpret_cast<const uint8_t*>(addr))
#define memcpy_P(dst, src, n) memcpy((dst), (src), (n))
