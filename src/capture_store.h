/*
 * Table of captured signals, persisted in NVS so that a captured gate remote
 * survives a power cycle.
 *
 * A capture stores the raw pulse train, not just the decoded code: replaying
 * the exact waveform works for every OOK remote, including the ones whose
 * protocol the decoder does not recognise.
 */

#pragma once

#include <Arduino.h>

#include "ook_decoder.h"
#include "rf_config.h"

struct Capture {
    RfBand band     = RfBand::k315;
    uint32_t code   = 0;
    uint16_t baseUs = 0;
    uint16_t count  = 0;   // number of stored pulses
    uint16_t hits   = 0;
    uint8_t bits    = 0;
    uint8_t proto   = 0;   // RfProtocol
    bool valid      = false;
    uint16_t pulses[rfcfg::storedPulses];
};

/// Two pulse trains belong to the same remote when every pulse matches within
/// 25 %, which absorbs the jitter of a cheap superheterodyne receiver.  Shared
/// by both stores: SavedStore has to dedup its RAW entries the same way, since
/// for those there is no decoded code to compare.
inline bool similarTrain(const uint16_t* a, const uint16_t* b, int count)
{
    for (int i = 0; i < count; ++i) {
        const uint16_t hi = a[i] > b[i] ? a[i] : b[i];
        const uint16_t lo = a[i] > b[i] ? b[i] : a[i];
        if (uint32_t(hi - lo) * 4u > uint32_t(hi)) return false;
    }
    return true;
}

class CaptureStore {
public:
    void begin();  // load from NVS

    int size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const Capture& at(int index) const { return items_[index]; }

    /// Shape score 0..100 of the stored pulse train (see scoreWaveform()).
    /// Cached in RAM only and recomputed on load: putting it in Capture would
    /// change the NVS blob layout, and the stored signals are not reproducible.
    uint8_t quality(int index) const
    {
        return index >= 0 && index < count_ ? quality_[index] : 0;
    }

    /// Store a burst, or bump the hit counter of an identical one.
    /// Returns the index, or -1 when the burst is unusable.
    int ingest(const RfFrame& frame, const Decoded& decoded, RfBand band, bool* isNew);

    void remove(int index);
    void clear();
    void save();

    /// Write a pending hit-counter change to NVS, at most once every
    /// RF_STORE_FLUSH_MS.  A held remote bumps `hits` many times per second and
    /// each save rewrites the whole 5 kB blob, so it is batched.  Call from the
    /// main loop.
    void flush();

private:
    void rescore();
    void remove(int index, bool persist);

    Capture items_[rfcfg::maxCaptures];
    uint8_t quality_[rfcfg::maxCaptures] = {0};
    int count_ = 0;
    bool dirty_ = false;
    uint32_t lastFlushMs_ = 0;
};
