#include "quick_bind.h"

#include <SPIFFS.h>

namespace quickbind {

Binding slots[kSlots];

const uint16_t kColors[8] = {0x07E0, 0xF800, 0x001F, 0xFFE0, 0xFD20, 0x801F, 0xFFFF, 0x07FF};

namespace {
constexpr char kPath[] = "/quick.txt";
constexpr char kKeys[kSlots] = {'8', '9', '0', 'I', 'O', 'P', 'J', 'K', 'L'};
constexpr const char* kLegends[kSlots] = {"8", "9", "0", "I", "O", "P", "J", "K", "L"};
}  // namespace

void load()
{
    for (auto& b : slots) b = Binding{};
    if (!SPIFFS.begin(false) && !SPIFFS.begin(true)) return;
    if (!SPIFFS.exists(kPath)) return;
    File f = SPIFFS.open(kPath, FILE_READ);
    if (!f) return;
    char line[48];
    while (f.available() > 0) {
        size_t n = 0;
        while (f.available() > 0 && n + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[n++] = static_cast<char>(ch);
        }
        line[n] = '\0';
        int s = -1, kind = 0, index = 0, icon = 0, color = 0;
        unsigned long key = 0;
        const int fields =
            sscanf(line, "%d;%d;%d;%d;%d;%lu", &s, &kind, &index, &icon, &color, &key);
        if (fields < 5) continue;
        if (s < 0 || s >= kSlots || kind < kRf || kind > kNfc) continue;
        slots[s].kind  = static_cast<uint8_t>(kind);
        slots[s].index = static_cast<uint8_t>(index & 0xFF);
        slots[s].icon  = static_cast<uint8_t>(icon % kIconCount);
        slots[s].color = static_cast<uint8_t>(color % kColorCount);
        slots[s].key   = fields >= 6 ? static_cast<uint32_t>(key) : 0;
    }
    f.close();
}

bool save()
{
    if (!SPIFFS.begin(false)) return false;
    File f = SPIFFS.open(kPath, FILE_WRITE);
    if (!f) return false;
    for (int i = 0; i < kSlots; ++i) {
        if (slots[i].kind == kNone) continue;
        f.printf("%d;%u;%u;%u;%u;%lu\n", i, static_cast<unsigned>(slots[i].kind),
                 static_cast<unsigned>(slots[i].index), static_cast<unsigned>(slots[i].icon),
                 static_cast<unsigned>(slots[i].color),
                 static_cast<unsigned long>(slots[i].key));
    }
    f.close();
    return true;
}

void clear(int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    slots[slot] = Binding{};
    save();
}

void set(int slot, uint8_t kind, uint8_t index, uint8_t icon, uint8_t color, uint32_t key)
{
    if (slot < 0 || slot >= kSlots) return;
    slots[slot].kind  = kind;
    slots[slot].index = index;
    slots[slot].icon  = icon;
    slots[slot].color = color;
    slots[slot].key   = key;
    save();
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
