#pragma once

#include <USBCDC.h>

namespace flashmode {

/// Install the USB flashing hook on the firmware's CDC port.  Call from
/// main.cpp after USB.begin() (the port must exist, so the subscription
/// starts before any host can send a line coding).
///
/// Without this hook the core restarts into the ROM bootloader silently the
/// moment a host sets the CDC line coding to 1200 bps (USBCDC.cpp
/// _onLineCoding -> usb_persist_restart): no user code runs, the keyboard and
/// every app keep "working" for the microseconds left, and the LCD freezes
/// on whatever happened to be on it -- a half-written device then sits in the
/// bootloader still showing the old screen.  With the hook, the 1200 bps
/// request arrives as an event instead: the loop task is suspended, the
/// dedicated flashing page is painted (it survives the reset: the LCD keeps
/// its framebuffer across an MCU reset), and only then does the chip enter
/// the ROM bootloader.  While the host uploads, the firmware is not running
/// -- the painted page, progress bar included, is the whole UI there is.
void begin(USBCDC& cdc);

}  // namespace flashmode
