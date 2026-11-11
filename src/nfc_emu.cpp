// NFC DOOR -- minimal card replay firmware.
//
// Built only by `pio run -e nfc-emu`.  The shipping environments never define
// RF_NFC_EMULATE, so for them this is an empty translation unit.
//
// ---------------------------------------------------------------------------
// Why this file was rewritten (2026-11-11)
//
// The previous version of this file (and the NFC USE page in nfc_app.cpp) never
// opened the door, while the stock M5Unit-NFC NFCA/Emulation example with the
// door UID swapped in DID.  So the hardware and the library are fine; the
// failure was in what the custom firmware did differently from the example.
// This rewrite removes every difference that is not strictly necessary:
//
//   1. Carrier type is ALWAYS NTAG_213 -- the type the working example ran
//      with.  The old code used MIFARE_Ultralight for 4-byte UIDs, which
//      changes how the library answers post-SELECT commands (GET_VERSION etc.)
//      and is the one on-air-relevant difference once atqa/sak match.
//   2. ATQA/SAK are left at the library's NTAG_213 defaults (0044/00) -- the
//      values the door accepted.  The old code overwrote them with the captured
//      Classic values (0004/08), which the door rejected.
//   3. The emulated memory is the example's valid NTAG213 image (capability
//      container + NDEF), with the card's UID embedded.  The old code sent a
//      mostly-zeroed buffer.
//   4. The session never expires (setExpiredTime(0)).  The old code used 3 s,
//      so a reader that held a session open longer than that killed the
//      emulation mid-conversation.
//   5. The loop is shaped like the example's: emu.update() runs every pass,
//      with no delay() and no per-pass I2C.  The keyboard is polled at 20 Hz
//      instead (the TCA8418 dies under high-frequency I2C, docs/DEV-STATE.md
//      section 13.1), and the screen is written only on state changes.
//
// What did NOT change: cfg.emulation is still set before Units.begin() (reader
// and emulator are mutually exclusive chip configurations; switching firmware
// means reflashing), and REQA / anti-collision / SELECT are still answered by
// the ST25R3916 itself and never reach receive_callback().  A UID-only reader
// therefore produces ZERO commands in the ring below; a reader that sends
// 0x60/0x61 (MIFARE AUTH) cannot be served -- the emulation layer has no
// crypto1 -- and the verdict line says so.
//
// Cards come from the on-board collection (/nfc/cards.txt on SPIFFS, written by
// the nfc-read firmware).  No real UID appears in this source file on purpose:
// the repository is public and the collection holds real access credentials.
// ---------------------------------------------------------------------------
#ifdef RF_NFC_EMULATE

#include <M5Cardputer.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include <wiring/m5_unit_unified_wiring.hpp>

#include <SPIFFS.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "nfc_store.h"
#include "polling_keyboard.h"
#include "rf_config.h"

namespace {

using State = m5::nfc::EmulationLayerA::State;
using PICC  = m5::nfc::a::PICC;
using Type  = m5::nfc::a::Type;

// Same RGB565 palette and page geometry as src/app.cpp.
constexpr uint16_t kColBg     = 0x0000;
constexpr uint16_t kColText   = 0xFFFF;
constexpr uint16_t kColDim    = 0x8410;
constexpr uint16_t kColAccent = 0xFD20;  // orange, the colour of the arrow keycaps
constexpr uint16_t kColOk     = 0x07E0;
constexpr uint16_t kColWarn   = 0xF800;
constexpr uint16_t kColBar    = 0x2104;
constexpr uint16_t kColHead   = 0x18E3;

constexpr int kHeaderH = 13;
constexpr int kFooterY = 118;

// ---------------------------------------------------------------------------
// Command trace
//
// receive_callback() runs while the chip owes the reader an answer, so it must
// not touch serial or the filesystem -- everything is recorded to RAM and
// flushed once the state machine is back in None/Off, where no session can be
// in flight.
// ---------------------------------------------------------------------------
struct CmdRec {
    uint32_t ms;
    uint8_t cmd;
    uint8_t len;
    uint8_t arg;
};

constexpr uint8_t kCmdRing = 32;
CmdRec cmdRing[kCmdRing];
uint8_t cmdHead      = 0;
uint16_t cmdSeen     = 0;
uint16_t cmdAuth     = 0;  // 0x60 / 0x61 seen
uint16_t cmdFlushed  = 0;

constexpr uint8_t kCmdAuthA = 0x60;  // MIFARE Classic AUTH key A (also NTAG GET_VERSION)
constexpr uint8_t kCmdAuthB = 0x61;  // MIFARE Classic AUTH key B

class EmulationTrace : public m5::nfc::EmulationLayerA {
public:
    using EmulationLayerA::EmulationLayerA;

