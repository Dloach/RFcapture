// NFC tool: read a card, keep it, put it back on the air.
//
// Rewritten from scratch on top of M5Unit-NFC's own examples:
//   reading  -> examples/UnitUnified/NFCA/Detect + identifies the PICC
//   emulation-> examples/UnitUnified/NFCA/Emulation, followed step for step
//
// Two things learned the hard way and encoded here:
//
// 1. Emulation mode must be chosen BEFORE Units.begin().  The library has no
//    supported way to turn the poller into a target at runtime (calling
//    configureEmulationMode() later arms the state machine but the chip answers
//    nothing).  So the mode is picked by rebooting with a flag in NVS.
//
// 2. While a session is in flight, nothing slow may run.  The library's example
//    says printing stops the emulation answering, and the same is true of an I2C
//    keyboard poll (~1 ms) or a full-screen redraw (~15 ms).  In emulation mode
//    this firmware touches the keyboard only while the card is idle (state Off /
//    None) and never redraws more than a 16 px strip.
//
// Keys are shown on every page, in the same visual language as the rest of this
// device: orange move, ENTER do it, esc back.
//
//   menu:   ; .  move        ENTER open
//   list:   ; .  move        ENTER emulate this card      D delete
//   scan:   S start/stop     esc back
//
// Entered from src/launcher.cpp; the RF feature lives in src/app.cpp.

#include <M5Cardputer.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include <wiring/m5_unit_unified_wiring.hpp>

#include <Preferences.h>
#include <vector>

#include "launcher.h"
#include "nfc_app.h"
#include "nfc_cards.h"
#include "sd_mount.h"
#include "polling_keyboard.h"
#include "bind_picker.h"
#include "quick_bind.h"
#include "ui_hint.h"

using namespace m5::nfc;
using namespace m5::nfc::a;

