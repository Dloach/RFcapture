/*
 * BACKUP feature: RF signals + NFC cards on the TF card, in one place.
 *
 * Entered from the launcher (launcher.h) and deliberately never initialises
 * either radio, so the SD card is the only device on the SPI bus.
 */

#pragma once

namespace backupapp {

void setup();
void loop();

}  // namespace backupapp
