#pragma once

#include <Arduino.h>

namespace uiprefs {

void begin();
void applyBrightness();

uint8_t brightness();
void setBrightness(uint8_t value);

uint16_t timeoutSeconds();
void setTimeoutSeconds(uint16_t seconds);

/// Call once per main-loop iteration.  Any key press restores a blanked panel;
/// after the configured timeout the backlight is turned off.
void tick();

}  // namespace uiprefs
