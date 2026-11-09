/*
 * CAP.CC1101 on the M5Stack Cardputer ADV.
 *
 * The Cap plugs into the EXT 2.54-14P header and shares the SPI bus with the
 * microSD slot.  Pin mapping taken from the M5Stack Cap CC1101 documentation:
 *
 *   CC1101 CS     -> G5    (EXT pin 13)
 *   CC1101 GDO0   -> G15   (EXT pin 14)   demodulated OOK data / TX data in
 *   CC1101 RF_SW0 -> G13   (EXT pin 12)   antenna switch, plain GPIO
 *   SPI SCK       -> G40   (EXT pin 7)
 *   SPI MOSI      -> G14   (EXT pin 9)
 *   SPI MISO      -> G39   (EXT pin 11)
 *   NFC CS        -> G6    (EXT pin 5)    keep high, otherwise the ST25R3916
 *                                         answers on the same SPI bus
 *
 * RF switch truth table (from the Cap CC1101 schematic V0.3):
 *   RF_SW0=0 RF_SW1=1 -> 315 MHz
 *   RF_SW0=1 RF_SW1=0 -> 433 MHz
 *   RF_SW0=1 RF_SW1=1 -> 868/915 MHz
 * RF_SW1 is not a host pin: it is CC1101 GDO2, driven through its IOCFG2
 * register (HW-to-0 = 0x2E, HW-to-2 with invert = 0x6E).
 */

#pragma once

#include <Arduino.h>

// Verified on real hardware: the CC1101 answers on CS = G5, the on-board NFC
// controller (ST25R3916) sits on G6 and is kept deselected.
#define RF_CC_CS   5
#define RF_CC_GDO0 15
#define RF_CC_SW0  13
#define RF_NFC_CS  6
#define RF_CC_SCK  40
#define RF_CC_MISO 39
#define RF_CC_MOSI 14

// Cardputer ADV keyboard controller (TCA8418) interrupt line.  Not part of the
// CC1101 cap, but it is the pin the stock M5Cardputer reader attaches an ISR to,
// so it has to be detached before the polling reader takes over - see
// App::begin() and docs/DEV-STATE.md section 3.
#define RF_KBD_INT 11

// A low level longer than this separates two transmissions.  Must stay above
// the longest sync pulse (31 * T, ~14 ms for a slow remote).
#define RF_FRAME_GAP_US 18000

// Pulses shorter than this are demodulator noise and get merged away.
#define RF_NOISE_US 150

// Pulses kept for one burst.  A single 24 bit EV1527 frame is exactly 50
// pulses; a held remote produces a burst of many back-to-back frames.
#define RF_MAX_FRAME_PULSES 300

// Pulses kept per stored capture.  CaptureStore now keeps exactly one frame
// (50 pulses), but entries written by older firmware hold whole multi-frame
// trains, so the array stays generous enough to load them unchanged.
#define RF_STORED_PULSES 160

// Number of captures kept in RAM / NVS.
#define RF_MAX_CAPTURES 16

// Number of permanently saved ("永久保存") signals.
#define RF_MAX_SAVED 8

// How often a pending hit-counter change is written to NVS, in milliseconds.
// Holding a remote bumps the counter several times a second and each write
// rewrites the whole capture blob, so the writes are batched.  Losing at most
// this much of a counter is acceptable; it is cosmetic.
#define RF_STORE_FLUSH_MS 30000

// Minimum number of times one stored train is transmitted per "send".
// Receivers act only on several *consecutive* identical frames (see
// docs/DEBUG-2026-11-08.md), so this is a floor and not a target.
#define RF_TX_REPEATS 3

// How long one "send" lasts on air, in milliseconds.  A person holds the remote
// for roughly 0.5 - 2 s; longer bursts mean more consecutive frames, and every
// extra frame is another chance to get through at long range (3rd floor to a
// gate on the 1st floor is a lot of concrete).  The repeat count is derived
// from the measured length of the stored train so the on-air time stays fixed.
#define RF_TX_TARGET_MS 700

// Safety cap on the derived repeat count (and therefore on the RMT item buffer).
#define RF_TX_MAX_REPEATS 40

