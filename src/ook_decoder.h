/*
 * Decoder for the OOK remote control protocols used by most 433.92 MHz
 * rolling shutter / sliding gate remotes sold in China.
 *
 * Both families share the same on-air shape: a long sync pulse followed by a
 * series of pulse pairs whose high and low lengths are 1T / 3T of a base time
 * (T is typically 250 .. 450 us).
 *
 *   EV1527  : sync = 1T high + 31T low, then 24 bits, MSB first.
 *             bit 0 -> 1T high + 3T low, bit 1 -> 3T high + 1T low.
 *             The 24 bits are a 20 bit address plus a 4 bit button code.
 *
 *   PT2262  : identical waveform, but only 12 tri-state symbols are sent.
 *             A symbol can be 0 (1T/3T), 1 (3T/1T) or floating (1T/1T).
 *
 * Because the waveform is the same, a frame is first parsed as a run of pulse
 * pairs and only afterwards classified by the number of pairs and by whether
 * floating symbols were seen.
 */

#pragma once

#include <Arduino.h>

#include "rf_frame.h"

enum class RfProtocol : uint8_t {
    kUnknown = 0,
    kEv1527  = 1,  // 24 bit fixed code (most 铁闸 / 卷帘门 remotes)
    kPt2262  = 2,  // 12 tri-state symbols
};

struct Decoded {
    RfProtocol proto  = RfProtocol::kUnknown;
    uint32_t code     = 0;   // EV1527: 24 bit word, PT2262: 12 symbols packed
    uint8_t bits      = 0;   // number of pulse pairs that were decoded
    uint8_t triStates = 0;   // floating symbols (PT2262 only)
    uint16_t baseUs   = 0;   // measured 1T pulse width
    bool valid        = false;
};

/// Decode a captured burst.  Returns a result with valid == false when the
/// waveform does not look like a known protocol.
Decoded decodeFrame(const RfFrame& frame);

/// Robust estimate of the 1T base time: the smallest pulse width that at least
/// four other pulses agree with.  A plain minimum is not usable - the receiver's
/// noise filter merges anything below RF_NOISE_US into its neighbour, so a single
/// surviving spike drags the minimum far below the real base time and every
/// 1T/3T bucket then misses.  Returns 0 when no such width exists (noise).
uint16_t estimateBaseUs(const uint16_t* pulses, int count);
uint16_t estimateBaseUs(const RfFrame& frame);

/// Shape score 0..100 for a stored pulse train, independent of whether the code
/// decoded.  60 % is the fraction of pulses that land on 1T or 3T and 40 % is how
/// close the longest low is to the 31T sync.  Levels are implied by index parity
/// (index 0 is a high), matching what CaptureStore keeps: it stores widths only.
/// A train whose sync pulses straddle both parities is capped into the "unusable"
/// band - its level labels were inverted mid-burst and it replays complemented.
/// Returns 0 when the arguments are unusable.
uint8_t scoreWaveform(const uint16_t* pulses, int count, uint16_t baseUs);

const char* protocolName(RfProtocol proto);

/// Human readable bit string ("1010..."), written into out (must hold bits+1).
void formatBits(const Decoded& decoded, char* out, size_t outSize);
