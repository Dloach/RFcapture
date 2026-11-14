#pragma once

namespace settingsapp {

void setup();

/// Runs one iteration.  Returns true when the user asked to go back to the
/// launcher.  The caller must switch back without rebooting if it wants the
/// WiFi link to survive.
bool loop();

}  // namespace settingsapp
