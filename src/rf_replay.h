#pragma once

// Train-planning helpers shared by the RF app (src/app.cpp) and the launcher's
// one-key quick send.  Extracted verbatim from App's private methods so both
// transmit paths plan identical bursts.

#include <Arduino.h>

#include "rf_config.h"

namespace rfreplay {

// Levels are implied by parity: index 0 is the sync HIGH, so the long sync LOW
// sits at an odd index.
inline void wholeFrameRange(const uint16_t* pulses, int count, uint16_t baseUs, int* start,
                            int* end)
{
    *start = 0;
    *end   = count;
    if (pulses == nullptr || count < 8 || baseUs < 60) return;

    const uint32_t limit = 6u * baseUs;
    int last = -1;
    for (int i = 1; i < count; i += 2) {
        if (pulses[i] >= limit) last = i - 1;
    }
    // Need at least one frame left, otherwise keep everything.
    if (last > 8 && last < count) *end = last;
}

inline int planRepeats(const uint16_t* pulses, int count, uint32_t* durMs)
{
    uint32_t trainUs = 0;
    for (int i = 0; i < count; ++i) trainUs += pulses[i];
    if (trainUs == 0) trainUs = 1;

    const uint32_t targetUs = static_cast<uint32_t>(rfcfg::txTargetMs) * 1000u;
    int reps = static_cast<int>((targetUs + trainUs - 1) / trainUs);
    if (reps < rfcfg::txRepeats) reps = rfcfg::txRepeats;
    if (reps > rfcfg::txMaxRepeats) reps = rfcfg::txMaxRepeats;

    if (durMs != nullptr) {
        *durMs = static_cast<uint32_t>((static_cast<uint64_t>(trainUs) * reps) / 1000ull);
    }
    return reps;
}

}  // namespace rfreplay
