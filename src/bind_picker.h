#pragma once

#include <Arduino.h>

namespace bindpick {

// Modal bind editor, shared by the RF SEND page and the NFC card list: pick a
// grid key, then an icon, then a color; ENTER saves and returns true, esc
// cancels and returns false.  D on the slot step clears that slot's binding.
// `key` is the stable identity of the bound record (quickbind::keyForRf /
// keyForNfc); index is only the fast path / legacy fallback.
// Takes over the screen and keyboard until done; the caller redraws after.
bool run(uint8_t kind, uint8_t index, const char* what, uint32_t key = 0);

}  // namespace bindpick
