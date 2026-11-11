// Dependency spike for the planned NFC app.  It answers exactly one question:
// do M5Unit-NFC and M5UnitUnified coexist with M5Cardputer@1.1.1 under
// espressif32@6.13.0 (Arduino core 2.x / IDF 4.4)?  The library's own
// Cardputer-ADV examples build against espressif32@6.12.0, one minor version
// below the pin this project cannot move off, so this has to be proven rather
// than assumed.
//
// Built only by `pio run -e nfc-spike`.  The shipping environments never define
// RF_NFC_SPIKE, so for them this is an empty translation unit.
//
// The API sequence mirrors M5Unit-NFC's examples/UnitUnified/NFCA/Detect, with
// two deliberate differences: M5Cardputer.begin() instead of M5.begin(),
// because that is what the real firmware calls, and no wiring::failStop(),
// because a spike that hangs on failure is useless.
#ifdef RF_NFC_SPIKE

#include <M5Cardputer.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include <wiring/m5_unit_unified_wiring.hpp>

#include <SD.h>
#include <SPIFFS.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "rf_config.h"

namespace {

using PICC  = m5::nfc::a::PICC;
using PICCB = m5::nfc::b::PICC;
using PICCF = m5::nfc::f::PICC;
using PICCV = m5::nfc::v::PICC;

// Same RGB565 literals src/app.cpp uses; the TFT_* macros are LovyanGFX's and
// this project does not use them.
constexpr uint16_t kBg   = 0x0000;
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kOk   = 0x07E0;
constexpr uint16_t kBad  = 0xF800;

// Fixed line positions, text size 1 (8 px per row).  The status block is
// redrawn in place rather than after fillScreen(): at a 2 s heartbeat a full
// clear would flicker, and a flickering screen is exactly what makes it hard to
// tell "nothing happened" from "I blinked and missed it".
constexpr int16_t kYStatus = 16;  // polls / hits counters
constexpr int16_t kYLog    = 24;  // storage target + write/fail counters
constexpr int16_t kYProto  = 32;  // per-protocol hit counters
constexpr int16_t kYRegs   = 40;  // ST25R3916 register snapshot
constexpr int16_t kYLast   = 48;  // last detected PICC (3 lines)
constexpr int16_t kYPrompt = 96;  // TAP A CARD / GOT n CARDS

m5::unit::UnitUnified units;

// CapST25R3916's default CS is already 6, but pass rfcfg::nfcCsPin explicitly:
// if the two ever disagree the spike should fail loudly here rather than talk
// to nothing.
m5::unit::CapCC1101NFC nfc{static_cast<uint8_t>(rfcfg::nfcCsPin)};

// All four reader layers over the one unit.
//
// ST25R3916 is single-protocol: unit.config().mode picks ONE protocol at
// begin(), and every transceive path rejects a mismatched mode -- CHECK_MODE at
// the top of unit_ST25R3916_nfc{a,b,f,v}.cpp returns false with "Illegal mode".
// The reader layers never switch it themselves, and the library ships no
// multi-protocol example: NFCB/NFCF/NFCV's Detect.ino each set cfg.mode once
// before begin().  So polling more than one protocol means driving the public
// configureNFCMode() between them, which is what enterProto() below does.
m5::nfc::NFCLayerA nfcA{nfc};
m5::nfc::NFCLayerB nfcB{nfc};
m5::nfc::NFCLayerF nfcF{nfc};
m5::nfc::NFCLayerV nfcV{nfc};

bool nfcReady = false;

// Protocol table.  Every array below is indexed by these, in this order.
enum : uint8_t { kProtoA = 0, kProtoB, kProtoF, kProtoV, kProtoCount };

const char* const kProtoName[kProtoCount]  = {"A", "B", "F", "V"};
const m5::nfc::NFC kProtoMode[kProtoCount] = {m5::nfc::NFC::A, m5::nfc::NFC::B,
                                              m5::nfc::NFC::F, m5::nfc::NFC::V};

// What REG_MODE_DEFINITION's om field (bits 5:3) should read back as after a
// successful switch, from InitiatorOperationMode in ST25R3916_definition.hpp:
// A = 0x01<<3, B = 0x02<<3, F = 0x03<<3, V = SubCarrierStream 0x0E<<3.  The
// low three bits are the nfc_ar / optional flags the configure paths also set,
// so the stored value is the whole register rather than just om.
const uint8_t kProtoOm[kProtoCount] = {0x08, 0x10, 0x18, 0x70};

// 0 = not yet evaluated, 1 = ok, 2 = failed.  Zero-init is what makes "not yet
// evaluated" the starting state without a constructor.
uint8_t kProtoCfgState[kProtoCount];
uint8_t kProtoReg[kProtoCount];     // REG_MODE_DEFINITION right after the switch
uint32_t kProtoPolls[kProtoCount];  // detect() calls actually issued
uint32_t kProtoHits[kProtoCount];   // PICCs found
uint32_t totalHits = 0;

// Per-protocol detect window.
//
// The library defaults are 1000 ms for A/B/V and 500 ms for F, which would make
// one pass over four protocols take ~3.5 s -- long enough that a one-second tap
// only ever meets whichever protocol happened to be polling, which is exactly
// the ambiguity this change exists to remove.  250 ms each keeps a pass at ~1 s,
// the cadence the A-only spike already proved works, and A's detect() retries
// REQA every 1 ms so 250 ms is ~250 attempts rather than a marginal window.
// F gets slightly more because Slot16 polling plus format identification is a
// longer sequence per iteration (nfc_layer_f.cpp:137).
constexpr uint32_t kDetectTimeoutMs  = 250;
constexpr uint32_t kDetectTimeoutFMs = 300;

// ---------------------------------------------------------------------------
// Capture log
//
// Everything is written to a file as it happens, because the point of this
// spike is to walk around with the board tapping things at cards that are not
// next to the host PC.  A record that only ever existed on the USB-CDC wire is
// a record that is gone: the port re-enumerates on reset, and a detection that
// lands between two capture windows cannot be recovered afterwards.
//
// SD is preferred -- pull the card and read it on any machine.  SPIFFS is the
// fallback so the log still exists with no card inserted, at the cost of having
// to read it back over serial.  Never NVS: docs/DEV-STATE.md section 12.1, the
// 20 KB NVS partition is the tightest non-growable resource on this board and
// holds the four irreplaceable RF records.
// ---------------------------------------------------------------------------
constexpr int kSdCsPin         = 12;  // same as tf_backup.cpp
constexpr uint32_t kSdHz       = 10000000;
constexpr char kLogDir[]       = "/nfc";
constexpr char kLogPath[]      = "/nfc/capture.log";

fs::FS* logFs      = nullptr;  // &SD, &SPIFFS, or null when neither mounted
const char* logFsName = "NONE";
uint32_t logWrites = 0;
uint32_t logFails  = 0;

// Last detection, kept in RAM so it survives into any later serial window even
// when no storage mounted.  This is the recovery path for the exact failure
// that motivated the file log above.  Protocol-scoped fields (ATQA/SAK, DSFID,
// ...) live only in the log line; what is re-broadcast here is the part that is
// common to all four, because a re-broadcast that only makes sense for NFC-A
// would misrepresent the other three.
char lastProto[4]  = "";
char lastUid[24]   = "";
char lastType[24]  = "";
uint32_t lastHitMs = 0;

// Register snapshot from the most recent heartbeat, so a detection line can
// carry the chip state it happened under without re-reading mid-transceive.
// REG_MODE_DEFINITION is deliberately absent: with four protocols per pass a
// live read here would only ever show whichever one ran last, which reads like
// a stuck chip.  kProtoReg[] holds the per-protocol values instead.
uint8_t regOpctl = 0, regTxDrv = 0;
bool regValid    = false;

// Open, append, close -- one syscall-ish burst per record rather than a
// long-lived handle.  Slower, but a bench tool gets its battery pulled at
// arbitrary moments and a buffered handle loses everything since the last
// flush when that happens.
bool logAppend(const char* fmt, ...)
{
    if (logFs == nullptr) return false;
    File f = logFs->open(kLogPath, FILE_APPEND);
    if (!f) {
        ++logFails;
        return false;
    }
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const bool ok = n > 0 && n < static_cast<int>(sizeof(buf)) && f.write(reinterpret_cast<uint8_t*>(buf), n) == n;
    f.close();
    if (ok) {
        ++logWrites;
    } else {
        ++logFails;
    }
    return ok;
}

void mountLog()
{
    // Mirrors tf_backup.cpp: cardType() is only meaningful once begun, but
    // short-circuits the re-mount on a second call.
    if (SD.cardType() != CARD_NONE || SD.begin(kSdCsPin, SPI, kSdHz)) {
        logFs     = &SD;
        logFsName = "SD";
    } else {
        SD.end();
        // format=false first: the partition is documented as unused, so it may
        // already hold a log from a previous session that we want to append to
        // rather than erase.
        if (SPIFFS.begin(false) || SPIFFS.begin(true)) {
            logFs     = &SPIFFS;
            logFsName = "SPIFFS";
        }
    }
    if (logFs == nullptr) return;
    if (!logFs->exists(kLogDir)) logFs->mkdir(kLogDir);
}

// Re-broadcast the whole log to serial on a timer.
//
// Without this the SPIFFS fallback is a dead end: no SD card means the only way
// back in is serial, and a boot-time dump is useless because the board boots the
// moment it is plugged in, before the host has opened the CDC port.  A periodic
// dump needs no button, no key press and no timing on the operator's part -- any
// capture window longer than the interval recovers the entire history.
//
// Deliberately not tied to the keyboard: src/app.cpp:83 installs a custom
// PollingKeyboardReader because the stock path has an unresolved freeze
// (docs/DEV-STATE.md section 4), and a spike has no business inheriting that.
constexpr uint32_t kLogDumpEveryMs = 60000;
constexpr uint32_t kLogDumpMaxBytes = 65536;  // tail-only beyond this

void dumpLog()
{
    if (logFs == nullptr) return;
    File f = logFs->open(kLogPath, FILE_READ);
    if (!f) return;

    const uint32_t size = f.size();
    const uint32_t skip = size > kLogDumpMaxBytes ? size - kLogDumpMaxBytes : 0;
    Serial.printf("[spike] === LOG BEGIN fs=%s path=%s size=%u shown=%u ===\n", logFsName, kLogPath,
                  static_cast<unsigned>(size), static_cast<unsigned>(size - skip));
    if (skip > 0) {
        f.seek(skip);
        // Discard the partial first line so every emitted line is parseable.
        while (f.available() && f.read() != '\n') {
        }
    }
    char buf[256];
    while (f.available() > 0) {
        const int n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        Serial.print(buf);
    }
    f.close();
    Serial.printf("[spike] === LOG END ===\n");
}

// ---------------------------------------------------------------------------
// Multi-protocol polling
// ---------------------------------------------------------------------------

// Switch the chip to protocol `pi` and record whether the switch took.
//
// A failed configureNFCMode() and "no card of this protocol in the field"
// produce exactly the same observable -- detect() returning false -- so the
// switch result has to be recorded separately or the log is uninterpretable.
// It is logged only on state change: a permanently failing protocol would
// otherwise write four lines a second into a 1.5 MB partition.
void enterProto(const uint8_t pi)
{
    // Field off before switching, and this is load-bearing rather than tidy-up.
    //
    // All four configure_nfc_x() functions end with nfc_initial_field_on()
    // (unit_ST25R3916_nfca.cpp:167, nfcb.cpp:93, nfcf.cpp:66, nfcv.cpp:87), and
    // that function bails out with "Already tx_en" if REG_OPERATION_CONTROL's
    // tx_en is already set -- so it fails, configure_nfc_x() returns false, and
    // configureNFCMode() reports failure.  configureNFCMode() does issue
    // CMD_STOP_ALL_ACTIVITIES first, but that halts the state machine; it does
    // NOT clear tx_en.  The consequence of skipping this is that only the first
    // protocol in a pass ever configures, and the other three fail every time.
    //
    // `en` (bit7, oscillator + regulator) is deliberately preserved: it is not
    // part of field on/off, and configure_nfc_v() does a read-modify-write on
    // this same register (set_bit_register8 for en_fd_c1|en_fd_c0), so clearing
    // it here would be undone or would leave the chip unpowered.
    namespace rv = m5::unit::st25r3916::regval;
    uint8_t opctl = 0;
    if (nfc.readOperationControl(opctl)) {
        nfc.writeOperationControl(static_cast<uint8_t>(opctl & ~(rv::tx_en | rv::rx_en)));
    }

    const bool ok   = nfc.configureNFCMode(kProtoMode[pi]);
    const uint8_t s = ok ? 1 : 2;
    if (s != kProtoCfgState[pi]) {
        kProtoCfgState[pi] = s;
        logAppend("M ms=%u proto=%s cfg=%d\n", static_cast<unsigned>(millis()), kProtoName[pi],
                  static_cast<int>(ok));
    }
    if (!ok) {
        return;
    }

    uint8_t mode = 0;
    if (nfc.readModeDefinition(mode)) {
        kProtoReg[pi] = mode;
        // om is bits 5:3; a mismatch means configure_nfc_x() wrote something
        // other than the protocol it was asked for, which would show up as
        // "no hits" and be blamed on the card.
        if ((mode & 0x78) != kProtoOm[pi]) {
            logAppend("M ms=%u proto=%s WARN reg=%02X expected om=%02X\n",
                      static_cast<unsigned>(millis()), kProtoName[pi], mode, kProtoOm[pi]);
        }
    }

    // configureNFCMode() issues CMD_STOP_ALL_ACTIVITIES, which tears the field
    // down; give the oscillator and antenna a beat before the first poll so a
    // card is not asked to power up into a settling field.
    delay(2);
}

// Shared tail for a hit on any protocol: counters, screen, beep, and the RAM
// copy that later serial windows re-broadcast.  Returns the gap since the
// previous hit on ANY protocol so the caller can put it in its own log line.
//
// `id` is whatever the standard calls its identifier -- A and V a UID, B a
// PUPI, F an IDm -- so it is named for the role rather than for one protocol.
uint32_t noteHit(const uint8_t pi, const uint32_t ms, const char* id, const char* type,
                 const char* detail)
{
    const uint32_t gap = lastHitMs == 0 ? 0 : ms - lastHitMs;
    lastHitMs          = ms;
    ++kProtoHits[pi];
    ++totalHits;
    snprintf(lastProto, sizeof(lastProto), "%s", kProtoName[pi]);
    snprintf(lastUid, sizeof(lastUid), "%s", id);
    snprintf(lastType, sizeof(lastType), "%s", type);

    auto& lcd = M5Cardputer.Display;
    lcd.setTextColor(kOk, kBg);
    lcd.setCursor(0, kYLast);
    lcd.printf("%s id %-21s\n%-25s\n%-39s", kProtoName[pi], id, type, detail);
    lcd.setCursor(0, kYPrompt);
    lcd.printf("GOT %-2u CARDS               ", static_cast<unsigned>(totalHits));
    lcd.setTextColor(kText, kBg);

    // Long enough to be unmistakably the board's own speaker rather than a
    // phone beeping at an NFC field.
    M5.Speaker.tone(2500, 150);
    return gap;
}

// One line per detection, no dedup: a card held in the field produces a record
// per pass and that is genuinely what happened.  `gap` is recorded so a burst
// can be collapsed offline instead of guessed at.
void pollA()
{
    ++kProtoPolls[kProtoA];
    std::vector<PICC> piccs;
    if (nfcA.detect(piccs, kDetectTimeoutMs)) {
        for (auto& p : piccs) {
            const uint32_t ms  = millis();
            const std::string id = p.uidAsString();

            // detect() only classifies provisionally from SAK, so identify() is
            // what makes the type string mean anything.
            if (!nfcA.identify(p)) {
                Serial.printf("[spike] A identify failed uid=%s\n", id.c_str());
                // Logged too.  "全部采集信息" includes the ones that did not
                // classify: a PICC that answers REQA and anti-collision but
                // fails identify() is exactly the interesting failure, and it
                // is invisible if only successes are written down.
                logAppend("F ms=%u proto=A uid=%s atqa=%04X sak=%02X uidLen=%u\n",
                          static_cast<unsigned>(ms), id.c_str(), static_cast<unsigned>(p.atqa),
                          static_cast<unsigned>(p.sak), static_cast<unsigned>(p.size));
                auto& lcd = M5Cardputer.Display;
                lcd.setTextColor(kBad, kBg);
                lcd.setCursor(0, kYLast);
                lcd.printf("A IDENTIFY FAILED      \n%-25s\n%-39s", id.c_str(), "");
                lcd.setTextColor(kText, kBg);
                continue;
            }

            const std::string type = p.typeAsString();
            char detail[40];
            snprintf(detail, sizeof(detail), "atqa=%04X sak=%02X len=%u",
                     static_cast<unsigned>(p.atqa), static_cast<unsigned>(p.sak),
                     static_cast<unsigned>(p.size));

            Serial.printf("[spike] A uid=%s type=%s atqa=%04X sak=%02X user=%u total=%u\n",
                          id.c_str(), type.c_str(), static_cast<unsigned>(p.atqa),
                          static_cast<unsigned>(p.sak), static_cast<unsigned>(p.userAreaSize()),
                          static_cast<unsigned>(p.totalSize()));

            // uidLen matters because NFC-A emulation only accepts 4 or 7 byte
            // UIDs (emulation_layer_a_ST25R3916.cpp:142), so whether a read UID
            // is replayable at all is decided by this field.
            const uint32_t gap = noteHit(kProtoA, ms, id.c_str(), type.c_str(), detail);
            logAppend("H ms=%u proto=A id=%s type=%s atqa=%04X sak=%02X uidLen=%u user=%u "
                      "total=%u gap=%u heap=%u opctl=%02X mode=%02X\n",
                      static_cast<unsigned>(ms), id.c_str(), type.c_str(),
                      static_cast<unsigned>(p.atqa), static_cast<unsigned>(p.sak),
                      static_cast<unsigned>(p.size), static_cast<unsigned>(p.userAreaSize()),
                      static_cast<unsigned>(p.totalSize()), static_cast<unsigned>(gap),
                      static_cast<unsigned>(ESP.getFreeHeap()), regOpctl, kProtoReg[kProtoA]);
        }
    }
    // deactivate()'s documented precondition is a PICC in the ACTIVE state, and
    // calling it with nothing selected is not a harmless no-op: NFCLayerB emits
    // "Failed to hlt" and "Failed to deselecte" on every pass
    // (nfc_layer_b.cpp:182,201), which in a capture reads like a hardware fault
    // rather than an idle field.  detect() returns !piccs.empty(), so this is
    // exactly "only deactivate what was activated".
    if (!piccs.empty()) nfcA.deactivate();
}

void pollB()
{
    ++kProtoPolls[kProtoB];
    std::vector<PICCB> piccs;
    // afi=0x00 is the wildcard.  A non-zero AFI would filter out every card not
    // carrying that application family, which is the opposite of what a "what
    // is this thing" probe wants.
    if (nfcB.detect(piccs, 0x00, 4, kDetectTimeoutMs)) {
        for (auto& p : piccs) {
            const uint32_t ms    = millis();
            const std::string id   = p.pupiAsString();
            const std::string type = p.typeAsString();
            char detail[40];
            snprintf(detail, sizeof(detail), "iso14443-4=%d cid=%u",
                     static_cast<int>(p.isISO14443_4()), static_cast<unsigned>(p.cid));

            Serial.printf("[spike] B pupi=%s type=%s %s\n", id.c_str(), type.c_str(), detail);

            const uint32_t gap = noteHit(kProtoB, ms, id.c_str(), type.c_str(), detail);
            logAppend("H ms=%u proto=B id=%s type=%s iso14443_4=%d cid=%u gap=%u heap=%u "
                      "opctl=%02X mode=%02X\n",
                      static_cast<unsigned>(ms), id.c_str(), type.c_str(),
                      static_cast<int>(p.isISO14443_4()), static_cast<unsigned>(p.cid),
                      static_cast<unsigned>(gap), static_cast<unsigned>(ESP.getFreeHeap()),
                      regOpctl, kProtoReg[kProtoB]);
        }
    }
    if (!piccs.empty()) nfcB.deactivate();
}

void pollF()
{
    ++kProtoPolls[kProtoF];
    std::vector<PICCF> piccs;
    // Slot16 is the widest polling time slot, and detect() opens with the
    // wildcard system code (nfc_layer_f.cpp:158), so nothing is filtered out.
    // The private_code overload is skipped by passing no private codes.
    if (nfcF.detect(piccs, m5::nfc::f::TimeSlot::Slot16, kDetectTimeoutFMs)) {
        for (auto& p : piccs) {
            const uint32_t ms    = millis();
            const std::string id   = p.idmAsString();
            const std::string pmm  = p.pmmAsString();
            const std::string type = p.typeAsString();
            char detail[40];
            snprintf(detail, sizeof(detail), "user=%u valid=%d",
                     static_cast<unsigned>(p.userAreaSize()), static_cast<int>(p.valid()));

            Serial.printf("[spike] F idm=%s pmm=%s type=%s %s\n", id.c_str(), pmm.c_str(),
                          type.c_str(), detail);

            const uint32_t gap = noteHit(kProtoF, ms, id.c_str(), type.c_str(), detail);
            logAppend("H ms=%u proto=F id=%s pmm=%s type=%s user=%u valid=%d gap=%u heap=%u "
                      "opctl=%02X mode=%02X\n",
                      static_cast<unsigned>(ms), id.c_str(), pmm.c_str(), type.c_str(),
                      static_cast<unsigned>(p.userAreaSize()), static_cast<int>(p.valid()),
                      static_cast<unsigned>(gap), static_cast<unsigned>(ESP.getFreeHeap()),
                      regOpctl, kProtoReg[kProtoF]);
        }
    }
    if (!piccs.empty()) nfcF.deactivate();
}

void pollV()
{
    ++kProtoPolls[kProtoV];
    std::vector<PICCV> piccs;
    if (nfcV.detect(piccs, kDetectTimeoutMs)) {
        for (auto& p : piccs) {
            const uint32_t ms    = millis();
            const std::string id   = p.uidAsString();
            const std::string type = p.typeAsString();
            char detail[40];
            snprintf(detail, sizeof(detail), "dsf=%02X afi=%02X b=%u/%u",
                     static_cast<unsigned>(p.dsfID), static_cast<unsigned>(p.afi),
                     static_cast<unsigned>(p.blocks), static_cast<unsigned>(p.block_size));

            Serial.printf("[spike] V uid=%s type=%s %s valid=%d\n", id.c_str(), type.c_str(),
                          detail, static_cast<int>(p.valid()));

            const uint32_t gap = noteHit(kProtoV, ms, id.c_str(), type.c_str(), detail);
            // validUID() requires uid[0] == 0xE0 (nfcv.hpp:107); logged because
            // a detected-but-invalid UID is the difference between "read it" and
            // "heard something".
            logAppend("H ms=%u proto=V id=%s type=%s dsfID=%02X afi=%02X blocks=%u bs=%u "
                      "validUID=%d gap=%u heap=%u opctl=%02X mode=%02X\n",
                      static_cast<unsigned>(ms), id.c_str(), type.c_str(),
                      static_cast<unsigned>(p.dsfID), static_cast<unsigned>(p.afi),
                      static_cast<unsigned>(p.blocks), static_cast<unsigned>(p.block_size),
                      static_cast<int>(p.validUID()), static_cast<unsigned>(gap),
                      static_cast<unsigned>(ESP.getFreeHeap()), regOpctl, kProtoReg[kProtoV]);
        }
    }
    if (!piccs.empty()) nfcV.deactivate();
}

}  // namespace

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    auto& lcd = M5Cardputer.Display;
    lcd.fillScreen(kBg);
    lcd.setTextSize(1);
    lcd.setCursor(0, 0);

    // ST25R3916 is SPI mode 1 at 10 MHz; the CC1101 on the same bus is mode 0.
    // addSPI() only calls SPI.begin() when the bus is not already begun, so the
    // pin assignment made by cc1101.cpp is left alone.
    //
    // cfg.mode is set explicitly instead of left at its default: the pass below
    // polls four protocols by switching modes, so which one the chip starts in
    // has to be known rather than assumed.  config() is applied at begin().
    auto ucfg = nfc.config();
    ucfg.mode = m5::nfc::NFC::A;
    nfc.config(ucfg);

    const bool wired = m5::unit::wiring::addSPI(units, nfc, 10000000, 1);
    nfcReady         = wired && units.begin();

    Serial.printf("[spike] addSPI=%d units.begin=%d\n", static_cast<int>(wired),
                  static_cast<int>(nfcReady));
    if (nfcReady) {
        Serial.printf("[spike] %s\n", units.debugInfo().c_str());
    }

    // Mounted after the NFC unit so addSPI() has already begun the bus with the
    // board's own pin table; SD shares that bus on its own CS (G12), the same
    // arrangement the shipping firmware uses for CC1101(G5) + SD(G12).
    mountLog();
    Serial.printf("[spike] log fs=%s path=%s\n", logFsName, kLogPath);

    // No RTC on this board, so a wall-clock timestamp is not available.  The
    // session header is what makes the appended log self-describing: each boot
    // is delimited, and the register snapshot in it is the chip state the whole
    // session started from.  `protos` and `t` record what was being polled and
    // with what window, because a log with zero hits means something completely
    // different depending on whether the protocol in question was polled at all.
    if (logFs != nullptr) {
        uint8_t opctl = 0, mode = 0, txdrv = 0;
        const bool regs = nfcReady && nfc.readOperationControl(opctl) &&
                          nfc.readModeDefinition(mode) && nfc.readTXDriver(txdrv);
        logAppend("# session ms=%u nfcReady=%d protos=ABFV t=%u/%u regs=%d opctl=%02X "
                  "mode=%02X txdrv=%02X free=%u\n",
                  static_cast<unsigned>(millis()), static_cast<int>(nfcReady),
                  static_cast<unsigned>(kDetectTimeoutMs), static_cast<unsigned>(kDetectTimeoutFMs),
                  static_cast<int>(regs), opctl, mode, txdrv,
                  static_cast<unsigned>(ESP.getFreeHeap()));
    }

    lcd.setTextColor(nfcReady ? kOk : kBad, kBg);
    lcd.setCursor(0, 0);
    lcd.printf("NFC SPIKE  begin=%d", static_cast<int>(nfcReady));
    lcd.setTextColor(logFs != nullptr ? kOk : kBad, kBg);
    lcd.setCursor(0, kYLog);
    lcd.printf("log=%-6s %s", logFsName, logFs != nullptr ? "" : "(NO STORAGE)");
    if (!nfcReady) {
        lcd.setTextColor(kBad, kBg);
        lcd.setCursor(0, kYLast);
        lcd.printf("ST25R3916 not\nanswering on CS G%d", rfcfg::nfcCsPin);
    } else {
        lcd.setTextColor(kText, kBg);
        lcd.setCursor(0, kYPrompt);
        lcd.printf("TAP A CARD  A/B/F/V");
    }
    lcd.setTextColor(kText, kBg);
}

