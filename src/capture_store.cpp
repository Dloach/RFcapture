#include "capture_store.h"

#include <Preferences.h>

namespace {

constexpr uint32_t kMagic = 0x4331u;

// A RAW capture never went through the decoder, so it has no measured base time;
// estimate one from the train itself.
uint8_t scoreOf(const Capture& c)
{
    uint16_t base = c.baseUs;
    if (base < 40) base = estimateBaseUs(c.pulses, c.count);
    return scoreWaveform(c.pulses, c.count, base);
}

}  // namespace

void CaptureStore::rescore()
{
    for (int i = 0; i < count_; ++i) quality_[i] = scoreOf(items_[i]);
}

void CaptureStore::begin()
{
    Preferences prefs;
    if (!prefs.begin("rfcap", true)) return;

    const uint32_t magic = prefs.getUInt("magic", 0);
    const int stored     = prefs.getInt("count", 0);
    const size_t bytes   = prefs.getBytesLength("items");

    // Recover from a `count` key that no longer matches the blob: as long as the
    // magic is right and the blob is a whole number of Capture records, the
    // records themselves are still good.  Without this a single bad write of
    // `count` hides an intact list until it is overwritten.
    if (magic == kMagic && bytes > 0 && (bytes % sizeof(Capture)) == 0) {
        int n = static_cast<int>(bytes / sizeof(Capture));
        if (n > rfcfg::maxCaptures) n = rfcfg::maxCaptures;
        if (stored > 0 && stored <= n) n = stored;  // a sane count wins
        if (n > 0) {
            prefs.getBytes("items", items_, sizeof(Capture) * size_t(n));
            count_ = n;
        }
    }
    prefs.end();

    rescore();

    Serial.printf("[store] %d capture(s) restored (magic=0x%04X count=%d bytes=%u)\n",
                  count_, static_cast<unsigned>(magic), stored, static_cast<unsigned>(bytes));
}

void CaptureStore::save()
{
    Preferences prefs;
    if (!prefs.begin("rfcap", false)) return;

    prefs.putUInt("magic", kMagic);
    if (count_ > 0) {
        prefs.putBytes("items", items_, sizeof(Capture) * size_t(count_));
    } else {
        // An empty list must remove the blob, not just zero the count:
        // begin()'s torn-write recovery rebuilds the list from a leftover
        // blob, which is why "clear all" used to come back after a reboot.
        prefs.remove("items");
    }
    // Write the count last: it acts as the commit marker, so a write that is cut
    // short leaves the previous count with the previous blob instead of a count
    // that disagrees with what is actually stored.
    prefs.putInt("count", count_);
    prefs.end();
    dirty_       = false;
    lastFlushMs_ = millis();
}

void CaptureStore::flush()
{
    if (!dirty_) return;
    if (millis() - lastFlushMs_ < rfcfg::storeFlushMs) return;
    save();
}

int CaptureStore::ingest(const RfFrame& frame, const Decoded& decoded, RfBand band, bool* isNew)
{
    if (isNew) *isNew = false;

    // Normalise: drop leading idle pulses so that pulses[0] is always a carrier
    // burst, then keep at most what fits into one capture.
    int start = 0;
    while (start < frame.count && frame.pulses[start].level == 0) start++;

    // Store exactly one whole frame: from the sync pulse up to (not including)
    // the next sync.  Replaying a truncated multi-frame train made some buttons
    // work and others not at all, depending on where the capture happened to
    // start.
    int end = frame.count;
    if (decoded.valid && decoded.baseUs >= 60) {
        const uint32_t t     = decoded.baseUs;
        const uint32_t limit = 6u * t;
        for (int i = start + 1; i < frame.count; ++i) {
            if (frame.pulses[i].level == 0 && frame.pulses[i].us >= limit) {
                start = i - 1;  // keep the high pulse that precedes the sync
                break;
            }
        }
        for (int i = start + 2; i < frame.count; ++i) {
            if (frame.pulses[i].level == 0 && frame.pulses[i].us >= limit) {
                end = i - 1;  // stop where the next frame begins
                break;
            }
        }
        // Store exactly the frame and nothing after it.  On a held remote the
        // frames are back to back: the next sync pulse follows the last data
        // bit with no gap at all (verified against the raw waveforms in
        // docs/DEBUG-2026-11-08.md section 7).  Replaying this train with
        // gapUs = 0 therefore reproduces the original burst exactly.  Trimming
        // to a bare frame only works because OokTransmitter::send() starts the
        // next repetition immediately - never re-introduce dead time there.
    }

    uint16_t train[rfcfg::storedPulses];
    int count = 0;
    for (int i = start; i < end && count < rfcfg::storedPulses; ++i) {
        train[count++] = frame.pulses[i].us;
    }
    if (count < 8) return -1;

    for (int i = 0; i < count_; ++i) {
        if (items_[i].band != band) continue;

        bool same = false;
        if (decoded.valid && items_[i].valid) {
            same = (items_[i].bits == decoded.bits && items_[i].code == decoded.code);
        } else if (items_[i].count == count) {
            same = similarTrain(items_[i].pulses, train, count);
        }
        if (same) {
            // Bumping the counter does not write to NVS: a held remote lands
            // here many times a second and each write rewrites the whole blob.
            // flush() batches it, and losing the last few counts is harmless.
            if (items_[i].hits < 0xFFFF) {
                items_[i].hits++;
                dirty_ = true;
            }
            return i;
        }
    }

    int slot = count_;
    if (count_ < rfcfg::maxCaptures) {
        count_++;
    } else {
        // Table is full: drop the oldest entry and append.  The save() at the
        // end of this function writes the result, so do not save twice.
        remove(0, false);
        slot = count_;
        count_++;
    }

    Capture& c = items_[slot];
    c.band     = band;
    c.code     = decoded.valid ? decoded.code : 0;
    c.baseUs   = decoded.baseUs;
    c.bits     = decoded.bits;
    c.proto    = static_cast<uint8_t>(decoded.proto);
    c.valid    = decoded.valid;
    c.hits     = 1;
    c.count    = static_cast<uint16_t>(count);
    for (int i = 0; i < count; ++i) c.pulses[i] = train[i];
    quality_[slot] = scoreOf(c);

    save();
    if (isNew) *isNew = true;
    return slot;
}

void CaptureStore::remove(int index)
{
    remove(index, true);
}

void CaptureStore::remove(int index, bool persist)
{
    if (index < 0 || index >= count_) return;
    for (int i = index; i < count_ - 1; ++i) {
        items_[i]   = items_[i + 1];
        quality_[i] = quality_[i + 1];
    }
    count_--;
    if (persist) save();
}

void CaptureStore::clear()
{
    count_ = 0;
    save();
}
