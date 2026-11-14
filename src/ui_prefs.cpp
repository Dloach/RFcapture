#include "ui_prefs.h"

#include <M5Cardputer.h>
#include <Preferences.h>

namespace uiprefs {
namespace {

constexpr char kNs[] = "ui";
uint8_t gBrightness = 90;
uint16_t gTimeout   = 0;
bool loaded         = false;
bool blanked        = false;
uint32_t lastInput  = 0;

void load()
{
    if (loaded) return;
    loaded = true;
    Preferences p;
    if (!p.begin(kNs, true)) return;
    gBrightness = p.getUChar("bright", 90);
    gTimeout    = p.getUShort("timeout", 0);
    p.end();
    if (gBrightness < 10) gBrightness = 10;
    if (gBrightness > 100) gBrightness = 100;
}

void saveBrightness()
{
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putUChar("bright", gBrightness);
    p.end();
}

void saveTimeout()
{
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putUShort("timeout", gTimeout);
    p.end();
}

}  // namespace

void begin()
{
    load();
    lastInput = millis();
}

void applyBrightness()
{
    M5Cardputer.Display.setBrightness(gBrightness);
    blanked = false;
}

uint8_t brightness()
{
    load();
    return gBrightness;
}

void setBrightness(uint8_t value)
{
    load();
    if (value < 10) value = 10;
    if (value > 100) value = 100;
    gBrightness = value;
    saveBrightness();
    M5Cardputer.Display.setBrightness(gBrightness);
    blanked = false;
}

uint16_t timeoutSeconds()
{
    load();
    return gTimeout;
}

void setTimeoutSeconds(uint16_t seconds)
{
    load();
    gTimeout = seconds;
    saveTimeout();
    lastInput = millis();
}

void tick()
{
    load();
    const bool active = M5Cardputer.Keyboard.isPressed() > 0;
    if (active) {
        lastInput = millis();
        if (blanked) {
            M5Cardputer.Display.setBrightness(gBrightness);
            blanked = false;
        }
        return;
    }
    if (gTimeout == 0 || blanked) return;
    if (millis() - lastInput >= static_cast<uint32_t>(gTimeout) * 1000) {
        M5Cardputer.Display.setBrightness(0);
        blanked = true;
    }
}

}  // namespace uiprefs
