/*
 * IR protocol engine: parsed <-> timing conversion.
 *
 * Pure C++ on purpose -- no Arduino.h, no allocation, no globals.  The host
 * tests in tests/host/test_ir.cpp compile this file straight into a native
 * binary, so an encoder or decoder regression is caught on the PC instead of
 * after a 2.5 minute cross-compile and a trip to the hardware.
 *
 * Model
 * -----
 * A parsed signal is (protocol, address, command) plus the small amount of
 * state the protocol needs (toggle for RC5/RC6, "this is a held key" for the
 * repeat code).  Timings are the same signal expressed as alternating
 * mark/space durations in microseconds, starting with a mark, which is what
 * the RMT transmitter eats and what the RMT receiver hands back.
 *
 * Carrier frequency belongs to the protocol (38 kHz for NEC, 40 for Sony,
 * 36 for Philips); a raw capture carries its own.
 *
 * Timing constants follow IRremoteESP8266 / Arduino-IRremote, whose values in
 * turn come from the respective vendor datasheets.
 */

#pragma once

#include <stdint.h>

namespace ir {

enum class Protocol : uint8_t {
    kUnknown = 0,
    kRaw,         // timing list only, no protocol
    kNec,         // 38 kHz, 32 bit: addr(8) ~addr(8) cmd(8) ~cmd(8)
    kNecExt,      // 38 kHz, 32 bit: addr(16) cmd(8) ~cmd(8), no integrity check
    kSamsung32,   // 38 kHz, 32 bit: addr(8) addr(8) cmd(8) ~cmd(8)
    kRc5,         // 36 kHz, 14 bit: S1 S2 T A(5) C(6)
    kRc5X,        // 36 kHz, 14 bit with 7 command bits
    kRc6,         // 36 kHz, 20 bit mode 0 (experimental, see ir_protocol.cpp)
    kSirc,        // 40 kHz, 12 bit: C(7) A(5)
    kSirc15,      // 40 kHz, 15 bit: C(8) A(7)
    kSirc20,      // 40 kHz, 20 bit: C(8) A(5) E(8)
    kKaseikyo,    // 37 kHz, 48 bit: vendor(16) genre/data/parity(32)
    kGree,        // 38 kHz, 64 bit: Gree / YAPOF air conditioner state
    kCount
};

/// Highest number of mark/space entries one encoded signal can occupy.  A raw
/// air-conditioner frame is roughly 200-600 entries; 512 keeps a capture in
/// one NVS blob while still covering single-frame A/C states.
constexpr int kMaxTimings = 512;

struct Signal {
    Protocol proto     = Protocol::kUnknown;
    uint32_t address   = 0;
    uint32_t command   = 0;
    bool     toggle    = false;  // RC5/RC6 toggle bit for the current press
    bool     repeat    = false;  // held key: use the protocol repeat frame
};

/// Alternating mark/space durations in microseconds, starting with a mark.
struct Timings {
    uint16_t us[kMaxTimings] = {0};
    int count                = 0;
    uint32_t frequency       = 38000;
    uint8_t dutyPercent      = 33;
};

/// Flipper-protocol name ("NEC", "SAMSUNG32", ... used in .ir files).
const char* protocolName(Protocol p);

/// Case-insensitive lookup; returns kUnknown for an unrecognised name.
Protocol protocolFromName(const char* name);

/// Carrier frequency a protocol is defined at.
uint32_t carrierHz(Protocol p);

/// True when this protocol can be built from address/command (i.e. is not raw).
bool encodable(Protocol p);

/// Encode a parsed signal into one full frame.  Returns false for kRaw,
/// kUnknown or a payload the protocol cannot represent.  `out.count` is left
/// at 0 on failure.
bool encode(const Signal& s, Timings* out);

/// Decode alternating mark/space durations into a parsed signal.  On failure
/// the output protocol is kUnknown (and remains kRaw-able to the caller).
bool decode(const uint16_t* us, int count, Signal* out);

/// Convenience: the repeat frame for a protocol, or the full frame when the
/// protocol repeats the whole payload.  Used for held keys.
bool encodeRepeat(const Signal& s, Timings* out);

/// Human readable "NEC A=0x04 C=0x08" style summary, for the UI and the log.
int describe(const Signal& s, char* out, int cap);

/// True when `name` is one of the protocols this firmware can synthesise.
bool protocolSupported(const char* name);

// ---------------------------------------------------------------------------
// Gree / YAPOF air conditioners
//
// Gree does not send "keys", it sends a complete state: every press is one
// 64-bit frame describing power, mode, temperature, fan and swing.  The layout
// and the checksum are the ones IRremoteESP8266's IRGreeAC uses (whose unit
// tests are recorded from real YAA1FBF / YAPOF remotes), which is also what
// every Gree-capable project implements.
//
// Air conditioners are the one place where a "learn one button" model does not
// work: the temperature lives inside the frame, so the app builds the state and
// sends the whole thing.
// ---------------------------------------------------------------------------
namespace gree {

constexpr int kStateBytes = 8;

constexpr uint8_t kModeAuto = 0;
constexpr uint8_t kModeCool = 1;
constexpr uint8_t kModeDry  = 2;
constexpr uint8_t kModeFan  = 3;
constexpr uint8_t kModeHeat = 4;

constexpr uint8_t kFanAuto = 0;
constexpr uint8_t kFanMin  = 1;
constexpr uint8_t kFanMed  = 2;
constexpr uint8_t kFanMax  = 3;

constexpr uint8_t kMinTempC = 16;
constexpr uint8_t kMaxTempC = 30;

// Two remote families exist in the wild, and they differ in two bytes only.
// Every capture in Flipper-IRDB's GREE directory uses YAPOF; IRremoteESP8266's
// recorded YAW1F frame uses the other.  Sending one when the unit wants the
// other gets no reaction at all, which is exactly the symptom to avoid.
enum Model : uint8_t {
    kModelYaw1F = 0,  // byte 2 ModelA = 1 while on, byte 5 = 0x20
    kModelYapof = 1,  // no ModelA, byte 5 = 0x40 (the WiFi bit)
};

struct State {
    uint8_t raw[kStateBytes] = {0};
};

/// Build a state.  `tempC` is clamped to 16..30, `mode`/`fan` to their ranges.
State makeState(bool power, uint8_t mode, uint8_t tempC, uint8_t fan, bool swing, bool light,
                uint8_t model = kModelYaw1F);

/// 4-bit block checksum Gree expects in the top nibble of the last byte
/// (Kelvinator's algorithm: start at 10, low nibbles of bytes 0-3, high nibbles
/// of bytes 4-6, modulo 16).
uint8_t checksum(const uint8_t* raw, int len = kStateBytes);

/// Recompute the checksum in place and return the state.
State& fixup(State& s);

bool valid(const State& s);

/// Pack into a Signal (address = bytes 0-3, command = bytes 4-7, both little
/// endian) and back.
Signal toSignal(const State& s);
bool fromSignal(const Signal& signal, State* out);

const char* modeName(uint8_t mode);
const char* fanName(uint8_t fan);

}  // namespace gree

}  // namespace ir
