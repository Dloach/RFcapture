/*
 * Backup / restore of the permanently saved signals on the microSD card.
 *
 * The card slot shares SCK/MOSI/MISO (G40/G14/G39) with the CAP.CC1101, so the
 * card is mounted on the same SPI bus the CC1101 already started, used, and
 * unmounted again.  The file is plain text so it can be inspected or edited on
 * a PC: /cardputer/signals.txt
 */

#pragma once

#include <Arduino.h>

#include "saved_store.h"

enum class TfResult : uint8_t {
    kOk,
    kPartial,  // restored, but malformed records were skipped
    kNoCard,
    kNoFile,
    kEmpty,
    kBadFile,
    kIoError,
};

namespace tfbackup {

/// Write every saved signal to the card.  Returns kOk only when the file was
/// written and renamed into place.
TfResult saveSignals(const SavedStore& store, int* count);

/// Replace the saved list with the content of the card file.
TfResult loadSignals(SavedStore& store, int* count);

}  // namespace tfbackup