namespace {

// ---------------------------------------------------------------------------
// Palette / geometry: the same values the rest of this device uses.
// ---------------------------------------------------------------------------
constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;

constexpr int kHeaderH = 13;
constexpr int kFooterY = 118;
constexpr int kRowH    = 15;
constexpr int kRows    = 7;  // 13..118 is 105 px of body

auto& lcd = M5Cardputer.Display;

m5::unit::UnitUnified units;
// Default address is CS 6 on this cap; passed explicitly so a disagreement fails
// loudly here instead of talking to nothing.
m5::unit::CapCC1101NFC unit{6};
NFCLayerA nfcA{unit};
EmulationLayerA emu{unit};

bool nfcReady = false;

void fieldOff();  // defined with scanOnce(), below; used by every reboot path

// ---------------------------------------------------------------------------
// Boot mode: reader or emulator.  One NVS namespace, two bytes plus an optional
// stable binding key.
// ---------------------------------------------------------------------------
constexpr char kNs[] = "nfcretry";

struct Boot {
    uint8_t mode;  // 0 = reader, 1 = emulate card[idx]
    uint8_t idx;
    uint32_t key;  // stable quickbind key; 0 = legacy index-only binding
};

Boot readBoot()
{
    Preferences p;
    Boot b{0, 0, 0};
    if (p.begin(kNs, true)) {
        b.mode = p.getUChar("mode", 0);
        b.idx  = p.getUChar("idx", 0);
        b.key  = p.getUInt("key", 0);
        p.end();
    }
    return b;
}

void clearBoot()
{
    Preferences p;
    if (p.begin(kNs, false)) {
        p.remove("mode");
        p.remove("idx");
        p.remove("key");
        p.end();
    }
}

void rebootAs(const uint8_t mode, const uint8_t idx, const uint32_t key = 0)
{
    // Kill the carrier before leaving: the ST25R3916 keeps its registers
    // across an ESP32 restart, and a live 13.56 MHz field corrupts the SD
    // mount in the next feature (and starves the keyboard).
    if (nfcReady) fieldOff();
    nfccards::flush(true);  // do not lose the last batched hit counts
    Preferences p;
    if (p.begin(kNs, false)) {
        p.putUChar("mode", mode);
        p.putUChar("idx", idx);
        if (key != 0) {
            p.putUInt("key", key);
        } else {
            p.remove("key");
        }
        p.end();
    }
    Serial.flush();
    delay(50);
    ESP.restart();
}

// ---------------------------------------------------------------------------
// Drawing helpers (direct to the panel; no sprite buffer in this project)
// ---------------------------------------------------------------------------
void header(const char* title)
{
    lcd.fillRect(0, 0, lcd.width(), kHeaderH, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(title, 3, 2);

    const int level = M5.Power.getBatteryLevel();
    char bat[12];
    if (level < 0) {
        snprintf(bat, sizeof(bat), "--");
    } else {
        snprintf(bat, sizeof(bat), "%d%%", level);
    }
    lcd.drawRightString(bat, lcd.width() - 3, 2);
}

// Coloured key hints, the same convention as the rest of the firmware: the key in
// green, what it does in grey.
struct Hint {
    const char* key;
    const char* text;
};

void hints(const Hint* h, const int n)
{
    lcd.drawFastHLine(0, kFooterY, lcd.width(), kDim);
    int x = 3;
    for (int i = 0; i < n; ++i) {
        lcd.setTextColor(kOk, kBg);
        if (h[i].key[0] == ';') {
            // "; ." means "the orange arrow keys": draw them as triangles.
            drawMoveTriangles(lcd, x, kFooterY + 4, kOk);
            x += kMoveTrianglesW;
        } else {
            lcd.drawString(h[i].key, x, kFooterY + 4);
            x += lcd.textWidth(h[i].key);
        }
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(h[i].text, x + 2, kFooterY + 4);
        x += lcd.textWidth(h[i].text) + 10;
    }
}

// ---------------------------------------------------------------------------
// Reader side
// ---------------------------------------------------------------------------
enum class Scr : uint8_t { kMenu, kList, kScan, kRename };
Scr scr = Scr::kMenu;

int menuSel = 0;
char statusMsg[40] = {0};  // one-line result of the last read/error
char listNotice[32] = {0}; // warning shown instead of the list title
int listSel = 0;
int listTop = 0;

bool scanning = false;
bool dirty    = true;  // something on screen changed
uint32_t polls = 0;
char liveUid[24] = {0};
char liveType[26] = {0};
bool liveOk = false;

// Rename editor
int renameIdx    = -1;
char renameBuf[24] = {0};
int renameLen    = 0;
int renamePos    = 0;
bool renameCursorOn = true;
uint32_t renameCursorMs = 0;
bool renameFull = true;
std::vector<char> prevWord;

void drawMenu()
{
    lcd.fillScreen(kBg);
    header("NFC TOOLS");

    const char* items[] = {"READ CARDS", "CARD LIST"};
    lcd.setTextSize(2);
    for (int i = 0; i < 2; ++i) {
        const int y = 34 + i * 26;
        if (i == menuSel) lcd.fillRect(4, y - 4, lcd.width() - 8, 18, kBar);
        lcd.setTextColor(i == menuSel ? kAccent : kText, i == menuSel ? kBar : kBg);
        lcd.setCursor(10, y);
        lcd.printf("%s %s", i == menuSel ? ">" : " ", items[i]);
    }
    lcd.setTextSize(1);

    char info[48];
    snprintf(info, sizeof(info), "%u card(s) stored", static_cast<unsigned>(nfccards::count));
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(info, 6, 100);

    if (statusMsg[0]) {
        lcd.setTextColor(kAccent, kBg);
        lcd.drawString(statusMsg, 6, 110);
    }

    const Hint h[] = {{"; .", "move"}, {"ENT", "open"}};
    hints(h, 2);
}

void drawList()
{
    Serial.printf("[nfc] drawList count=%u sel=%u top=%u\n", static_cast<unsigned>(nfccards::count),
                  static_cast<unsigned>(listSel), static_cast<unsigned>(listTop));
    lcd.fillScreen(kBg);
    char title[32];
    if (listNotice[0]) {
        snprintf(title, sizeof(title), "%s", listNotice);
    } else {
        snprintf(title, sizeof(title), "CARD LIST  %u", static_cast<unsigned>(nfccards::count));
    }
    header(title);

    if (nfccards::count == 0) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("No cards yet.", 6, 40);
        lcd.drawString("Menu -> READ CARDS", 6, 56);
        const Hint h[] = {{"esc", "back"}};
        hints(h, 1);
        return;
    }

    for (int r = 0; r < kRows; ++r) {
        const int i = listTop + r;
        if (i >= nfccards::count) break;
        const nfccards::Card& c = nfccards::cards[i];
        const int y = kHeaderH + 3 + r * kRowH;

        if (i == listSel) lcd.fillRect(0, y - 2, lcd.width(), kRowH - 2, kBar);
        const uint16_t fg = (i == listSel) ? kAccent : kText;
        const uint16_t bg = (i == listSel) ? kBar : kBg;

        char uid[24];
        nfccards::uidToString(c, uid, sizeof(uid));

        // Name first (user-editable), then the UID that the door actually sees.
        // Ten name characters keeps the full 10-byte UID on screen without
        // colliding with the hit counter on the right.
        char line[40];
        snprintf(line, sizeof(line), "%02d %-10.10s %s", i + 1, c.name, uid);
        lcd.setTextColor(fg, bg);
        lcd.drawString(line, 3, y);

        char right[16];
        if (!nfccards::replayable(c)) {
            snprintf(right, sizeof(right), "NO RF");
        } else {
            snprintf(right, sizeof(right), "x%u", static_cast<unsigned>(c.hits));
        }
        lcd.setTextColor(nfccards::replayable(c) ? kDim : kWarn, bg);
        lcd.drawRightString(right, lcd.width() - 3, y);
    }

    const Hint h[] = {{"ENT", "emu"}, {"B", "bind"}, {"N", "name"}, {"D", "del"}, {"esc", "back"}};
    hints(h, 5);
}

// The antenna field, switched around each detect() pass.  Leaving it on while
// the page is open makes the keyboard controller stop scanning (it sits on the
// same flex PCB), and a dead keyboard is only recoverable by a power cycle.
//
// The ON half is load-bearing: the library switches the field on exactly once,
// at the end of configure_nfc_a() (nfc_initial_field_on), and the transceive
// path never touches tx_en again.  A previous version of this function only
// switched the field OFF after each pass, so after the first 120 ms window the
// antenna never radiated again and no card was ever seen.
void fieldOn()
{
    uint8_t opctl = 0;
    if (unit.readOperationControl(opctl)) {
        unit.writeOperationControl(static_cast<uint8_t>(opctl | 0x48));  // rx_en | tx_en
    }
}

void fieldOff()
{
    uint8_t opctl = 0;
    if (unit.readOperationControl(opctl)) {
        unit.writeOperationControl(static_cast<uint8_t>(opctl & ~0x08));  // tx_en
    }
}

void scanOnce()
{
    if (!nfcReady) return;
    ++polls;
    bool addedCard = false;
    fieldOn();
    std::vector<PICC> piccs;
    if (nfcA.detect(piccs, 120)) {
        for (auto& p : piccs) {
            const bool identified = nfcA.identify(p);

            nfccards::Card c;
            memset(&c, 0, sizeof(c));
            c.len = p.size ? p.size : 4;
            if (c.len > nfccards::kUidMax) c.len = nfccards::kUidMax;
            memcpy(c.uid, p.uid, c.len);
            c.atqa = p.atqa;
            c.sak  = p.sak;
            snprintf(c.type, sizeof(c.type), "%s",
                     identified ? p.typeAsString().c_str() : "NFC-A");
            c.hits = 1;

            bool isNew        = false;
            const int8_t idx  = nfccards::add(c, &isNew);
            if (isNew) addedCard = true;
            nfccards::uidToString(c, liveUid, sizeof(liveUid));
            snprintf(liveType, sizeof(liveType), "%s", c.type);
            liveOk = idx >= 0;
            dirty  = true;
            if (liveOk) M5.Speaker.tone(2500, 60);
            Serial.printf("[read] %s uid=%s len=%u atqa=%04X sak=%02X type=%s count=%u\n",
                          identified ? "identified" : "unidentified", liveUid,
                          static_cast<unsigned>(c.len), static_cast<unsigned>(c.atqa),
                          static_cast<unsigned>(c.sak), c.type, static_cast<unsigned>(nfccards::count));
        }
    }
    if (!piccs.empty()) nfcA.deactivate();
    fieldOff();
    // Never write SPIFFS while the field is on.  A new card is worth an
    // immediate commit; a hit-count bump waits for the 30 s batch (or exit).
    nfccards::flush(addedCard);
}

void drawScan()
{
    lcd.fillScreen(kBg);
    header("READ CARDS");

    lcd.setTextColor(!nfcReady ? kWarn : (scanning ? kOk : kDim), kBg);
    lcd.setTextSize(2);
    lcd.drawString(!nfcReady ? "CHIP OFFLINE" : (scanning ? "TAP A CARD" : "STOPPED"), 10, 22);
    lcd.setTextSize(1);

    if (liveUid[0]) {
        lcd.setTextColor(liveOk ? kAccent : kWarn, kBg);
        lcd.drawString(liveUid, 10, 50);
        lcd.setTextColor(kText, kBg);
        lcd.drawString(liveType, 10, 64);
    } else if (scanning) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("Hold the card against the", 10, 52);
        lcd.drawString("back of the module.", 10, 64);
    }

