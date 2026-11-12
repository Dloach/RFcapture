/*
 * Top-level launcher: one firmware, two features, chosen once per boot.
 *
 * The two features need mutually exclusive hardware configurations:
 *
 *   RF  - CC1101 in async OOK mode, GDO0 on an interrupt, RMT for transmit.
 *   NFC - ST25R3916, which must be told at Units.begin() time whether it is a
 *         reader or an emulated card.  There is no supported runtime switch.
 *
 * Rather than trying to hold both alive at once (they share the SPI bus and the
 * heap, and the NFC emulation is timing-critical), each choice is a reboot into
 * that feature.  A reboot costs about a second; a wrong configuration costs a
 * debugging session.
 *
 * The selection lives in NVS ("app" = 0 menu, 1 RF, 2 NFC).
 */

#pragma once

#include <Arduino.h>

#include "rf_config.h"

namespace launcher {

enum App : uint8_t {
    kAppMenu   = 0,
    kAppRf     = 1,
    kAppNfc    = 2,
    kAppBackup = 3,  // RF signals + NFC cards on the TF card
    kAppKbd    = 4,  // wired / Bluetooth keyboard
};

/// Which app this boot belongs to (from NVS).
App currentApp();

/// Store `app` and restart into it.
void rebootInto(App app);

/// Draw the chooser and wait for a key.  Never returns.
void menuSetup();
void menuLoop();

}  // namespace launcher
