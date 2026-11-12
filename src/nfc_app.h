/*
 * NFC feature module: read cards, keep them, put one back on the air.
 *
 * Entered from the launcher (launcher.h).  It has two sub-modes of its own,
 * because the ST25R3916 has to be told at Units.begin() time whether it is a
 * reader or an emulated card and cannot switch at runtime:
 *
 *   menu/reader : list, scan, pick a card to emulate
 *   emulator    : chosen by an NVS flag + reboot, arms the chosen card and does
 *                 nothing else (the emulation is timing-critical)
 *
 * Returning to the launcher is another reboot.
 */

#pragma once

#include <stdint.h>

namespace nfcapp {

void setup();
void loop();

// Queue a boot straight into emulation of card `idx` (writes the nfcretry boot
// flag).  `key` is the stable quickbind key; zero is a legacy index-only
// binding.  The caller then reboots into kAppNfc -- launcher.h's rebootInto().
// Used by the home grid's one-key emulation.
void queueEmulation(uint8_t idx, uint32_t key = 0);

}  // namespace nfcapp
