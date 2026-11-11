// Card identity to present.  Everything here was captured from the real card
// with `nfc-spike`; nothing is invented.
//
// The gate reader was verified to open for exactly this combination, so these
// values are the reference for everything in this project.
#pragma once

#include <cstdint>

namespace card {

// The card that opens the gate.  A 4-byte UID is what this reader expects, and a
// 4-byte UID is the only thing that goes into the anticollision answer.
constexpr uint8_t kUid[] = {0x3C, 0x02, 0xB6, 0x11};
constexpr uint8_t kUidLen = sizeof(kUid);

// Captured from the same card.  ATQA 0004 / SAK 08 is "MIFARE Classic 1K with a
// 4-byte UID".  Presenting the carrier's own values (ATQA 0044 / SAK 00, i.e. a
// 7-byte-UID Ultralight) makes the reader stop after selecting the UID and never
// act on it.
constexpr uint16_t kAtqa = 0x0004;
constexpr uint8_t kSak   = 0x08;

}  // namespace card
