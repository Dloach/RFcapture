/*
 * Replay of a captured OOK burst through the CC1101 in asynchronous TX mode.
 *
 * The baseband pulses are fed into GDO0 (G15).  Two backends exist:
 *
 *   RMT  - the pulse train is handed to the RMT transmitter, which reproduces
 *          it with the accuracy of the 80 MHz APB clock.  No CPU is spent while
 *          the frame goes out, so the watchdog and the UI stay alive.
 *   GPIO - the original software toggling with delayMicroseconds().  Kept as a
 *          fallback (and as an A/B reference) in case RMT is unavailable.
 *
 * Both backends honour the same contract: `pulses` alternate starting with a
 * HIGH level, and `gapUs` dead time is inserted between repetitions.
 */

#pragma once

#include <Arduino.h>

#include "rf_config.h"

class OokTransmitter {
public:
    /// Claim the output pin and, when RF_TX_USE_RMT is set, set up the RMT
    /// transmitter.  Safe to call once from App::begin().
    bool begin(int gpio);

    bool ready() const { return ready_; }
    /// True when the RMT backend was configured successfully.
    bool rmtReady() const { return rmtReady_; }

    /// `pulses` alternate high/low and must start with a high level.  `gapUs`
    /// is dead time inserted after each pass.  Keep it at 0 for captured trains:
    /// a held remote sends frames back to back, and the receivers that matter
    /// only accept several *consecutive* identical frames - a pause between
    /// repeats resets their frame counter.
    bool send(const uint16_t* pulses, int count, int repeats, uint32_t gapUs = 0);

private:
    bool configureRmt();
    bool sendRmt(const uint16_t* pulses, int count, int repeats, uint32_t gapUs);
    bool sendBitBang(const uint16_t* pulses, int count, int repeats, uint32_t gapUs);
    void releasePin();

    int pin_       = -1;
    bool ready_    = false;
    bool rmtReady_ = false;
};
