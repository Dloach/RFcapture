#include "quick_bind.h"

#include <SPIFFS.h>

namespace quickbind {

Binding slots[kPages][kSlots];

const uint16_t kColors[8] = {0x07E0, 0xF800, 0x001F, 0xFFE0, 0xFD20, 0x801F, 0xFFFF, 0x07FF};

namespace {
constexpr char kPath[] = "/quick.txt";
constexpr char kKeys[kSlots] = {'8', '9', '0', 'I', 'O', 'P', 'J', 'K', 'L'};
constexpr const char* kLegends[kSlots] = {"8", "9", "0", "I", "O", "P", "J", "K", "L"};
}  // namespace

void load()
{
    for (auto& page : slots) {
        for (auto& b : page) b = Binding{};
    }
    if (!SPIFFS.begin(false) && !SPIFFS.begin(true)) return;
    if (!SPIFFS.exists(kPath)) return;
    File f = SPIFFS.open(kPath, FILE_READ);
    if (!f) return;
    char line[64];
    while (f.available() > 0) {
        size_t n = 0;
        while (f.available() > 0 && n + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[n++] = static_cast<char>(ch);
        }
        line[n] = '\0';

        int page = 0, s = -1, kind = 0, index = 0, icon = 0, color = 0;
        unsigned long key = 0;
        // New format: page;slot;kind;index;icon;color;key
        int fields = sscanf(line, "%d;%d;%d;%d;%d;%d;%lu", &page, &s, &kind, &index, &icon,
                            &color, &key);
        if (fields < 7) {
            // Old format: slot;kind;index;icon;color;key (or without key).
            page   = 0;
            s      = -1;
            key    = 0;
            fields = sscanf(line, "%d;%d;%d;%d;%d;%lu", &s, &kind, &index, &icon, &color, &key);
            if (fields < 5) continue;
        }
        if (page < 0 || page >= kPages || s < 0 || s >= kSlots || kind < kRf || kind > kNfc) {
            continue;
        }
        slots[page][s].kind  = static_cast<uint8_t>(kind);
        slots[page][s].index = static_cast<uint8_t>(index & 0xFF);
        slots[page][s].icon  = static_cast<uint8_t>(icon % kIconCount);
        slots[page][s].color = static_cast<uint8_t>(color % kColorCount);
        slots[page][s].key   = fields >= 6 ? static_cast<uint32_t>(key) : 0;
    }
    f.close();
}

bool save()
{
    if (!SPIFFS.begin(false)) return false;
    File f = SPIFFS.open(kPath, FILE_WRITE);
    if (!f) return false;
    for (int page = 0; page < kPages; ++page) {
        for (int i = 0; i < kSlots; ++i) {
            if (slots[page][i].kind == kNone) continue;
            f.printf("%d;%d;%u;%u;%u;%u;%lu\n", page, i,
                     static_cast<unsigned>(slots[page][i].kind),
                     static_cast<unsigned>(slots[page][i].index),
                     static_cast<unsigned>(slots[page][i].icon),
                     static_cast<unsigned>(slots[page][i].color),
                     static_cast<unsigned long>(slots[page][i].key));
        }
    }
    f.close();
    return true;
}

void clear(int page, int slot)
{
    if (page < 0 || page >= kPages || slot < 0 || slot >= kSlots) return;
    slots[page][slot] = Binding{};
    save();
}

void set(int page, int slot, uint8_t kind, uint8_t index, uint8_t icon, uint8_t color,
         uint32_t key)
{
    if (page < 0 || page >= kPages || slot < 0 || slot >= kSlots) return;
    slots[page][slot].kind  = kind;
    slots[page][slot].index = index;
    slots[page][slot].icon  = icon;
    slots[page][slot].color = color;
    slots[page][slot].key   = key;
    save();
}

const char* pageName(int page)
{
    switch (page) {
        case 0: return "MAIN";
        case 1: return "FN";
        case 2: return "AA";
        case 3: return "OPT";
        default: return "?";
    }
}

int slotFor(uint8_t code)
{
    for (int i = 0; i < kSlots; ++i) {
        if (code == static_cast<uint8_t>(kKeys[i])) return i;
        // Letters also match lowercase (0x20 folds 'I' onto 'i').
        if (kKeys[i] >= 'A' && code == static_cast<uint8_t>(kKeys[i] | 0x20)) return i;
    }
    return -1;
}

const char* keyLegend(int slot)
{
    return (slot >= 0 && slot < kSlots) ? kLegends[slot] : "?";
}

}  // namespace quickbind