    State receive_callback(const uint8_t* rx, const uint32_t rx_len) override
    {
        if (rx && rx_len) {
            CmdRec& e = cmdRing[cmdHead];
            e.ms      = millis();
            e.cmd     = rx[0];
            e.len     = static_cast<uint8_t>(rx_len > 255 ? 255 : rx_len);
            e.arg     = rx_len > 1 ? rx[1] : 0;
            cmdHead   = static_cast<uint8_t>((cmdHead + 1) % kCmdRing);
            if (cmdSeen < 0xFFFF) ++cmdSeen;
            if (rx[0] == kCmdAuthA || rx[0] == kCmdAuthB) {
                if (cmdAuth < 0xFFFF) ++cmdAuth;
            }
        }
        // Not special-cased: the base class logs "CMD:%02X" and returns
        // State::Idle for anything it cannot answer, which ends the session --
        // the honest outcome, since there is no crypto1 here to answer an AUTH
        // with anyway.
        return EmulationLayerA::receive_callback(rx, rx_len);
    }
};

m5::unit::UnitUnified units;
m5::unit::CapCC1101NFC nfc{static_cast<uint8_t>(rfcfg::nfcCsPin)};
EmulationTrace emu{nfc};

bool nfcReady = false;

// ---------------------------------------------------------------------------
// Emulated card
//
// The memory image is the NTAG213 image from M5Unit-NFC's own NFCA/Emulation
// example (MIT, (c) M5Stack) -- capability container at page 3, NDEF text
// records, valid lock/dynamic-lock bytes -- because that exact image is part of
// the configuration that opened the door.  Only the UID region (bytes 0..8) is
// rewritten per card.
// ---------------------------------------------------------------------------
constexpr uint32_t kMemSize = 180;  // NTAG213 total

const uint8_t kNtag213Image[kMemSize] = {
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x48, 0x00, 0x00,  //
    0xE1, 0x10, 0x12, 0x00,  //
    0x01, 0x03, 0xA0, 0x0C,  //
    0x34, 0x03, 0x58, 0x91,  //
    0x01, 0x0D, 0x55, 0x04,  //
    0x6D, 0x35, 0x73, 0x74,  //
    0x61, 0x63, 0x6B, 0x2E,  //
    0x63, 0x6F, 0x6D, 0x2F,  //
    0x11, 0x01, 0x11, 0x54,  //
    0x02, 0x7A, 0x68, 0xE4,  //
    0xBD, 0xA0, 0xE5, 0xA5,  //
    0xBD, 0x20, 0x4D, 0x35,  //
    0x53, 0x74, 0x61, 0x63,  //
    0x6B, 0x11, 0x01, 0x10,  //
    0x54, 0x02, 0x65, 0x6E,  //
    0x48, 0x65, 0x6C, 0x6C,  //
    0x6F, 0x20, 0x4D, 0x35,  //
    0x53, 0x74, 0x61, 0x63,  //
    0x6B, 0x51, 0x01, 0x1A,  //
    0x54, 0x02, 0x6A, 0x61,  //
    0xE3, 0x81, 0x93, 0xE3,  //
    0x81, 0xA8, 0xE3, 0x81,  //
    0xAB, 0xE3, 0x81, 0xA1,  //
    0xE3, 0x81, 0xAF, 0x20,  //
    0x4D, 0x35, 0x53, 0x74,  //
    0x61, 0x63, 0x6B, 0xFE,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0xBD,  //
    0x02, 0x00, 0x00, 0xFF,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
    0x00, 0x00, 0x00, 0x00,  //
};

uint8_t piccMem[kMemSize];
PICC picc;

int8_t sel        = 0;  // index into nfcstore::cards
bool emulating    = false;
bool sawSelected  = false;  // latched: a reader completed SELECT since the last arm
const char* startFail = nullptr;

uint8_t bcc8(const uint8_t* p, const uint8_t len, const uint8_t init = 0)
{
    uint8_t v = init;
    for (uint8_t i = 0; i < len; ++i) v ^= p[i];
    return v;
}

// Fresh copy of the example image with the card's UID embedded, in the same
// layout the example's embed_uid() uses: 7-byte UIDs straddle pages 0-2 with
// the cascade-tag BCCs; a 4-byte UID fills page 0 with its plain BCC after it.
void buildMemory(const nfcstore::Card& c)
{
    memcpy(piccMem, kNtag213Image, sizeof(piccMem));
    if (c.len == 7) {
        memcpy(piccMem, c.uid, 3);
        piccMem[3] = bcc8(c.uid, 3, 0x88 /* CT */);
        memcpy(piccMem + 4, c.uid + 3, 4);
        piccMem[8] = bcc8(c.uid + 3, 4);
    } else {
        memcpy(piccMem, c.uid, 4);
        piccMem[4] = bcc8(c.uid, 4);
    }
}

bool startEmulation(const int8_t idx)
{
    startFail = nullptr;
    if (idx < 0 || idx >= nfcstore::count) {
        startFail = "NO CARD SELECTED";
        return false;
    }
    const nfcstore::Card& c = nfcstore::cards[idx];
    if (!nfcstore::replayable(c)) {
        startFail = "10-BYTE UID: CHIP REFUSES";
        return false;
    }

    if (emulating) {
        emu.end();  // sets State::None, which is begin()'s precondition
        emulating = false;
    }

    memset(&picc, 0, sizeof(picc));
    // NTAG_213 regardless of what the card really is, and NO atqa/sak override:
    // picc.emulate() fills 0044/00 from emulation_settings[], which is the
    // identity the door demonstrably accepted.  The captured atqa/sak stay on
    // screen for reference but do not go on the air.
    if (!picc.emulate(Type::NTAG_213, c.uid, c.len)) {
        startFail = "RECORD NOT EMULATABLE";
        return false;
    }
    buildMemory(c);

    cmdHead     = 0;
    cmdSeen     = 0;
    cmdAuth     = 0;
    cmdFlushed  = 0;
    sawSelected = false;

    if (!emu.begin(picc, piccMem, kMemSize)) {
        startFail = "CHIP REFUSED (see serial)";
        return false;
    }
    // 0 disables update_expired() entirely: a reader that holds the session
    // open must never find the emulation gone.
    emu.setExpiredTime(0);
    emulating = true;

    char hex[24];
    nfcstore::uidHex(c, hex, sizeof(hex));
    Serial.printf("[emu] on-air uid=%s as NTAG213 atqa=0044 sak=00\n", hex);
    return true;
}

void stopEmulation()
{
    if (!emulating) return;
    emu.end();
    emulating = false;
}

// ---------------------------------------------------------------------------
// Screen.  No sprite: the page is drawn once per change, the state banner is
// repainted in place on every state transition -- the same trade the library's
// own example makes with its 32x16 state box.
// ---------------------------------------------------------------------------
auto& lcd = M5Cardputer.Display;

const char* const kStateWord[] = {
    "STOPPED",             // None
    "WAITING FOR READER",  // Off    -- armed, chip in sense mode
    "READER POWERING UP",  // Idle   -- a field arrived
    "UID SENT",            // Ready  -- anti-collision answered
    "READER HAS THE UID",  // Active -- SELECT succeeded
    "READER SAID HALT",    // Halt
};
const uint16_t kStateCol[] = {kColDim, kColOk, kColAccent, kColAccent, kColOk, kColWarn};

void drawBanner(const State s)
{
    const uint8_t si = static_cast<uint8_t>(s);
    lcd.fillRect(0, kHeaderH, lcd.width(), 26, kColBar);
    lcd.setTextSize(2);
    lcd.setTextColor(kStateCol[si], kColBar);
    lcd.drawString(kStateWord[si], 5, kHeaderH + 5);
    lcd.setTextSize(1);
}

void drawVerdict()
{
    const int vy = 101;
    uint16_t bg    = kColBar;
    uint16_t fg    = kColDim;
    const char* text;

    if (cmdAuth > 0) {
        bg   = 0x7800;  // dark red: the answer is no, and it should look like it
        fg   = kColText;
        text = "NEEDS A KEY - CANNOT CLONE";
    } else if (sawSelected) {
        bg   = 0x03E0;  // dark green
        fg   = kColText;
        text = "TOOK THE UID, ASKED NO KEY";
    } else if (emulating) {
        text = "HOLD IT AT THE READER";
    } else if (startFail != nullptr) {
        fg   = kColWarn;
        text = startFail;
    } else {
        text = "NOT EMULATING - PRESS ENTER";
    }

    lcd.fillRect(0, vy, lcd.width(), 16, bg);
    lcd.setTextColor(fg, bg);
    lcd.drawString(text, 4, vy + 4);
}

void drawPage()
{
    lcd.fillScreen(kColBg);

    char title[32];
    snprintf(title, sizeof(title), "NFC DOOR  %d/%u", static_cast<int>(sel) + 1,
             static_cast<unsigned>(nfcstore::count));
    lcd.fillRect(0, 0, lcd.width(), kHeaderH, kColHead);
    lcd.setTextColor(kColText, kColHead);
    lcd.drawString(title, 3, 2);

    drawBanner(emu.state());

    int y = kHeaderH + 30;
    if (nfcstore::count == 0) {
        lcd.setTextColor(kColWarn, kColBg);
        lcd.drawString("NO CARDS SAVED", 4, y);
        lcd.setTextColor(kColDim, kColBg);
        lcd.drawString("Flash nfc-read and scan cards first.", 4, y + 15);
        lcd.drawString("This build replays /nfc/cards.txt.", 4, y + 27);
    } else {
        const nfcstore::Card& c = nfcstore::cards[sel];
        char hex[24];
        nfcstore::uidHex(c, hex, sizeof(hex));

        lcd.setTextColor(kColDim, kColBg);
        lcd.drawString("CARD", 4, y);
        lcd.setTextColor(kColText, kColBg);
        lcd.drawString(c.name, 46, y);
        lcd.setTextColor(kColDim, kColBg);
        lcd.drawRightString(c.type, lcd.width() - 4, y);

        y += 15;
        lcd.drawString("UID", 4, y);
        lcd.setTextColor(kColText, kColBg);
        lcd.drawString(hex, 46, y);
        lcd.setTextColor(kColDim, kColBg);
        char meta[24];
        snprintf(meta, sizeof(meta), "ATQA %04X SAK %02X", static_cast<unsigned>(c.atqa),
                 static_cast<unsigned>(c.sak));
        lcd.drawRightString(meta, lcd.width() - 4, y);

        y += 15;
        lcd.drawString("AIR", 4, y);
        lcd.setTextColor(kColOk, kColBg);
        lcd.drawString("as NTAG213", 46, y);
        lcd.setTextColor(kColDim, kColBg);
        lcd.drawRightString("atqa 0044 sak 00", lcd.width() - 4, y);

        y += 15;
        lcd.drawString("CMDS", 4, y);
        lcd.setTextColor(cmdAuth > 0 ? kColWarn : kColText, kColBg);
        char cmds[24];
        snprintf(cmds, sizeof(cmds), "%u seen, %u AUTH", static_cast<unsigned>(cmdSeen),
                 static_cast<unsigned>(cmdAuth));
        lcd.drawString(cmds, 46, y);
    }

    drawVerdict();

    lcd.drawFastHLine(0, kFooterY, lcd.width(), kColDim);
    int x = 3;
    const struct {
        const char* key;
        const char* text;
    } hints[] = {
        {"UP/DN", "card"}, {"ENT", "re-arm"}, {"esc", "stop"},
    };
    for (const auto& h : hints) {
        lcd.setTextColor(kColOk, kColBg);
        lcd.drawString(h.key, x, kFooterY + 4);
        x += lcd.textWidth(h.key) + 2;
        lcd.setTextColor(kColDim, kColBg);
        lcd.drawString(h.text, x, kFooterY + 4);
        x += lcd.textWidth(h.text) + 10;
    }
}

// ---------------------------------------------------------------------------
// Trace flush + heartbeat.  Both wait for None/Off, where no session can be in
// flight -- printing mid-session stalls the emulation's answers.
// ---------------------------------------------------------------------------
const char* const kStateName[] = {"-", "O", "I", "R", "A", "H"};

void flushTrace()
{
    if (cmdSeen == cmdFlushed) return;
    Serial.printf("[emu] === CMD TRACE new=%u total=%u auth=%u ===\n",
                  static_cast<unsigned>(cmdSeen - cmdFlushed), static_cast<unsigned>(cmdSeen),
                  static_cast<unsigned>(cmdAuth));
    const uint16_t shown = cmdSeen < kCmdRing ? cmdSeen : kCmdRing;
    for (uint16_t i = 0; i < shown; ++i) {
        const uint16_t slot = static_cast<uint16_t>((cmdHead + kCmdRing - shown + i) % kCmdRing);
        const CmdRec& e     = cmdRing[slot];
        Serial.printf("[emu] cmd ms=%u %02X len=%u arg=%02X%s\n", static_cast<unsigned>(e.ms), e.cmd,
                      static_cast<unsigned>(e.len), static_cast<unsigned>(e.arg),
                      (e.cmd == kCmdAuthA || e.cmd == kCmdAuthB) ? "  <-- MIFARE AUTH" : "");
    }
    if (cmdAuth > 0) {
        Serial.printf("[emu] VERDICT: reader sent AUTH. It authenticates; replay is impossible "
                      "with this library (no crypto1).\n");
    } else if (cmdSeen > cmdFlushed || sawSelected) {
        Serial.printf("[emu] VERDICT: no AUTH. The reader stopped after selecting the UID.\n");
    }
    cmdFlushed = cmdSeen;
    drawVerdict();  // AUTH may have arrived since the last paint
}

constexpr uint32_t kHeartbeatMs = 5000;
uint32_t lastBeat               = 0;

void printHeartbeat()
{
    Serial.printf("[emu] ready=%d cards=%u sel=%d emulating=%d state=%s cmds=%u auth=%u heap=%u\n",
                  static_cast<int>(nfcReady), static_cast<unsigned>(nfcstore::count),
                  static_cast<int>(sel), static_cast<int>(emulating),
                  kStateName[static_cast<uint8_t>(emu.state())], static_cast<unsigned>(cmdSeen),
                  static_cast<unsigned>(cmdAuth), static_cast<unsigned>(ESP.getFreeHeap()));
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
enum Action : uint32_t {
    kActUp    = 1u << 0,
    kActDown  = 1u << 1,
    kActEnter = 1u << 2,
    kActBack  = 1u << 3,
};

uint32_t prevActions = 0;
uint32_t prevKeysMs  = 0;

uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            // Orange arrow legends: ; = up, . = down.  Letter alternatives for
            // when the second legend gets missed.
            case ';': case 'k': case 'K': a |= kActUp; break;
            case '.': case 'j': case 'J': a |= kActDown; break;
            case '`': case '~': case 0x1B: a |= kActBack; break;
            case KEY_ENTER: a |= kActEnter; break;
            case KEY_BACKSPACE: a |= kActBack; break;
            default: break;
        }
    }
    return a;
}

