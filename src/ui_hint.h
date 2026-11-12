#pragma once

// Small filled up/down triangles for footer hints, replacing the "; ." keycap
// text: the arrows printed on the keycaps are what the operator looks for, and
// two glyphs read as direction at a glance in a way "; ." never did.
//
// Templated on the draw target so the panel and a M5Canvas both work (both are
// LovyanGFX).  Draws 16 px wide, 8 px tall -- the size of one font-1 text row.

template <typename G>
inline void drawMoveTriangles(G& g, int x, int y, uint16_t color)
{
    g.fillTriangle(x, y + 7, x + 3, y, x + 6, y + 7, color);          // up
    g.fillTriangle(x + 9, y, x + 12, y + 7, x + 15, y + 0, color);    // down
}

// Width to advance the x cursor after drawMoveTriangles() (16 px + spacing).
constexpr int kMoveTrianglesW = 18;
