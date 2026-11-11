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

namespace nfcapp {

void setup();
void loop();

}  // namespace nfcapp
