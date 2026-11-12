#include "saved_store.h"

#include <Preferences.h>

namespace {

constexpr uint32_t kMagic = 0x5332u;

uint8_t scoreOf(const SavedSignal& s)
{
    uint16_t base = s.baseUs;
    int n         = s.count;
    if (n > rfcfg::storedPulses) n = rfcfg::storedPulses;
    if (base < 40) base = estimateBaseUs(s.pulses, n);
    return scoreWaveform(s.pulses, n, base);
}

uint8_t scoreCapture(const Capture& c)
{
    uint16_t base = c.baseUs;
    int n         = c.count;
    if (n > rfcfg::storedPulses) n = rfcfg::storedPulses;
    if (base < 40) base = estimateBaseUs(c.pulses, n);
    return scoreWaveform(c.pulses, n, base);
}

}  // namespace

void SavedStore::rescore()
{
    for (int i = 0; i < count_; ++i) quality_[i] = scoreOf(items_[i]);
}

void SavedStore::begin()
{
    count_ = 0;

    Preferences prefs;
    if (!prefs.begin("rfsave", true)) return;

    const uint32_t magic = prefs.getUInt("magic", 0);
    const int stored     = prefs.getInt("count", 0);
    const size_t bytes   = prefs.getBytesLength("items");

    // count is the commit marker: save() writes the blob first and count last.
    // If power was lost between them, the blob is a whole number of records and
    // the old count still points at a valid prefix.  A stored count that is too
    // large (the shrink case) is clamped to what the blob actually holds, so a
    // torn write can no longer hide or wipe the whole list.
    if (magic == kMagic && stored > 0) {
        const int n = recoverSavedCount(bytes, stored, sizeof(SavedSignal), rfcfg::maxSaved);
        if (n > 0) {
            prefs.getBytes("items", items_, sizeof(SavedSignal) * size_t(n));
            count_ = n;
            for (int i = 0; i < count_; ++i) {
                if (items_[i].count > rfcfg::storedPulses) items_[i].count = rfcfg::storedPulses;
            }
        }
    }
    prefs.end();

    rescore();

    Serial.printf("[saved] %d signal(s) restored (magic=0x%04X count=%d bytes=%u)\n", count_,
                  static_cast<unsigned>(magic), stored, static_cast<unsigned>(bytes));
}

void SavedStore::save()
{
    Preferences prefs;
    if (!prefs.begin("rfsave", false)) return;

    prefs.putUInt("magic", kMagic);
    if (count_ > 0) {
        const size_t want = sizeof(SavedSignal) * size_t(count_);
        if (prefs.putBytes("items", items_, want) != want) {
            // Do not advance the commit marker over a blob that was not fully
            // written: leaving the old count in place lets begin() fall back to
            // the previous list instead of claiming records that are not there.
            Serial.println("[saved] blob write failed; count not advanced");
            prefs.end();
            return;
        }
    } else {
        // An empty list must remove the blob, not just zero the count: begin()
        // would otherwise see a stale whole-record blob behind a zero count and
        // resurrect signals the operator deliberately deleted.
        prefs.remove("items");
    }
    // Write the count last: it is the commit marker.  A write cut short leaves
    // the previous count with the previous blob instead of a count that claims
    // records which were never written.
    prefs.putInt("count", count_);
    prefs.end();
}

SavedStore::AddResult SavedStore::add(const Capture& capture, int* indexOut)
{
    for (int i = 0; i < count_; ++i) {
        if (items_[i].band != capture.band) continue;

        bool same = false;
        const bool itemDecoded = items_[i].proto != 0;
        if (capture.valid && itemDecoded) {
            same = (items_[i].bits == capture.bits && items_[i].code == capture.code);
        } else if (!itemDecoded && items_[i].count == capture.count) {
            // A RAW entry has no code, so two of them were treated as
            // duplicates on pulse count alone - one save of a 160 pulse train
            // made every later 160 pulse RAW capture silently unsaveable.
            // A decoded capture may upgrade a RAW entry; a RAW capture must
            // never overwrite a decoded one (that would throw the code away).
            same = similarTrain(items_[i].pulses, capture.pulses, capture.count);
        }
        if (!same) continue;

        if (indexOut) *indexOut = i;
        const uint8_t newQ = scoreCapture(capture);
        if (!shouldReplaceWaveform(quality_[i], newQ)) {
            return AddResult::kDuplicate;
        }

        // Same signal, better waveform: keep the user's name and the slot, but
        // replace the pulses.  Without this, pressing ENTER again after a clean
        // re-capture just said "ALREADY SAVED" and left the bad waveform in the
        // permanent list, which is the copy that matters at the gate.
        SavedSignal& s = items_[i];
        int n          = capture.count;
        if (n > rfcfg::storedPulses) n = rfcfg::storedPulses;
        s.code   = capture.valid ? capture.code : 0;
        s.baseUs = capture.baseUs;
        s.bits   = capture.bits;
        s.proto  = static_cast<uint8_t>(capture.proto);
        s.count  = static_cast<uint16_t>(n);
        for (int k = 0; k < n; ++k) s.pulses[k] = capture.pulses[k];
        quality_[i] = newQ;
        save();
        return AddResult::kUpdated;
    }
    if (count_ >= rfcfg::maxSaved) return AddResult::kFull;

    SavedSignal& s = items_[count_];
    s.band         = capture.band;
    s.code         = capture.code;
    s.baseUs       = capture.baseUs;
    s.bits         = capture.bits;
    s.proto        = capture.proto;
    int n          = capture.count;
    if (n > rfcfg::storedPulses) n = rfcfg::storedPulses;
    s.count = static_cast<uint16_t>(n);
    for (int i = 0; i < n; ++i) {
        s.pulses[i] = capture.pulses[i];
    }

    if (capture.valid) {
        snprintf(s.name, sizeof(s.name), "%06lX",
                 static_cast<unsigned long>(capture.code & 0xFFFFFFu));
    } else {
        snprintf(s.name, sizeof(s.name), "RAW %u", static_cast<unsigned>(n));
    }

    if (indexOut) *indexOut = count_;
    quality_[count_] = scoreOf(s);
    count_++;
    save();
    return AddResult::kAdded;
}

void SavedStore::remove(int index)
{
    if (index < 0 || index >= count_) return;
    for (int i = index; i < count_ - 1; ++i) {
        items_[i]   = items_[i + 1];
        quality_[i] = quality_[i + 1];
    }
    count_--;
    save();
}

void SavedStore::rename(int index, const char* name)
{
    if (index < 0 || index >= count_ || name == nullptr || name[0] == 0) return;
    snprintf(items_[index].name, sizeof(items_[index].name), "%s", name);
    save();
}

int SavedStore::replaceAll(const SavedSignal* items, int count)
{
    if (count < 0) count = 0;
    if (count > rfcfg::maxSaved) count = rfcfg::maxSaved;

    for (int i = 0; i < count; ++i) {
        items_[i]                              = items[i];
        items_[i].name[sizeof(items_[i].name) - 1] = 0;
        if (items_[i].count > rfcfg::storedPulses) items_[i].count = rfcfg::storedPulses;
    }
    count_ = count;
    rescore();
    save();
    return count_;
}