    char line[48];
    snprintf(line, sizeof(line), "polls=%lu  stored=%u", static_cast<unsigned long>(polls),
             static_cast<unsigned>(nfccards::count));
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(line, 10, 88);

    if (nfccards::lastError) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString(nfccards::lastError, 10, 100);
    }

    const Hint h[] = {{"S", scanning ? "stop" : "scan"}, {"esc", "back"}};
    hints(h, 2);
}

void drawRenameLine()
{
    char shown[26];
    int n = 0;
    for (int i = 0; i <= renameLen && n + 1 < static_cast<int>(sizeof(shown)); ++i) {
        // A space when the cursor is off keeps the text from shifting left and
        // right every blink.
        if (i == renamePos) shown[n++] = renameCursorOn ? '_' : ' ';
        if (i < renameLen) shown[n++] = renameBuf[i];
    }
    shown[n] = '\0';

    // Only the name line is repainted: a blinking cursor must not blank and
    // redraw the whole panel every 500 ms.
    lcd.fillRect(0, 42, lcd.width(), 22, kBg);
    lcd.setTextColor(kText, kBg);
    lcd.setTextSize(2);
    lcd.drawString(shown, 6, 44);
    lcd.setTextSize(1);
}

void drawRename()
{
    lcd.fillScreen(kBg);
    header("RENAME");
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("New name:", 6, 24);

    drawRenameLine();

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Type on the keyboard.", 6, 78);

    const Hint h[] = {{"ENT", "save"}, {", /", "cursor"}, {"del", "erase"}, {"esc", "cancel"}};
    hints(h, 4);
}