void switchCard(const int d)
{
    if (nfcstore::count == 0) return;
    // Skip records that can never go on the air (10-byte UIDs).
    for (uint8_t tries = 0; tries < nfcstore::count; ++tries) {
        sel = static_cast<int8_t>((sel + d + nfcstore::count) % nfcstore::count);
        if (nfcstore::replayable(nfcstore::cards[sel])) break;
    }
    if (startEmulation(sel)) {
        M5.Speaker.tone(2000, 60);
    } else {
        M5.Speaker.tone(600, 120);
    }
    drawPage();
}

void doStart()
{
    if (startEmulation(sel)) {
        M5.Speaker.tone(2000, 60);
    } else {
        Serial.printf("[emu] start failed sel=%d reason=%s\n", static_cast<int>(sel),
                      startFail ? startFail : "?");
        M5.Speaker.tone(600, 120);
    }
    drawPage();
}

void handleKeys()
{
    const uint32_t a       = readActions();
    const uint32_t pressed = a & ~prevActions;
    prevActions            = a;
    if (!pressed) return;

    if (pressed & kActUp) switchCard(-1);
    if (pressed & kActDown) switchCard(1);
    if (pressed & kActEnter) doStart();
    if (pressed & kActBack) {
        stopEmulation();
        drawPage();
    }
}

}  // namespace

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    // Polling keyboard reader, same replacement as src/app.cpp:82: the stock
    // edge-driven reader misses /INT edges and dies after a handful of presses
    // (docs/DEV-STATE.md section 4).  The detach must come first, or the
    // destroyed stock reader leaves a dangling handler on GPIO 11.
    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));

    lcd.setRotation(rfcfg::screenRotation);  // landscape 240x135
    lcd.setBrightness(90);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);

    M5.Speaker.setVolume(48);

    // cfg.emulation must be set before Units.begin(): configureEmulationMode()
    // runs from begin(), and reader/emulator are mutually exclusive chip
    // configurations.  This build is an emulator; nfc-read is the reader.
    auto ucfg      = nfc.config();
    ucfg.emulation = true;
    ucfg.mode      = m5::nfc::NFC::A;
    nfc.config(ucfg);

    const bool wired = m5::unit::wiring::addSPI(units, nfc, 10000000, 1);
    nfcReady         = wired && units.begin();
    Serial.printf("[emu] addSPI=%d units.begin=%d\n", static_cast<int>(wired),
                  static_cast<int>(nfcReady));

    // SPIFFS only, no SD: the collection lives there, and the SD slot shares
    // the SPI bus with the NFC chip -- one less device on the bus during
    // emulation is one less thing that can disturb it.
    nfcstore::mount(false);
    const uint8_t n = nfcstore::load();
    Serial.printf("[emu] store=%s cards=%u err=%s\n", nfcstore::primaryName,
                  static_cast<unsigned>(n), nfcstore::lastError ? nfcstore::lastError : "-");

    if (!nfcReady) {
        lcd.fillScreen(kColBg);
        lcd.fillRect(0, 0, lcd.width(), kHeaderH, kColHead);
        lcd.setTextColor(kColText, kColHead);
        lcd.drawString("NFC DOOR", 3, 2);
        lcd.setTextColor(kColWarn, kColBg);
        lcd.drawString("NFC CHIP NOT ANSWERING", 6, 30);
        lcd.setTextColor(kColDim, kColBg);
        char line[40];
        snprintf(line, sizeof(line), "ST25R3916 silent on CS G%d.", rfcfg::nfcCsPin);
        lcd.drawString(line, 6, 50);
        lcd.drawString("Check the CAP.CC1101 module, then reset.", 6, 66);
        return;
    }

    // Start on the first replayable card straight away: the use case is walking
    // up to a door with the board in one hand.
    if (nfcstore::count > 0) {
        sel = 0;
        if (!nfcstore::replayable(nfcstore::cards[sel])) switchCard(1);
        else doStart();
    }
    drawPage();
}

