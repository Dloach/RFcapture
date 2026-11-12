// NFC card store -- declarations.
//
// Split from an implementation-header because two translation units need it now:
// the NFC feature itself and the BACKUP feature (which writes the whole database
// to the TF card).  All definitions live in nfc_cards.cpp.
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>

namespace nfccards {

constexpr uint8_t kMaxCards = 16;
constexpr uint8_t kUidMax   = 10;
constexpr size_t kNameLen   = 17;
constexpr size_t kTypeLen   = 24;

struct Card {
    uint8_t uid[kUidMax];
    uint8_t len;
    uint16_t atqa;
    uint8_t sak;
    uint16_t hits;
    char type[kTypeLen];
    char name[kNameLen];
};

extern Card cards[kMaxCards];
extern uint8_t count;
extern const char* lastError;

int8_t find(const uint8_t len, const uint8_t* uid);
bool save();
/// Persist hit-counter changes at most once per 30 s, or immediately when
/// `force` is set.  New cards / deletes / renames still save immediately.
void flush(bool force = false);
int8_t add(const Card& c, bool* isNew = nullptr);
void remove(const int8_t i);
void rename(const int8_t i, const char* name);
bool replayable(const Card& c);
void uidToString(const Card& c, char* out, size_t n);

/// Load the collection from SPIFFS (importing the main firmware's file once if
/// this project has none yet).
int8_t load();

// SD backup / restore (used by the BACKUP feature, not by the NFC feature).
bool sdReady();
bool backupToSd();
bool restoreFromSd();

}  // namespace nfccards
