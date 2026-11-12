#pragma once

// One-key quick actions bound to the launcher home grid (8 9 0 / I O P / J K L).
// Each slot points at a saved RF signal or an NFC card, plus the icon and color
// the operator picked for it.  Persisted on SPIFFS (/quick.txt), never NVS
// (docs/DEV-STATE.md section 12.1).

#include <Arduino.h>

#include "icons.h"

namespace quickbind {

enum Kind : uint8_t { kNone = 0, kRf = 1, kNfc = 2 };
constexpr int kSlots = 9;

struct Binding {
    uint8_t kind   = kNone;
    uint8_t index  = 0;  // SavedStore index (RF) or nfccards index (NFC)
    uint8_t icon   = 0;
    uint8_t color  = 0;
};

extern Binding slots[kSlots];

// RGB565 palette: green, red, blue, yellow, orange, purple, white, cyan.
extern const uint16_t kColors[8];
constexpr int kColorCount = 8;

void load();
bool save();
void clear(int slot);
void set(int slot, uint8_t kind, uint8_t index, uint8_t icon, uint8_t color);

// Slot index for a keyboard code, or -1.  Letters match both cases.
int slotFor(uint8_t code);

// The printed key legend for a slot ("8".."L").
const char* keyLegend(int slot);

// One grid cell: key legend in the corner, the bound icon centred.  Drawn by
// the launcher home and by the bind picker's slot step.
template <typename G>
inline void drawSlotCell(G& g, int x, int y, int w, int h, int slot, bool selected)
{
    const Binding& b = slots[slot];
    if (selected) g.fillRect(x, y, w, h, 0x2104);
    g.drawRect(x, y, w, h, selected ? 0xFD20 : 0x4208);
    g.setTextSize(1);
    g.setTextColor(b.kind == kNone ? 0x4208 : 0x8410, selected ? 0x2104 : 0x0000);
    g.drawString(keyLegend(slot), x + 2, y + 2);
    if (b.kind != kNone) {
        drawIcon(g, x + w / 2, y + h / 2 + 3, 8, b.icon, kColors[b.color]);
    }
}

}  // namespace quickbind
