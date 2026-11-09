/*
 * Plain data types describing one demodulated OOK burst.  Kept free of any
 * driver dependency so the decoder can also be built and tested on a PC.
 */

#pragma once

#include <Arduino.h>

#include "rf_config.h"

struct RfPulse {
    uint16_t us;
    uint8_t level;  // 1 = carrier on (high), 0 = idle (low)
};

struct RfFrame {
    RfPulse pulses[rfcfg::maxFramePulses];
    uint16_t count = 0;
    bool truncated  = false;
    uint32_t atMs   = 0;
    uint32_t seq    = 0;
};
