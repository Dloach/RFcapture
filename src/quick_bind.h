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
// Page 0 is the unmodified grid.  Holding Fn / Aa / Opt shows pages 1 / 2 / 3,
// so a binding is addressed as (page, slot) and the 3x3 grid is multiplied by 4.
constexpr int kPages = 4;

struct Binding {
    uint8_t kind   = kNone;
    uint8_t index  = 0;  // SavedStore index (RF) or nfccards index (NFC)
    uint8_t icon   = 0;
    uint8_t color  = 0;
    // Stable identity of the bound record.  The index alone is not enough:
    // deleting an earlier card/signal shifts every later index, and the old
    // code then replayed whatever now sits at that slot.  The key is derived
    // from the card UID or the RF code/band (RAW: from the pulse train), so it
    // survives reordering.  Zero means "legacy binding written before keys".
    uint32_t key   = 0;
};

extern Binding slots[kPages][kSlots];

// ---------------------------------------------------------------------------
// Stable binding keys.  Pure functions so the host tests can exercise them.
// ---------------------------------------------------------------------------
inline uint32_t keyHashByte(uint32_t h, uint8_t b)
{
    h ^= b;
    h *= 16777619u;
    return h;
}

inline uint32_t keyForNfc(const uint8_t* uid, uint8_t len)
{
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < len; ++i) h = keyHashByte(h, uid[i]);
    return h ? h : 1u;
}

// `decoded` distinguishes a real code from a RAW entry.  A code of zero is a
// legal EV1527 word, so it must not be used as the "no code" sentinel.  `bits`
// is part of the identity: EV1527 (24 bit) and PT2262 (12 symbols) can carry
// the same numeric value on the same band, and they are different signals.
inline uint32_t keyForRf(uint8_t band, bool decoded, uint32_t code, uint8_t bits,
                         const uint16_t* pulses, int count)
{
    if (decoded) {
        uint32_t h = 2166136261u;
        h          = keyHashByte(h, band);
        h          = keyHashByte(h, bits);
        h          = keyHashByte(h, static_cast<uint8_t>(code & 0xFF));
        h          = keyHashByte(h, static_cast<uint8_t>((code >> 8) & 0xFF));
        h          = keyHashByte(h, static_cast<uint8_t>((code >> 16) & 0xFF));
        return 0x80000000u | (h & 0x7FFFFFFFu);
    }

    if (pulses == nullptr || count <= 0) return 1u;
    uint32_t h = 2166136261u;
    h          = keyHashByte(h, static_cast<uint8_t>(count & 0xFF));
    h          = keyHashByte(h, static_cast<uint8_t>((count >> 8) & 0xFF));
    for (int i = 0; i < count; ++i) {
        h = keyHashByte(h, static_cast<uint8_t>(pulses[i] & 0xFF));
        h = keyHashByte(h, static_cast<uint8_t>((pulses[i] >> 8) & 0xFF));
    }
    h &= 0x7FFFFFFFu;  // keep the decoded namespace separate
    return h ? h : 1u;
}

// RGB565 palette: green, red, blue, yellow, orange, purple, white, cyan.
extern const uint16_t kColors[8];
constexpr int kColorCount = 8;

void load();
bool save();
void clear(int page, int slot);
void set(int page, int slot, uint8_t kind, uint8_t index, uint8_t icon, uint8_t color,
         uint32_t key = 0);

// Display name of a grid page ("MAIN", "FN", "AA", "OPT").
const char* pageName(int page);

// Slot index for a keyboard code, or -1.  Letters match both cases.
int slotFor(uint8_t code);

// The printed key legend for a slot ("8".."L").
const char* keyLegend(int slot);

// One grid cell: key legend in the corner, the bound icon centred.  Drawn by
// the launcher home and by the bind picker's slot step.
template <typename G>
inline void drawSlotCell(G& g, int x, int y, int w, int h, int page, int slot, bool selected)
{
    const Binding& b = slots[page][slot];
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
