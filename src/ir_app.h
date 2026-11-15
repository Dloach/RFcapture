#pragma once

// IR TOOLS: send, learn and manage infrared remotes.  One feature per boot, so
// this is entered through launcher::kAppIr and leaves by rebooting the menu.
namespace irapp {

void setup();
void loop();

}  // namespace irapp
