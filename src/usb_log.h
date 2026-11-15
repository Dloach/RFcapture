/*
 * One-line diagnostics over the USB CDC port (the COM port the Cardputer
 * exposes).  `Serial` in this firmware is UART0, which is not routed to USB, so
 * app-level logs have to go through the shared USBCDC instance in main.cpp.
 *
 * The port only exists after main() calls USB.begin(), so log from loop(), not
 * from setup().  Connect with:  pio device monitor -p COM13 -b 115200
 */
#pragma once

/// printf-style log, truncated to one 160-byte line.
void usbLogf(const char* format, ...);
