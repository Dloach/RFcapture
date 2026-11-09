#include "trace.h"

namespace trace {

// RTC_NOINIT_ATTR keeps the store in RTC slow memory *and* tells the bootloader
// not to re-initialise it, which is the whole point: a post-mortem dump has to
// show the frames from before the reset.  Plain RTC_DATA_ATTR would be copied
// back from flash on every boot and wipe them.  On a cold power-on the contents
// are random, so every reader validates magic first.
RTC_NOINIT_ATTR Store g_store;

namespace {
constexpr uint32_t kMagic = 0x52544632u;  // "RTF2"
bool g_dirty = false;
}

void reset()
{
    g_store.magic = kMagic;
    g_store.count = 0;
    g_store.next  = 0;
}

void record(const RfFrame& frame, uint8_t band, uint8_t bits, uint8_t proto, bool valid,
            uint16_t baseUs)
{
    if (g_store.magic != kMagic) reset();

    Entry& e   = g_store.entries[g_store.next];
    e.count    = frame.count < kPulses ? frame.count : kPulses;
    e.total    = frame.count;
    e.baseUs   = baseUs;
    e.band     = band;
    e.bits     = bits;
    e.proto    = proto;
    e.valid    = valid ? 1 : 0;
    for (int i = 0; i < e.count; ++i) {
        e.pulses[i] = frame.pulses[i].us;
    }

    g_store.next = (g_store.next + 1) % kSlots;
    if (g_store.count < kSlots) g_store.count++;
    g_dirty = true;
}

bool takeDirty()
{
    const bool d = g_dirty;
    g_dirty      = false;
    return d;
}

bool dumpAfterReset()
{
    if (g_store.magic != kMagic || g_store.count == 0 || g_store.count > kSlots) {
        reset();
        return false;
    }
    Serial.println("[trace] frames surviving the last reset:");
    dump();
    return true;
}

void dump()
{
    if (g_store.magic != kMagic || g_store.count == 0) {
        Serial.println("[trace] empty");
        return;
    }
    Serial.printf("[trace] %u frame(s)\n", static_cast<unsigned>(g_store.count));
    const int first = (g_store.next + kSlots - g_store.count) % kSlots;
    for (int i = 0; i < g_store.count; ++i) {
        const Entry& e = g_store.entries[(first + i) % kSlots];
        Serial.printf("[trace] #%d band=%u bits=%u proto=%u valid=%u T=%u n=%u:",
                      i, static_cast<unsigned>(e.band), static_cast<unsigned>(e.bits),
                      static_cast<unsigned>(e.proto), static_cast<unsigned>(e.valid),
                      static_cast<unsigned>(e.baseUs), static_cast<unsigned>(e.total));
        for (int k = 0; k < e.count; ++k) {
            Serial.printf(" %u", static_cast<unsigned>(e.pulses[k]));
        }
        Serial.println();
    }
}

}  // namespace trace