// ---------------------------------------------------------------------------
// Emulation side
// ---------------------------------------------------------------------------
const char* const kStateWord[] = {"NONE", "WAITING FOR READER", "READER SEES IT",
                                  "READER HAS THE UID", "READER IS TALKING", "READER SAID HALT"};
const uint16_t kStateCol[]     = {kDim, kWarn, kDim, kOk, kOk, kWarn};

uint8_t emuIdx = 0;

// Ultralight memory image, from the vendor example.  Page 0/1 are overwritten
// with the card's UID: for a 4-byte UID that is UID + BCC, for 7 bytes the
// example's three-plus-four layout.
uint8_t piccMemory[64] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA3, 0x00, 0x00, 0xE1, 0x10, 0x06, 0x00,
    0x03, 0x25, 0x91, 0x01, 0x0D, 0x55, 0x04, 0x6D, 0x35, 0x73, 0x74, 0x61, 0x63, 0x6B, 0x2E, 0x63,
    0x6F, 0x6D, 0x2F, 0x51, 0x01, 0x10, 0x54, 0x02, 0x65, 0x6E, 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x20,
    0x4D, 0x35, 0x53, 0x74, 0x61, 0x63, 0x6B, 0xFE, 0x44, 0x45, 0x46, 0x00, 0x44, 0x45, 0x46, 0x00,
};

uint8_t bcc8(const uint8_t* p, const uint8_t len, const uint8_t init = 0)
{
    uint8_t v = init;
    for (uint_fast8_t i = 0; i < len; ++i) v ^= p[i];
    return v;
}

void drawEmuPage()
{
    lcd.fillScreen(kBg);
    header("EMULATE");

    const nfccards::Card& c = nfccards::cards[emuIdx];
    char uid[24];
    nfccards::uidToString(c, uid, sizeof(uid));

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("UID", 5, 20);
    lcd.setTextColor(kAccent, kBg);
    lcd.setTextSize(2);
    lcd.drawString(uid, 40, 18);
    lcd.setTextSize(1);

    char line[40];
    lcd.setTextColor(kDim, kBg);
    snprintf(line, sizeof(line), "ATQA %04X   SAK %02X   %u B", static_cast<unsigned>(c.atqa),
             static_cast<unsigned>(c.sak), static_cast<unsigned>(c.len));
    lcd.drawString(line, 5, 40);
    lcd.drawString(c.type, 5, 52);

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Hold the module against the", 5, 74);
    lcd.drawString("reader. esc returns to menu.", 5, 86);
}