void loop()
{
    if (!nfcReady) {
        delay(200);
        return;
    }

    // The emulation state machine is the whole product; it gets every pass with
    // no delay() anywhere.  Everything else is throttled or event-driven.
    units.update();
    if (emulating) emu.update();

    const State s = emu.state();
    if (s == State::Ready || s == State::Active) sawSelected = true;

    // Banner only on transitions; a beep on Active is the eyes-free "the reader
    // took the UID" signal, the same 150 ms / 2500 Hz the reader spike uses.
    static State lastState = State::None;
    if (s != lastState) {
        if (s == State::Active) M5.Speaker.tone(2500, 150);
        lastState = s;
        drawBanner(s);
        drawVerdict();
    }

    // Keyboard at 20 Hz.  Faster polling serves no one -- and the TCA8418 dies
    // under high-frequency I2C (docs/DEV-STATE.md section 13.1).
    const uint32_t now = millis();
    if (now - prevKeysMs >= 50) {
        prevKeysMs = now;
        M5Cardputer.update();
        handleKeys();
    }

    // Serial output only when no session can be in flight.
    if (s == State::None || s == State::Off) {
        flushTrace();
        if (now - lastBeat >= kHeartbeatMs) {
            lastBeat = now;
            printHeartbeat();
        }
    }
}

#endif  // RF_NFC_EMULATE
