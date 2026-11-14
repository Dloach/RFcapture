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
#include "badusb_app.h"
#include "files_app.h"
#include "kbd_app.h"
#include "launcher.h"
#include "nfc_app.h"
#include "rf_config.h"
#include "settings_app.h"
#include "ssh_app.h"
#include "ui_prefs.h"
#include "usbdisk_app.h"
#include <USB.h>

namespace {

enum class Running : uint8_t { kMenu, kRf, kNfc, kBackup, kKbd, kSettings, kSsh, kFiles, kBadUsb, kUsbDisk };

Running g_running = Running::kMenu;
App* g_rf         = nullptr;

}  // namespace

void setup()
{
    // Deselect every device on the shared SPI bus (SCK 40 / MOSI 14 / MISO 39)
    // before anything touches it, and keep the module powered.
    //
    // After any reboot these pins come up as floating inputs, and a floating
    // NFC_CS reads as "selected" to the ST25R3916: it then answers the SD
    // card's init clocks and drives MISO, which latches the card until the
    // next power cycle.  That -- not anything the SD slot does -- is why the
    // TF card died after the NFC feature had been used, and why pulling CS12
    // high alone never helped.
    //
    // Do NOT pulse POWER_EN (G3) to reset the module: while it is unpowered,
    // the live SPI pins keep feeding its chips through their pin clamp diodes,
    // the rail never collapses, and the chips come back half-powered and deaf
    // for the rest of the power cycle.  The 60 ms pulse "fixed the SD card but
    // killed NFC" for exactly that reason.
    pinMode(RF_CC_CS, OUTPUT);   // CC1101
    digitalWrite(RF_CC_CS, HIGH);
    pinMode(RF_NFC_CS, OUTPUT);  // ST25R3916
    digitalWrite(RF_NFC_CS, HIGH);
    pinMode(12, OUTPUT);         // TF card
    digitalWrite(12, HIGH);
    pinMode(3, OUTPUT);          // POWER_EN: module always powered
    digitalWrite(3, HIGH);

    const launcher::App app = launcher::currentApp();

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

        case launcher::kAppKbd:
            g_running = Running::kKbd;
            kbdapp::setup();
            break;

        case launcher::kAppSettings:
            g_running = Running::kSettings;
            settingsapp::setup();
            break;

        case launcher::kAppSsh:
            g_running = Running::kSsh;
            sshapp::setup();
            break;

        case launcher::kAppFiles:
            g_running = Running::kFiles;
            filesapp::setup();
            break;

        case launcher::kAppBadUsb:
            g_running = Running::kBadUsb;
            badusbapp::setup();
            break;

        case launcher::kAppUsbDisk:
            g_running = Running::kUsbDisk;
            usbdiskapp::setup();
            break;

        default:
            g_running = Running::kMenu;
            launcher::menuSetup();
            break;
    }

    // CDC is no longer started by the core before setup(): USB DISK has to
    // register its MSC interface first so the descriptor includes it.
    USB.begin();
    uiprefs::begin();
    uiprefs::applyBrightness();
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
        case Running::kKbd:
            kbdapp::loop();
            break;
        case Running::kSettings:
            if (settingsapp::loop()) {
                g_running = Running::kMenu;
                launcher::menuSetup();
            }
            break;
        case Running::kSsh:
            sshapp::loop();
            break;
        case Running::kFiles:
            filesapp::loop();
            break;
        case Running::kBadUsb:
            badusbapp::loop();
            break;
        case Running::kUsbDisk:
            usbdiskapp::loop();
            break;
        default:
            launcher::menuLoop();
            break;
    }

    uiprefs::tick();
}