// The only thing redrawn while a session runs: a 16 px strip.
void drawStateStrip()
{
    const uint8_t si = static_cast<uint8_t>(emu.state());
    const uint8_t k  = si < 6 ? si : 0;
    lcd.fillRect(0, 102, lcd.width(), 16, kBg);
    lcd.setTextColor(kStateCol[k], kBg);
    lcd.drawString(kStateWord[k], 5, 105);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
enum Action : uint32_t {
    kUp     = 1u << 0,
    kDown   = 1u << 1,
    kEnter  = 1u << 2,
    kBack   = 1u << 3,   // BKSP: delete a character in the rename editor
    kScan   = 1u << 4,
    kDelete = 1u << 5,
    kName   = 1u << 6,
    kBind   = 1u << 7,
    kCancel = 1u << 8,   // ` / ESC: leave the current page without saving
    kLeft   = 1u << 9,
    kRight  = 1u << 10,
};

uint32_t prevActions = 0;

uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            case ';': case 'k': case 'K': a |= kUp; break;
            case '.': case 'j': case 'J': a |= kDown; break;
            case ',': case 'h': case 'H': a |= kLeft; break;
            case '/': case 'l': case 'L': a |= kRight; break;
            case '`': case '~': a |= kCancel; break;
            case 0x1B: a |= kCancel; break;
            case KEY_ENTER: a |= kEnter; break;
            case KEY_BACKSPACE: a |= kBack; break;
            case 's': case 'S': a |= kScan; break;
            case 'd': case 'D': a |= kDelete; break;
            case 'n': case 'N': a |= kName; break;
            case 'b': case 'B': a |= kBind; break;
            default: break;
        }
    }
    return a;
}

void startEmulationOf(int index)
{
    if (index < 0 || index >= nfccards::count) {
        snprintf(listNotice, sizeof(listNotice), "CARD NOT FOUND");
        dirty = true;
        return;
    }
    const nfccards::Card& c = nfccards::cards[index];
    if (!nfccards::replayable(c)) {
        snprintf(listNotice, sizeof(listNotice), "CARD NOT REPLAYABLE");
        dirty = true;
        return;
    }
    if (!nfcReady) {
        snprintf(listNotice, sizeof(listNotice), "NFC OFFLINE");
        dirty = true;
        return;
    }
    rebootAs(1, static_cast<uint8_t>(index), quickbind::keyForNfc(c.uid, c.len));
}

// A failed arm used to reboot straight back to the reader menu with no message
// -- at a door that reads as "pressed ENTER, screen went black, back at the
// menu".  Say what failed and wait for esc instead.
void emuFail(const char* reason)
{
    Serial.printf("[emu] FAILED: %s (nfcReady=%d)\n", reason, static_cast<int>(nfcReady));
    lcd.fillScreen(kBg);
    header("EMULATE");
    lcd.setTextColor(kWarn, kBg);
    lcd.setTextSize(2);
    lcd.drawString(reason, 6, 28);
    lcd.setTextSize(1);
    lcd.setTextColor(kDim, kBg);
    if (!nfcReady) {
        lcd.drawString("The NFC chip did not come up.", 6, 56);
        lcd.drawString("Unplug the board, then retry.", 6, 68);
    }
    lcd.drawString("esc: back to NFC menu", 6, 96);
    for (;;) {
        M5Cardputer.update();
        if (readActions() & (kBack | kCancel)) break;
        delay(20);
    }
    rebootAs(0, 0);  // does not return
}

