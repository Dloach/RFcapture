/*
 * IR transmitter: raw mark/space timings out of the on-board IR LED.
 *
 * Hardware (verified against Sch_M5CardputerAdv_v1.0): the LED is wired to
 * G44 through a 22 R resistor, with no carrier generator of any kind.  The
 * 38 kHz burst therefore has to come out of the peripheral: the ESP32-S3 RMT
 * block has a carrier modulator, so a mark becomes "carrier on for N us" and a
 * space becomes "carrier off for N us" with no CPU involvement.  A software
 * bit-bang loop would jitter far too much at 38 kHz inside a 8 ms frame.
 *
 * The legacy `driver/rmt.h` API is used on purpose: the firmware is pinned to
 * Arduino core 2.x / ESP-IDF 4.4 (see platformio.ini), where the new
 * `driver/rmt_tx.h` API does not exist.  src/ook_tx.cpp makes the same choice.
 *
 * Channel use: RMT channels 0..3 transmit, 4..7 receive on the S3, so TX takes
 * channel 0 and never collides with the receiver in ir_rx.cpp (channel 4).
 */

#pragma once

#include <stdint.h>

#include "ir_config.h"

namespace irtx {

/// G44 on the Cardputer and the Cardputer ADV (IR_TX_PIN in ir_config.h).
constexpr int kTxPin = IR_TX_PIN;

/// Configure the channel.  Returns false when RMT is unusable, in which case
/// every send() also returns false rather than pretending.
bool begin(int gpio = kTxPin);

/// Release the channel and park the pin low (LED off).
void end();

bool ready();

/// Transmit one frame `repeats` times, `gapUs` of silence between the copies.
/// `us` is alternating mark/space starting with a mark.  Blocks for the whole
/// transmission (a few hundred milliseconds at most).
bool send(const uint16_t* us, int count, uint32_t carrierHz, uint8_t dutyPercent, int repeats,
          uint32_t gapUs);

/// Continuous carrier for `ms` milliseconds.  Point a phone camera at the top
/// edge: a working LED shows as a purple dot.  Capped at 2000 ms, and the duty
/// cycle is kept low because the LED is driven straight from the GPIO.
bool carrierBurst(uint32_t ms, uint32_t carrierHz = 38000, uint8_t dutyPercent = 20);

}  // namespace irtx
