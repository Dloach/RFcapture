/*
 * Diagnostic trace kept in RTC slow memory (RTC_NOINIT_ATTR).
 *
 * Serial output is only captured while a host is listening, which made remote
 * button tests unreliable: by the time the log was collected the interesting
 * frame was long gone.  The last few frames (decoded or not) are therefore kept
 * in RTC RAM, which survives a software reset or a watchdog panic, and are
 * dumped once at the next boot by dumpAfterReset().
 */

#pragma once

#include <Arduino.h>

#include "rf_frame.h"

namespace trace {

constexpr int kSlots  = 8;
constexpr int kPulses = 48;

struct Entry {
    uint16_t count;  // pulses stored (capped)
    uint16_t total;  // pulses in the original frame
    uint16_t baseUs;
    uint8_t band;
    uint8_t bits;
    uint8_t proto;
    uint8_t valid;
    uint16_t pulses[kPulses];
};

struct Store {
    uint32_t magic;
    uint8_t count;
    uint8_t next;
    Entry entries[kSlots];
};

extern Store g_store;

void reset();
bool takeDirty();

/// Dump whatever survived the last reset, once, from setup().  Returns false
/// when RTC memory holds nothing valid (cold power-on) and resets the store.
bool dumpAfterReset();

void record(const RfFrame& frame, uint8_t band, uint8_t bits, uint8_t proto, bool valid,
            uint16_t baseUs);
void dump();

}  // namespace trace