void handleKeys(uint32_t pressed)
{
    if (scr == Scr::kRename) {
        if (pressed & kCancel) {
            // ESC must leave the editor, not delete a character.  BKSP keeps
            // its usual meaning below.
            renameBuf[0] = 0;
            renameLen    = 0;
            renamePos    = 0;
            scr          = Scr::kList;
            dirty        = true;
            return;
        }
        if (pressed & kLeft) {
            if (renamePos > 0) --renamePos;
            dirty = true;
        }
        if (pressed & kRight) {
            if (renamePos < renameLen) ++renamePos;
            dirty = true;
        }
        if (pressed & kDelete) {
            if (renamePos < renameLen) {
                memmove(&renameBuf[renamePos], &renameBuf[renamePos + 1],
                        static_cast<size_t>(renameLen - renamePos));
                --renameLen;
                renameBuf[renameLen] = 0;
                dirty                = true;
            }
        }
        if (pressed & kBack) {
            if (renamePos > 0) {
                memmove(&renameBuf[renamePos - 1], &renameBuf[renamePos],
                        static_cast<size_t>(renameLen - renamePos + 1));
                --renamePos;
                --renameLen;
                dirty = true;
            }
        }
        if (pressed & kEnter) {
            if (renameLen > 0) nfccards::rename(static_cast<int8_t>(renameIdx), renameBuf);
            scr   = Scr::kList;
            dirty = true;
        }
        return;
    }

    if (pressed & (kBack | kCancel)) {
        if (scr == Scr::kMenu) {
            // Leave the NFC feature: kill the carrier first, same reason as
            // rebootAs() above, then back to the launcher's RF/NFC choice.
            if (nfcReady) fieldOff();
            nfccards::flush(true);
            launcher::rebootInto(launcher::kAppMenu);
        }
        if (scr == Scr::kScan) scanning = false;
        scr = Scr::kMenu;
        return;
    }

    switch (scr) {
        case Scr::kMenu:
            if (pressed & kUp) menuSel = (menuSel + 1) % 2;    // two items: wrap
            if (pressed & kDown) menuSel = (menuSel + 1) % 2;  // both toggle
            if (pressed & kEnter) {
                switch (menuSel) {
                    case 0:
                        scr      = Scr::kScan;
                        scanning = true;
                        polls    = 0;
                        statusMsg[0] = 0;
                        break;
                    case 1:
                        Serial.printf("[nfc] enter list count=%u\n",
                                      static_cast<unsigned>(nfccards::count));
                        listNotice[0] = 0;
                        scr           = Scr::kList;
                        break;
                }
            }
            break;

        case Scr::kList:
            if (nfccards::count) {
                if (pressed & kUp) listSel = (listSel + nfccards::count - 1) % nfccards::count;
                if (pressed & kDown) listSel = (listSel + 1) % nfccards::count;
                if (listSel < listTop) listTop = listSel;
                if (listSel >= listTop + kRows) listTop = listSel - kRows + 1;
            }
            if (pressed & kEnter) startEmulationOf(listSel);
            if (pressed & kBind) {
                if (nfccards::count) {
                    // Modal picker: grid key, then icon, then color.  Store the
                    // UID-derived key so deleting an earlier card cannot make
                    // this slot emulate a different one.
                    const nfccards::Card& c = nfccards::cards[listSel];
                    bindpick::run(quickbind::kNfc, static_cast<uint8_t>(listSel), c.name,
                                  quickbind::keyForNfc(c.uid, c.len));
                    dirty = true;
                }
            }
            if (pressed & kDelete) {
                nfccards::remove(static_cast<int8_t>(listSel));
                if (listSel >= nfccards::count) listSel = nfccards::count ? nfccards::count - 1 : 0;
            }
            if (pressed & kName) {
                if (nfccards::count) {
                    renameIdx = listSel;
                    snprintf(renameBuf, sizeof(renameBuf), "%s", nfccards::cards[listSel].name);
                    renameLen = static_cast<int>(strlen(renameBuf));
                    renamePos = renameLen;
                    renameCursorOn = true;
                    renameCursorMs = millis();
                    renameFull     = true;
                    prevWord.clear();
                    scr = Scr::kRename;
                }
            }
            break;

        case Scr::kScan:
            if (pressed & kScan) {
                scanning = !scanning;
                if (scanning) polls = 0;
            }
            break;

        default: break;
    }
}

// ---------------------------------------------------------------------------
// Reader-mode setup and loop
// ---------------------------------------------------------------------------
void readerSetup()
{
    auto ucfg = unit.config();
    ucfg.mode = NFC::A;
    unit.config(ucfg);

    // Retry: after the boot-time power pulse the chip is not always ready on the
    // first attempt, and a single failed begin() used to leave the whole NFC
    // feature silently dead (no card reads, emulation unable to arm).
    for (int attempt = 0; attempt < 4; ++attempt) {
        const bool wired = m5::unit::wiring::addSPI(units, unit, 10000000, 1);
        nfcReady         = wired && units.begin();
        Serial.printf("[read] addSPI=%d begin=%d (attempt %d)\n", static_cast<int>(wired),
                      static_cast<int>(nfcReady), attempt + 1);
        if (nfcReady) break;
        delay(300);
    }

    // configure_nfc_a() ends with the carrier ON (nfc_initial_field_on).  Park
    // it now: scans switch it on per pass and off again, and a field left
    // radiating starves the keyboard -- and corrupts the next feature's SD
    // mount, because the chip keeps this register across an ESP32 restart.
    if (nfcReady) fieldOff();

    // Keyboard last: the polling reader is I2C traffic, and anything that touches
    // the bus while the NFC unit is being brought up has been a source of grief.
    // The detach must come first (src/app.cpp:82), or the destroyed stock reader
    // leaves a dangling handler on GPIO 11.
    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));

    if (!nfcReady) {
        // Reading is silently dead without this on screen: detect() on a chip
        // that never came up returns false forever and the page looks "alive".
        snprintf(statusMsg, sizeof(statusMsg), "NFC OFFLINE - unplug, retry");
    }

    nfccards::load();
    Serial.printf("[read] cards=%u err=%s\n", static_cast<unsigned>(nfccards::count),
                  nfccards::lastError ? nfccards::lastError : "-");

    scr = Scr::kMenu;
    drawMenu();
}

