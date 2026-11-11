/*
 * M5Stack Cardputer ADV - multi-feature firmware.
 *
 * Boot lands on a chooser (src/launcher.cpp):
 *
 *   RF TOOLS    315/433/868 MHz OOK remote capture, storage and replay
 *               (src/app.cpp, CC1101 + RMT)
 *   NFC TOOLS   read NFC-A cards, keep them, emulate one at a door
 *               (src/nfc_app.cpp, ST25R3916)
 *   BACKUP      the TF card copy of both databases (src/backup_app.cpp)
 *
 * Each choice is a reboot into that feature rather than a runtime mode switch:
 * the three need mutually exclusive SPI configurations (CC1101 mode 0, the
 * ST25R3916 mode 1, the SD card mode 0), and mixing them on one bus latched the
 * card hard enough to need a power cycle.  See launcher.h and backup_app.cpp.
 */

#include "app.h"
#include "backup_app.h"
#include "launcher.h"
#include "nfc_app.h"

namespace {

enum class Running : uint8_t { kMenu, kRf, kNfc, kBackup };

Running g_running = Running::kMenu;
App* g_rf         = nullptr;

}  // namespace

void setup()
{
    const launcher::App app = launcher::currentApp();

    // Reset the radio module, but ONLY for the BACKUP feature.
    //
    // CC1101 (CS 5), the ST25R3916 (CS 6) and the SD card (CS 12) share the SPI
    // bus, and after the NFC feature has driven the chip it keeps driving MISO, so
    // the card cannot answer until the module is power-cycled.  A short power
    // pulse here clears that -- but the pulse also upsets the ST25R3916's own
    // bring-up, so it must not run on boots that are going to use the NFC chip.
    // BACKUP uses no radio at all, which makes it the one place the pulse is free.
    if (app == launcher::kAppBackup) {
        pinMode(3, OUTPUT);
        digitalWrite(3, LOW);
        delay(60);
        digitalWrite(3, HIGH);
        delay(300);
    }

    switch (app) {
        case launcher::kAppRf:
            // Created here rather than as a global so M5Cardputer (which has
            // dynamic initialisation) already exists when App takes its display
            // reference.
            g_running = Running::kRf;
            g_rf      = new App();
            g_rf->begin();
            break;

        case launcher::kAppNfc:
            g_running = Running::kNfc;
            nfcapp::setup();
            break;

        case launcher::kAppBackup:
            g_running = Running::kBackup;
            backupapp::setup();
            break;

        default:
            g_running = Running::kMenu;
            launcher::menuSetup();
            break;
    }
}

void loop()
{
    switch (g_running) {
        case Running::kRf:
            if (g_rf) g_rf->update();
            break;
        case Running::kNfc:
            nfcapp::loop();
            break;
        case Running::kBackup:
            backupapp::loop();
            break;
        default:
            launcher::menuLoop();
            break;
    }
}