void loop()
{
    M5Cardputer.update();

    // The USB-CDC port re-enumerates on every reset, so a capture started after
    // the board comes up misses setup()'s one-shot report.  Repeat it here so
    // the answer is on the wire whenever somebody happens to be listening.
    //
    // `loops` proves the loop is actually spinning -- detect() blocks for up to
    // its timeout on every call, so a stalled loop and a card-free field look
    // identical without it.
    //
    // Do NOT add a raw nfcA.request() probe here to tell "field dead" from
    // "anti-collision failed": request() leaves the PICC in READY, and REQA is
    // only answered by IDLE cards, so the probe would poison the detect() that
    // follows.  Build with -DM5_LOG_LEVEL=5 instead; NFCLayerA::detect() already
    // logs ATQA at verbose level, which makes the same distinction with no
    // interference.
    static uint32_t lastBeat = 0;
    static uint32_t lastDump = 0;
    static uint32_t loops    = 0;
    ++loops;

    // First dump at ~5 s rather than a full interval in: the board boots the
    // instant it is plugged in, so an early dump is what makes a short capture
    // window enough to recover the whole history.
    if (lastDump == 0 ? millis() > 5000 : millis() - lastDump >= kLogDumpEveryMs) {
        lastDump = millis();
        dumpLog();
    }

    auto& lcd = M5Cardputer.Display;

    if (millis() - lastBeat >= 2000) {
        lastBeat = millis();
        Serial.printf("[spike] ready=%d loops=%u hits=%u heap=%u min=%u\n",
                      static_cast<int>(nfcReady), static_cast<unsigned>(loops),
                      static_cast<unsigned>(totalHits), static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMinFreeHeap()));

        // Card-free health probe.  `loops` advancing ~1/s means the pass is
        // burning its whole window, i.e. nothing ever answers -- which looks
        // identical whether the field is dead or nobody tapped a card.  These
        // registers settle it without a card:
        //   opctl bit7 (en)    = oscillator + regulator up.  Unlike tx_en/rx_en
        //                        it is NOT toggled per transceive, so reading it
        //                        between detect() calls is meaningful.
        //   txdrv              = antenna drive level; 0x00 means nothing is
        //                        being driven regardless of tx_en.
        // tx_en is printed because it turned out to read 1 steadily here, but
        // `en` is the load-bearing one: unit_ST25R3916.cpp does bracket some
        // paths with field-on/field-off, so tx_en is the bit that could
        // legitimately read 0 between operations and mislead.
        //
        // REG_MODE_DEFINITION is NOT read live any more.  With four protocols
        // per pass a live read only ever shows whichever one ran last (V),
        // which reads exactly like a chip stuck in the wrong mode.  The
        // per-protocol values captured at each switch are the meaningful ones,
        // and `targ` is verified there instead -- configureNFCMode() writes an
        // InitiatorOperationMode, whose targ bit is 0 by construction.
        if (nfcReady) {
            uint8_t opctl = 0, txdrv = 0;
            regValid = nfc.readOperationControl(opctl) && nfc.readTXDriver(txdrv);
            if (regValid) {
                regOpctl = opctl;
                regTxDrv = txdrv;
            }
            namespace rv = m5::unit::st25r3916::regval;
            Serial.printf("[spike] regs ok=%d opctl=%02X(en=%d tx_en=%d) txdrv=%02X\n",
                          static_cast<int>(regValid), opctl, static_cast<int>((opctl & rv::en) != 0),
                          static_cast<int>((opctl & rv::tx_en) != 0), txdrv);
            Serial.printf("[spike] mode A=%02X B=%02X F=%02X V=%02X (expect om %02X %02X %02X %02X)\n",
                          kProtoReg[kProtoA], kProtoReg[kProtoB], kProtoReg[kProtoF],
                          kProtoReg[kProtoV], kProtoOm[kProtoA], kProtoOm[kProtoB], kProtoOm[kProtoF],
                          kProtoOm[kProtoV]);
        }

        // polls == 0 for a protocol means enterProto() failed to configure it
        // and it was never polled at all, which is a completely different
        // statement from "polled and nothing answered".
        Serial.printf("[spike] proto A cfg=%d polls=%u hits=%u | B %d %u %u | F %d %u %u | V %d %u %u\n",
                      static_cast<int>(kProtoCfgState[kProtoA]),
                      static_cast<unsigned>(kProtoPolls[kProtoA]),
                      static_cast<unsigned>(kProtoHits[kProtoA]),
                      static_cast<int>(kProtoCfgState[kProtoB]),
                      static_cast<unsigned>(kProtoPolls[kProtoB]),
                      static_cast<unsigned>(kProtoHits[kProtoB]),
                      static_cast<int>(kProtoCfgState[kProtoF]),
                      static_cast<unsigned>(kProtoPolls[kProtoF]),
                      static_cast<unsigned>(kProtoHits[kProtoF]),
                      static_cast<int>(kProtoCfgState[kProtoV]),
                      static_cast<unsigned>(kProtoPolls[kProtoV]),
                      static_cast<unsigned>(kProtoHits[kProtoV]));

        Serial.printf("[spike] log fs=%s writes=%u fails=%u\n", logFsName,
                      static_cast<unsigned>(logWrites), static_cast<unsigned>(logFails));
        // Re-broadcast the last detection on every heartbeat.  Costs one line
        // and means a capture window opened at any later time still recovers
        // what was read, which is the failure mode that hit twice on the bench.
        if (totalHits > 0) {
            Serial.printf("[spike] last proto=%s id=%s type=%s atMs=%u\n", lastProto, lastUid,
                          lastType, static_cast<unsigned>(lastHitMs));
        }

        // Mirrored on screen for the same reason as the serial heartbeat: a
        // bench session where the only evidence is a serial scrollback is easy
        // to misread, and "did that beep come from the board or from the phone
        // I was holding against it" is a real ambiguity here.  The polls
        // counter visibly advancing is what proves the loop is alive.
        //
        // The per-protocol line is the part that actually answers the question
        // this build exists for: `!` means that protocol could not be
        // configured, so a zero there is not evidence of anything.
        lcd.setTextColor(kText, kBg);
        lcd.setCursor(0, kYStatus);
        lcd.printf("polls=%-6u hits=%-6u", static_cast<unsigned>(loops),
                   static_cast<unsigned>(totalHits));
        lcd.setCursor(0, kYLog);
        lcd.setTextColor(logFs != nullptr ? kText : kBad, kBg);
        lcd.printf("%-6s w=%-4u f=%-4u", logFsName, static_cast<unsigned>(logWrites),
                   static_cast<unsigned>(logFails));
        lcd.setTextColor(kText, kBg);
        lcd.setCursor(0, kYProto);
        for (uint8_t i = 0; i < kProtoCount; ++i) {
            lcd.printf("%s=%-3u%c", kProtoName[i], static_cast<unsigned>(kProtoHits[i]),
                       kProtoCfgState[i] == 2 ? '!' : ' ');
        }
        lcd.setCursor(0, kYRegs);
        if (regValid) {
            lcd.printf("op=%02X tx=%02X m=%02X.%02X.%02X.%02X", regOpctl, regTxDrv,
                       kProtoReg[kProtoA], kProtoReg[kProtoB], kProtoReg[kProtoF],
                       kProtoReg[kProtoV]);
        } else {
            lcd.printf("regs: n/a                            ");
        }
    }

    if (!nfcReady) {
        delay(200);
        return;
    }

    units.update();

    // All four protocols on every pass, rather than rotating one per pass: with
    // rotation a card held for one second only ever meets one protocol, which
    // reproduces the ambiguity this change exists to remove.
    //
    // The blue square fob (MIFARE Classic 1K, protocol A) is the control.  Mode
    // switching is new to this build and could in principle have broken the one
    // protocol that was already proven; if the square fob still reads as A, the
    // switching is exonerated and a zero on B/F/V means what it says.
    enterProto(kProtoA);
    if (kProtoCfgState[kProtoA] == 1) pollA();
    enterProto(kProtoB);
    if (kProtoCfgState[kProtoB] == 1) pollB();
    enterProto(kProtoF);
    if (kProtoCfgState[kProtoF] == 1) pollF();
    enterProto(kProtoV);
    if (kProtoCfgState[kProtoV] == 1) pollV();
}

#endif  // RF_NFC_SPIKE