void readerLoop()
{
    M5Cardputer.update();
    nfccards::flush();  // batched hit counts; no-op unless dirty and 30 s elapsed

    if (scanning && scr == Scr::kScan) {
        static uint32_t lastScan = 0;
        if (millis() - lastScan >= 400) {
            lastScan = millis();
            scanOnce();
        }
    }

    const uint32_t a       = readActions();
    const uint32_t pressed = a & ~prevActions;
    prevActions            = a;
    if (pressed) {
        dirty = true;
        handleKeys(pressed);
    }

    if (scr == Scr::kRename) {
        const auto& word = M5Cardputer.Keyboard.keysState().word;
        for (char ch : word) {
            if (ch < 32 || ch >= 127) continue;
            // ; , . / are the orange arrow keys, not name punctuation; ` is
            // the esc key.  Same filter as the RF rename editor.
            if (ch == ';' || ch == ',' || ch == '.' || ch == '/') continue;
            if (ch == '`' || ch == '~') continue;
            bool already = false;
            for (char q : prevWord) {
                if (q == ch) {
                    already = true;
                    break;
                }
            }
            if (already) continue;
            if (renamePos > renameLen) renamePos = renameLen;
            if (renameLen >= static_cast<int>(sizeof(renameBuf)) - 1) break;
            memmove(&renameBuf[renamePos + 1], &renameBuf[renamePos],
                    static_cast<size_t>(renameLen - renamePos + 1));
            renameBuf[renamePos] = ch;
            ++renameLen;
            ++renamePos;
            dirty = true;
        }
        prevWord.assign(word.begin(), word.end());
        if (!word.empty()) dirty = true;
    }

    // Blink the rename cursor at 2 Hz.  Repaint only the name line; a full-page
    // redraw every 500 ms was visible as flicker on the small panel.
    if (scr == Scr::kRename && millis() - renameCursorMs >= 500) {
        renameCursorMs = millis();
        renameCursorOn = !renameCursorOn;
        drawRenameLine();
    }

    // Redraw only when something changed.  Repainting the whole panel on a timer
    // is what made the menu flicker: fillScreen() blanks the page and the redraw
    // lands a few milliseconds later, which the eye sees as a blink.
    static uint32_t lastDraw = 0;
    if (dirty && millis() - lastDraw >= 30) {
        lastDraw = millis();
        dirty    = false;
        switch (scr) {
            case Scr::kMenu: drawMenu(); break;
            case Scr::kList: drawList(); break;
            case Scr::kScan: drawScan(); break;
            case Scr::kRename:
                if (renameFull) {
                    drawRename();
                    renameFull = false;
                } else {
                    drawRenameLine();
                }
                break;
        }
    }

    delay(2);
}

