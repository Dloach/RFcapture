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

class SavedStore {
public:
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

    /// Copy a capture into the permanent list.  Returns false when the list is
    /// full or the same code is already there.
    bool add(const Capture& capture, int* indexOut = nullptr);

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
