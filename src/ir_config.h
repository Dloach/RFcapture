/*
 * IR pin map and bring-up switches.
 *
 * Hardware evidence (Sch_M5CardputerAdv_v1.0, 2025-06-20):
 *   IR1  a two-pin infrared LED, anode through R14 (22 R 1 %) to G44
 *   no   infrared receiver anywhere on the board
 *
 * So G44 is transmit only, and learning needs a 38 kHz demodulating module on
 * the Grove port (see ir_rx.h for the wiring).
 */

#pragma once

#include <stdint.h>

/// On-board IR LED.  A plain GPIO output through 22 R -- all of the 38 kHz
/// carrier has to be generated inside the SoC (see ir_tx.h).
#define IR_TX_PIN 44

/// Is IR TOOLS part of this build?
///
/// 0 (shipping): the six src/ir_*.cpp files are excluded from the compile and
/// the launcher has no entry for them; the sources stay in the tree.
/// 1: `pio run -e cardputer-ir -t upload` compiles them back in and restores the
/// launcher entry.  Nothing else in the firmware references the IR code, so the
/// switch is this one define plus build_src_filter in platformio.ini.
#ifndef ENABLE_IR_TOOLS
#define ENABLE_IR_TOOLS 0
#endif

/// Grove yellow / white, the two free header pins.  The default is G1, which is
/// what the community Cardputer IR projects use.
#define IR_RX_PIN_DEFAULT 1

/// Bring-up build: every reset lands in IR TOOLS and the app fires a few
/// carrier bursts on start, so the LED can be checked with a phone camera over
/// a plain USB serial log (see docs/IR.md, "调试").  Off in the shipping build;
/// enable with the `ir-selftest` environment.
#ifndef IR_BOOT_SELFTEST
#define IR_BOOT_SELFTEST 0
#endif
