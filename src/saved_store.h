/*
 * Permanently saved signals ("永久保存"), separate from the automatic capture
 * list.  Stored in NVS with a user editable name, so the Send page only shows
 * what the user explicitly kept.
 */

#pragma once

#include <Arduino.h>

#include "capture_store.h"
#include "rf_config.h"

struct SavedSignal {
    char name[16] = {0};
    RfBand band    = RfBand::k315;
    uint32_t code  = 0;
    uint16_t baseUs = 0;
    uint16_t count  = 0;
    uint8_t bits    = 0;
    uint8_t proto   = 0;
    uint16_t pulses[rfcfg::storedPulses];
};

/// Number of records a persisted blob can yield after a tear.  The blob length
/// is the physical truth; `stored` is the commit marker written last.  A marker
/// that is sane and no larger than the blob wins (ignoring an uncommitted tail);
/// a marker that is too large is clamped to what the blob really holds.  Zero
/// means "nothing recoverable" (including the deliberately empty list).
inline int recoverSavedCount(size_t bytes, int stored, size_t recordSize, int maxRecords)
{
    if (recordSize == 0 || stored <= 0 || bytes < recordSize) return 0;

    int n = static_cast<int>(bytes / recordSize);
    if (n > maxRecords) n = maxRecords;

    // At least `stored` whole records are physically present: the commit marker
    // is trustworthy even if the write continued into a partial next record.
    if (bytes >= recordSize * size_t(stored)) return stored > maxRecords ? maxRecords : stored;

    // The blob is shorter than the marker claims (a shrink was interrupted):
    // recover the whole-record prefix it actually contains.
    return stored <= n ? stored : n;
}

class SavedStore {
public:
    enum class AddResult : uint8_t {
        kAdded,     // new entry appended
        kDuplicate, // same signal, no better waveform
        kUpdated,   // same signal, waveform replaced by a better capture
        kFull,      // no room
    };

    void begin();

    int size() const { return count_; }
    bool empty() const { return count_ == 0; }
    SavedSignal& at(int index) { return items_[index]; }
    const SavedSignal& at(int index) const { return items_[index]; }

    /// Shape score 0..100, RAM only - see CaptureStore::quality().
    uint8_t quality(int index) const
    {
        return index >= 0 && index < count_ ? quality_[index] : 0;
    }

    /// Copy a capture into the permanent list.  A duplicate is updated in place
    /// when the new capture's waveform scores significantly better; the name is
    /// preserved.  `indexOut` receives the entry index for every result except
    /// kFull.
    AddResult add(const Capture& capture, int* indexOut = nullptr);

    void remove(int index);
    void rename(int index, const char* name);

    /// Replace the whole list (used by a TF card restore).  Returns the number
    /// of records actually stored.
    int replaceAll(const SavedSignal* items, int count);

    void save();

private:
    void rescore();

    SavedSignal items_[rfcfg::maxSaved];
    uint8_t quality_[rfcfg::maxSaved] = {0};
    int count_ = 0;
};