// Transmit backend: 1 = RMT hardware timing (preferred), 0 = software toggling
// with delayMicroseconds().  The software path is kept as a fallback and as an
// A/B reference; it blocks the CPU for the whole transmission and jitters by a
// few microseconds per pulse.
#define RF_TX_USE_RMT 1

// Panel rotation: 0 = landscape (240x135).
#define RF_SCREEN_ROTATION 0

enum class RfBand : uint8_t {
    k315 = 0,
    k433 = 1,
    k868 = 2,
};



namespace rfcfg {

constexpr int csPin   = RF_CC_CS;
constexpr int gdo0Pin = RF_CC_GDO0;
constexpr int sw0Pin  = RF_CC_SW0;
constexpr int nfcCsPin = RF_NFC_CS;
constexpr int sckPin  = RF_CC_SCK;
constexpr int misoPin = RF_CC_MISO;
constexpr int mosiPin = RF_CC_MOSI;

constexpr uint32_t frameGapUs = RF_FRAME_GAP_US;
constexpr uint32_t noiseUs    = RF_NOISE_US;
constexpr int maxFramePulses  = RF_MAX_FRAME_PULSES;
constexpr int storedPulses    = RF_STORED_PULSES;
constexpr int maxCaptures     = RF_MAX_CAPTURES;
constexpr int maxSaved        = RF_MAX_SAVED;
constexpr uint32_t storeFlushMs = RF_STORE_FLUSH_MS;
constexpr int txRepeats       = RF_TX_REPEATS;
constexpr int txTargetMs      = RF_TX_TARGET_MS;
constexpr int txMaxRepeats    = RF_TX_MAX_REPEATS;
constexpr int screenRotation  = RF_SCREEN_ROTATION;

// One probe point of the RSSI scanner.
struct ScanPoint {
    uint32_t hz;
    RfBand band;
};

// Common sub-GHz channels around the three bands this cap can switch to.  The
// receiver is tuned to each of them in turn and the RSSI register is read, so a
// remote can be located without guessing which band/preset it uses.
static const ScanPoint kScanPoints[] = {
    {315000000u, RfBand::k315},
    {433075000u, RfBand::k433},
    {433420000u, RfBand::k433},
    {433920000u, RfBand::k433},
    {434420000u, RfBand::k433},
    {868350000u, RfBand::k868},
    {868950000u, RfBand::k868},
};
static const int kScanCount = static_cast<int>(sizeof(kScanPoints) / sizeof(kScanPoints[0]));

// Anything at or above this RSSI is treated as "a transmitter is here".
#define RF_SCAN_ACTIVE_DBM (-72)

// Boot-time scan sweep used to bring the scanner up; off by default, enable for
// one build with -DRF_SCAN_SELFTEST=1.
#ifndef RF_SCAN_SELFTEST
#define RF_SCAN_SELFTEST 0
#endif

// Loopback bring-up aid: transmit a known frame with the RMT backend and decode
// it back through the interrupt receiver on the same pin.  Needs no remote.
// Static GDO0 wiring check: drive the pin high/low through IOCFG0 and read it.
#ifndef RF_GDO_TEST
#define RF_GDO_TEST 0
#endif

#ifndef RF_RX_SELFTEST
#define RF_RX_SELFTEST 0
#endif

// Serial diagnostics and the on-screen "is the UI still alive" blink that were
// added while chasing the keyboard freeze (docs/DEV-STATE.md section 3).  Left
// on by default so nothing about current behaviour changes silently; building
// with -DRF_DIAG=0 is the section 4 experiment - if the keyboard still dies
// without the 1 Hz forced redraw, the freeze was never a redraw problem.
// trace::dumpAfterReset() is deliberately NOT gated: it is the only output that
// survives a panic.
#ifndef RF_DIAG
#define RF_DIAG 1
#endif

inline const char* bandName(RfBand band)
{
    switch (band) {
        case RfBand::k315: return "315";
        case RfBand::k433: return "433";
        case RfBand::k868: return "868";
    }
    return "???";
}

}  // namespace rfcfg
