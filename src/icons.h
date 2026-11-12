#pragma once

// Icon set for the quick-bind grid.  Everything is drawn with primitives so no
// font is involved; add a case to extend the set.  Templated on the draw target
// so the panel and a M5Canvas both work (both are LovyanGFX).
//
//   0 ▲ up      1 ▼ down    2 ◀ left    3 ▶ right
//   4 ■ square  5 ● circle  6 door      7 ⚡ bolt

#include <stdint.h>

constexpr int kIconCount = 8;

template <typename G>
inline void drawIcon(G& g, int cx, int cy, int r, uint8_t icon, uint16_t color)
{
    switch (icon) {
        case 0: g.fillTriangle(cx - r, cy + r - 1, cx, cy - r, cx + r, cy + r - 1, color); break;
        case 1: g.fillTriangle(cx - r, cy - r + 1, cx + r, cy - r + 1, cx, cy + r, color); break;
        case 2: g.fillTriangle(cx - r, cy, cx + r - 1, cy - r, cx + r - 1, cy + r, color); break;
        case 3: g.fillTriangle(cx - r + 1, cy - r, cx - r + 1, cy + r, cx + r, cy, color); break;
        case 4: g.fillRect(cx - r, cy - r, r * 2, r * 2, color); break;
        case 5: g.fillCircle(cx, cy, r, color); break;
        case 6:  // door: a panel with a dark knob
            g.fillRect(cx - r + 1, cy - r, r * 2 - 2, r * 2 + 1, color);
            g.fillCircle(cx + r / 2, cy, 1, 0x0000);
            break;
        case 7:  // bolt: two offset triangles
            g.fillTriangle(cx - 1, cy - r, cx + r, cy - r, cx + 1, cy, color);
            g.fillTriangle(cx - 1, cy, cx - r, cy + r, cx + 1, cy + r, color);
            break;
        default: g.fillCircle(cx, cy, 2, color); break;
    }
}
