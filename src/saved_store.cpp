#include "saved_store.h"

#include <Preferences.h>

namespace {

constexpr uint32_t kMagic = 0x5332u;

uint8_t scoreOf(const SavedSignal& s)
{
    uint16_t base = s.baseUs;
    if (base < 40) base = estimateBaseUs(s.pulses, s.count);
    return scoreWaveform(s.pulses, s.count, base);
}

}  // namespace

void SavedStore::rescore()
{
    for (int i = 0; i < count_; ++i) quality_[i] = scoreOf(items_[i]);
}

void SavedStore::begin()
{
    Preferences prefs;
    if (!prefs.begin("rfsave", true)) return;

    const uint32_t magic = prefs.getUInt("magic", 0);
    const int stored     = prefs.getInt("count", 0);
    const size_t bytes   = prefs.getBytesLength("items");

    if (magic == kMagic && stored > 0 && stored <= rfcfg::maxSaved &&
        bytes == sizeof(SavedSignal) * size_t(stored)) {
        prefs.getBytes("items", items_, bytes);
        count_ = stored;
    }
    prefs.end();

    rescore();

    Serial.printf("[saved] %d permanent signal(s) restored\n", count_);
}

void SavedStore::save()
{
    Preferences prefs;
    if (!prefs.begin("rfsave", false)) return;

    prefs.putUInt("magic", kMagic);
    prefs.putInt("count", count_);
    if (count_ > 0) {
        prefs.putBytes("items", items_, sizeof(SavedSignal) * size_t(count_));
    }
    prefs.end();
}

bool SavedStore::add(const Capture& capture, int* indexOut)
{
    for (int i = 0; i < count_; ++i) {
        if (items_[i].band != capture.band) continue;
        if (capture.valid && items_[i].bits == capture.bits &&
            items_[i].code == capture.code) {
            if (indexOut) *indexOut = i;
            return false;  // already saved
        }
        // A RAW entry has no code to compare, so two of them were treated as
        // duplicates on pulse count alone - one save of a 160 pulse train made
        // every later 160 pulse RAW capture silently unsaveable.
        if (!capture.valid && items_[i].count == capture.count &&
            similarTrain(items_[i].pulses, capture.pulses, capture.count)) {
            if (indexOut) *indexOut = i;
            return false;
        }
    }
    if (count_ >= rfcfg::maxSaved) return false;

    SavedSignal& s = items_[count_];
    s.band         = capture.band;
    s.code         = capture.code;
    s.baseUs       = capture.baseUs;
    s.bits         = capture.bits;
    s.proto        = capture.proto;
    s.count        = capture.count;
    for (int i = 0; i < capture.count && i < rfcfg::storedPulses; ++i) {
        s.pulses[i] = capture.pulses[i];
    }

    if (capture.valid) {
        snprintf(s.name, sizeof(s.name), "%06lX",
                 static_cast<unsigned long>(capture.code & 0xFFFFFFu));
    } else {
        snprintf(s.name, sizeof(s.name), "RAW %u", static_cast<unsigned>(capture.count));
    }

    if (indexOut) *indexOut = count_;
    quality_[count_] = scoreOf(s);
    count_++;
    save();
    return true;
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
