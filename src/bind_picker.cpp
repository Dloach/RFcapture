// Modal quick-bind editor: slot -> icon -> color.
//
// Runs its own loop on top of whichever feature called it (RF send page or NFC
// card list); both have M5Cardputer up with the polling keyboard reader, so the
// picker just borrows them and hands back on esc/ENTER.

#include "bind_picker.h"

#include <M5Cardputer.h>
#include <esp_task_wdt.h>

#include "quick_bind.h"
#include "ui_hint.h"

namespace bindpick {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kHead   = 0x18E3;

auto& lcd = M5Cardputer.Display;

enum Act : uint32_t {
    kUp = 1u << 0, kDown = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3,
    kEnter = 1u << 4, kBack = 1u << 5, kDel = 1u << 6,
};

uint32_t readActs()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            case ';': case 'k': case 'K': a |= kUp; break;
            case '.': case 'j': case 'J': a |= kDown; break;
            case ',': case 'h': case 'H': a |= kLeft; break;
            case '/': case 'l': case 'L': a |= kRight; break;
            case KEY_ENTER: a |= kEnter; break;
            case '`': case '~': case 0x1B: case KEY_BACKSPACE: a |= kBack; break;
            case 'd': case 'D': a |= kDel; break;
            default: break;
        }
    }
    return a;
}

void header(const char* title, const char* what)
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(title, 3, 2);
    lcd.setTextColor(kDim, kHead);
    lcd.drawRightString(what, lcd.width() - 3, 2);
}

void footer(const char* extra)
{
    lcd.drawFastHLine(0, 118, lcd.width(), kDim);
    drawMoveTriangles(lcd, 4, 122, kOk);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("move", 24, 122);
    lcd.setTextColor(kOk, kBg);
    lcd.drawString("ENT", 62, 122);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("ok", 84, 122);
    lcd.setTextColor(kOk, kBg);
    lcd.drawString("esc", 118, 122);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("cancel", 140, 122);
    if (extra) {
        lcd.setTextColor(kOk, kBg);
        lcd.drawString(extra, 190, 122);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("unbind", 196 + lcd.textWidth(extra), 122);
    }
}

void drawSlots(int page, int sel, const char* what)
{
    lcd.fillScreen(kBg);
    char title[24];
    snprintf(title, sizeof(title), "BIND %s", quickbind::pageName(page));
    header(title, what);
    for (int s = 0; s < quickbind::kSlots; ++s) {
        const int gx = 24 + (s % 3) * 66;
        const int gy = 20 + (s / 3) * 31;
        quickbind::drawSlotCell(lcd, gx, gy, 62, 29, page, s, s == sel);
    }
    footer("D");
}

void drawIcons(int sel, const char* what)
{
    lcd.fillScreen(kBg);
    header("PICK ICON", what);
    for (int i = 0; i < kIconCount; ++i) {
        const int gx = 20 + (i % 4) * 54;
        const int gy = 30 + (i / 4) * 42;
        if (i == sel) {
            lcd.fillRect(gx - 2, gy - 2, 44, 40, 0x2104);
            lcd.drawRect(gx - 2, gy - 2, 44, 40, kAccent);
        } else {
            lcd.drawRect(gx - 2, gy - 2, 44, 40, 0x4208);
        }
        drawIcon(lcd, gx + 20, gy + 18, 10, static_cast<uint8_t>(i), kText);
    }
    footer(nullptr);
}

void drawColors(int sel, const char* what)
{
    lcd.fillScreen(kBg);
    header("PICK COLOR", what);
    for (int i = 0; i < quickbind::kColorCount; ++i) {
        const int gx = 16 + i * 28;
        const int gy = 56;
        lcd.fillRect(gx, gy, 22, 22, quickbind::kColors[i]);
        lcd.drawRect(gx - 2, gy - 2, 26, 26, i == sel ? kAccent : 0x4208);
    }
    footer(nullptr);
}

}  // namespace

bool run(uint8_t kind, uint8_t index, const char* what, uint32_t key)
{
    // The slot table is only loaded by the launcher's menuSetup; an RF or NFC
    // feature boot never runs that, so without this reload the picker would
    // start from an all-empty table and set()/clear() would wipe every binding
    // made in other boots.
    quickbind::load();

    int slot  = 0;
    int icon  = 0;
    int color = 0;
    int page  = 0;
    int step  = 0;
    uint32_t prev = 0;

    // Preselect the first free slot on MAIN.  Switching page with a modifier
    // keeps the slot index, so the same key is preselected on the new page.
    for (int i = 0; i < quickbind::kSlots; ++i) {
        if (quickbind::slots[0][i].kind == quickbind::kNone) {
            slot = i;
            break;
        }
    }

    if (step == 0) drawSlots(page, slot, what);

    for (;;) {
        M5Cardputer.update();
        // The RF app arms a 6 s task watchdog at begin(); a modal picker that
        // never feeds it dies mid-edit with a panic reboot.  Harmless where no
        // watchdog is subscribed (the NFC feature), so feed it unconditionally.
        esp_task_wdt_reset();

        // On the slot step, Fn / Aa / Opt choose the binding page.  This is
        // checked before the "no new key" early-out: the modifier itself is
        // ignored by readActs(), but it still has to redraw the page.
        if (step == 0) {
            const auto& ks = M5Cardputer.Keyboard.keysState();
            const int p    = ks.fn ? 1 : (ks.shift ? 2 : (ks.opt ? 3 : 0));
            if (p != page) {
                page = p;
                drawSlots(page, slot, what);
            }
        }

        const uint32_t a       = readActs();
        const uint32_t pressed = a & ~prev;
        prev                   = a;
        if (!pressed) {
            delay(5);
            continue;
        }

        if (pressed & kBack) return false;

        if (step == 0) {
            if (pressed & kLeft) slot = (slot + quickbind::kSlots - 1) % quickbind::kSlots;
            if (pressed & kRight) slot = (slot + 1) % quickbind::kSlots;
            if (pressed & kUp) slot = (slot + quickbind::kSlots - 3) % quickbind::kSlots;
            if (pressed & kDown) slot = (slot + 3) % quickbind::kSlots;
            if (pressed & kDel) quickbind::clear(page, slot);
            if (pressed & kEnter) {
                step = 1;
                drawIcons(icon, what);
                continue;
            }
            drawSlots(page, slot, what);
        } else if (step == 1) {
            if (pressed & kLeft) icon = (icon + kIconCount - 1) % kIconCount;
            if (pressed & kRight) icon = (icon + 1) % kIconCount;
            if (pressed & kUp) icon = (icon + kIconCount - 4) % kIconCount;
            if (pressed & kDown) icon = (icon + 4) % kIconCount;
            if (pressed & kEnter) {
                step = 2;
                drawColors(color, what);
                continue;
            }
            drawIcons(icon, what);
        } else {
            if (pressed & kLeft) color = (color + quickbind::kColorCount - 1) % quickbind::kColorCount;
            if (pressed & kRight) color = (color + 1) % quickbind::kColorCount;
            if (pressed & kEnter) {
                quickbind::set(page, slot, kind, index, static_cast<uint8_t>(icon),
                               static_cast<uint8_t>(color), key);
                Serial.printf("[bind] page=%s slot=%s kind=%u index=%u icon=%d color=%d key=%08lX\n",
                              quickbind::pageName(page), quickbind::keyLegend(slot),
                              static_cast<unsigned>(kind), static_cast<unsigned>(index), icon,
                              color, static_cast<unsigned long>(key));
                return true;
            }
            drawColors(color, what);
        }
    }
}

}  // namespace bindpick
