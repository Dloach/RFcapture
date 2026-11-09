/*
 * M5Stack Cardputer ADV - 315/433/868 MHz OOK remote capture and replay.
 *
 * Press a remote in front of the receiver: the burst is captured by a GPIO
 * interrupt time-stamped with micros(), decoded (EV1527 / PT2262) and listed on
 * screen.  Signals live in NVS and can be backed up to / restored from the TF
 * card.  See docs/README.md for wiring, key bindings and the backup layout.
 */

#include <M5Cardputer.h>

#include "app.h"

namespace {

// Created from setup() so that M5Cardputer (a global with dynamic
// initialisation) is already constructed when the display reference is taken.
App* g_app = nullptr;

}  // namespace

void setup()
{
    g_app = new App();
    g_app->begin();
}

void loop()
{
    if (g_app) {
        g_app->update();
    }
}
