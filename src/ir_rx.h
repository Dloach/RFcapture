/*
 * IR receiver: capture mark/space timings from an external demodulating
 * receiver.
 *
 * The Cardputer ADV has **no** IR receiver on board: the schematic shows a
 * single 2-pin IR LED (IR1, G44, 22 R) and nothing else in the infrared
 * section.  Learning therefore needs a 3-pin 38 kHz module (VS1838B, HX1838,
 * TSOP38238, or the M5Stack Unit-IR U002) in the Grove port:
 *
 *   module VCC  -> Grove 5V (red)      module OUT -> Grove G1 (yellow)
 *   module GND  -> Grove GND (black)
 *
 * G1 is the default; G2 (white) is selectable in the IR app's setup page for
 * modules whose wires are swapped.  Both are otherwise unused by this
 * firmware - the Grove port carries no radio.
 *
 * Capture uses RMT channel 4 at 1 us per tick, so the RMT interrupt records
 * edge timings in hardware while the UI keeps drawing.  The module's output is
 * active low (low while it sees the carrier), so a low run is a mark.
 */

#pragma once

#include <stdint.h>

#include "ir_config.h"

namespace irrx {

/// Grove yellow / white, the only two free header pins on this board.
constexpr int kPinG1 = 1;
constexpr int kPinG2 = 2;

/// Quiet time that ends a frame.  A NEC repeat pause is ~96 ms and a Sony one
/// ~45 ms, but the shortest gap between two frames of one press is RC5's
/// 2.7 ms, so this is a compromise: long enough to keep one frame whole
/// (longest intra-frame space is NEC's 1.69 ms leader gap of 4.5 ms).
constexpr uint32_t kFrameGapUs = 12000;

bool begin(int pin, int idleMs = 20);
void end();
bool ready();
int pin();

/// Non-blocking.  Returns true once, when a complete frame has been captured.
/// `out` receives alternating mark/space durations starting with a mark.
bool poll(uint16_t* out, int maxCount, int* outCount);

/// millis() of the last edge seen, for a "waiting / receiving" indicator.
uint32_t lastEdgeMs();

/// Discard anything buffered (call when (re)starting a learn session).
void flush();

}  // namespace irrx
