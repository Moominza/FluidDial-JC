// Host stub of the one display class the JC pages draw on. It renders into an RGB565
// framebuffer and records what was drawn so the harness can check for clipping and overlap.
#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>

#define BLACK 0x0000
#define NAVY 0x000F
#define DARKGREEN 0x03E0
#define DARKCYAN 0x03EF
#define MAROON 0x7800
#define PURPLE 0x780F
#define OLIVE 0x7BE0
#define LIGHTGREY 0xC618
#define DARKGREY 0x7BEF
#define BLUE 0x001F
#define GREEN 0x07E0
#define CYAN 0x07FF
#define RED 0xF800
#define MAGENTA 0xF81F
#define YELLOW 0xFFE0
#define WHITE 0xFFFF
#define ORANGE 0xFD20

struct StubGlyph { int16_t x, y, w, h; uint32_t run; uint32_t ink; uint32_t overlapped; };
struct StubFill { int16_t x, y, w, h; uint16_t color; size_t glyphs_before; uint32_t obscured_ink; };

class Arduino_GFX {
public:
    static constexpr int W = 800;
    static constexpr int H = 480;

    Arduino_GFX() : fb(W * H, 0), owner(W * H, 0) {}

    void fillScreen(uint16_t c) {
        std::fill(fb.begin(), fb.end(), c);
        std::fill(owner.begin(), owner.end(), 0u);
        glyphs.clear();
        fills.clear();
        oob = 0;
        run_id = 0;
        have_last = false;
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
        uint32_t obscured = 0;
        for (int yy = y; yy < y + h; ++yy) {
            for (int xx = x; xx < x + w; ++xx) {
                if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                if (owner[yy * W + xx] != 0) ++obscured;
                fb[yy * W + xx] = c;
                owner[yy * W + xx] = 0;
            }
        }
        fills.push_back({ x, y, w, h, c, glyphs.size(), obscured });
    }
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
        for (int xx = x; xx < x + w; ++xx) { put(xx, y, c); put(xx, y + h - 1, c); }
        for (int yy = y; yy < y + h; ++yy) { put(x, yy, c); put(x + w - 1, yy, c); }
    }
    // One glyph. A new text run starts when x moves back or the vertical position jumps.
    void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w, int16_t h) {
        if (!have_last || x < last_x - 6 || (y > last_y + 30) || (y < last_y - 30)) ++run_id;
        have_last = true;
        last_x = x;
        last_y = y;
        uint32_t ink = 0, overlapped = 0;
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) {
                const int xx = x + i, yy = y + j;
                if (xx < 0 || yy < 0 || xx >= W || yy >= H) { ++oob; continue; }
                const uint16_t v = bitmap[j * w + i];
                const size_t k = static_cast<size_t>(yy) * W + xx;
                if (v != fb[k]) {                       // this pixel carries ink
                    ++ink;
                    if (owner[k] != 0 && owner[k] != run_id) ++overlapped;   // lands on another text's ink
                    owner[k] = run_id;
                }
                fb[k] = v;
            }
        }
        glyphs.push_back({ x, y, w, h, run_id, ink, overlapped });
    }

    std::vector<uint16_t> fb;
    std::vector<uint32_t> owner;      // which text run last put ink on a pixel (0 = none)
    std::vector<StubGlyph> glyphs;    // since the last fillScreen
    std::vector<StubFill> fills;
    uint32_t oob = 0;                 // ink pixels that fell outside the screen
    uint32_t run_id = 0;

private:
    void put(int xx, int yy, uint16_t c) {
        if (xx < 0 || yy < 0 || xx >= W || yy >= H) return;
        fb[yy * W + xx] = c;
        owner[yy * W + xx] = 0;
    }
    bool have_last = false;
    int16_t last_x = 0, last_y = 0;
};