// ---------------------------------------------------------------------------
// Emulation-mode setup and loop
//
// As close to the vendor example as this device allows: the chip is armed and the
// loop does Units.update() then emu.update() and nothing else, except a keyboard
// poll while the card is idle.
// ---------------------------------------------------------------------------
void emulatorSetup(uint8_t idx, uint32_t key)
{
    // Emulation must be chosen before begin().
    auto ucfg      = unit.config();
    ucfg.emulation = true;
    ucfg.mode      = NFC::A;
    unit.config(ucfg);

    for (int attempt = 0; attempt < 4; ++attempt) {
        const bool wired = m5::unit::wiring::addSPI(units, unit, 10000000, 1);
        nfcReady         = wired && units.begin();
        Serial.printf("[emu] addSPI=%d begin=%d (attempt %d)\n", static_cast<int>(wired),
                      static_cast<int>(nfcReady), attempt + 1);
        if (nfcReady) break;
        delay(300);
    }

    // Same polling keyboard as the reader half -- emuFail() below waits for a
    // key, and the stock edge-driven reader dies after a handful of presses.
    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));

    if (!nfcReady) emuFail("NFC CHIP OFFLINE");  // waits for esc, never returns

    nfccards::load();
    if (nfccards::count == 0) {
        emuFail("NO CARDS STORED");
    }

    // Resolve by stable key first.  The index in a quick binding is only a hint:
    // deleting an earlier card shifts it, and the old `idx % count` would then
    // silently put a different card on the air.  A zero key is a legacy binding;
    // it may use the index but still has to land on a replayable card.
    int chosen = -1;
    if (key != 0) {
        for (uint8_t i = 0; i < nfccards::count; ++i) {
            if (quickbind::keyForNfc(nfccards::cards[i].uid, nfccards::cards[i].len) == key) {
                chosen = i;
                break;
            }
        }
    } else if (idx < nfccards::count) {
        chosen = idx;
    }
    if (chosen < 0) {
        emuFail("BINDING STALE");
    }
    if (!nfccards::replayable(nfccards::cards[chosen])) {
        emuFail("CARD NOT REPLAYABLE");
    }
    emuIdx = static_cast<uint8_t>(chosen);

    const nfccards::Card& c = nfccards::cards[emuIdx];

    PICC picc;
    // Carrier: the only types this library can emulate.  4-byte UIDs ride on
    // Ultralight, 7-byte on NTAG215.
    const Type carrier = (c.len == 7) ? Type::NTAG_215 : Type::MIFARE_Ultralight;
    if (!picc.emulate(carrier, c.uid, c.len)) {
        emuFail("CARD NOT EMULATABLE");
    }
    // The captured identity wins over the carrier's defaults.  This is the pair
    // the gate reader decides on after selecting the UID.
    picc.atqa = c.atqa;
    picc.sak  = c.sak;

    if (c.len == 4) {
        memcpy(piccMemory, c.uid, 4);
        piccMemory[4] = bcc8(c.uid, 4);
    } else {
        memcpy(piccMemory, c.uid, 3);
        piccMemory[3] = bcc8(c.uid, 3, 0x88);
        memcpy(piccMemory + 4, c.uid + 3, 4);
        piccMemory[8] = bcc8(c.uid + 3, 4);
    }

    if (!emu.begin(picc, piccMemory, sizeof(piccMemory))) {
        emuFail("ARM FAILED");
    }
    emu.setExpiredTime(10000);

    const auto& e = emu.emulatePICC();
    Serial.printf("[emu] on air uid=%s atqa=%04X sak=%02X type=%s\n", e.uidAsString().c_str(),
                  static_cast<unsigned>(e.atqa), static_cast<unsigned>(e.sak),
                  e.typeAsString().c_str());

    drawEmuPage();
    drawStateStrip();
}

void emulatorLoop()
{
    // Order and minimalism both matter: see the file header.
    units.update();
    emu.update();

    // Keyboard only while the card is idle.  A poll is ~1 ms of I2C and a reader
    // allows a few hundred microseconds for an answer.
    const auto state = emu.state();
    if (state == EmulationLayerA::State::None || state == EmulationLayerA::State::Off) {
        static uint32_t lastKey = 0;
        if (millis() - lastKey >= 400) {
            lastKey = millis();
            M5Cardputer.update();
            const uint32_t a       = readActions();
            const uint32_t pressed = a & ~prevActions;
            prevActions            = a;
            if (pressed & (kBack | kCancel)) {
                clearBoot();
                rebootAs(0, 0);
            }
        }
    }

    static EmulationLayerA::State latest = EmulationLayerA::State::None;
    if (state != latest) {
        latest = state;
        drawStateStrip();
        Serial.printf("[emu] state=%d\n", static_cast<int>(state));
    }

    delay(1);
}

// Which half of the firmware this boot belongs to.  Set once in setup(), because
// the choice comes from NVS and the chip is configured accordingly.
bool gEmuMode = false;

}  // namespace

namespace nfcapp {

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
void queueEmulation(uint8_t idx, uint32_t key)
{
    // Same flag rebootAs(1, idx, key) writes, without the restart: the caller
    // (the launcher's home grid) does the reboot itself through rebootInto().
    Preferences p;
    if (p.begin(kNs, false)) {
        p.putUChar("mode", 1);
        p.putUChar("idx", idx);
        if (key != 0) {
            p.putUInt("key", key);
        } else {
            p.remove("key");
        }
        p.end();
    }
}

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    lcd.setRotation(0);
    lcd.setBrightness(90);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    lcd.fillScreen(kBg);

    // Keep the module powered.  The old deferred-SD path used to cut POWER_EN
    // and needed this call to bring it back; SD work now happens only in the
    // BACKUP feature, on a boot where no radio is initialised.
    sdmount::radioPowerOn();

    const Boot boot = readBoot();
    gEmuMode         = (boot.mode == 1);
    if (gEmuMode) {
        clearBoot();
        emulatorSetup(boot.idx, boot.key);
    } else {
        readerSetup();
    }
}

void loop()
{
    if (gEmuMode) {
        emulatorLoop();
    } else {
        readerLoop();
    }
}

}  // namespace nfcapp