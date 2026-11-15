/*
 * Flipper Zero `.ir` remote files: list, load and serialise.
 *
 * Pure C++ with no Arduino.h, no allocation and no mutable globals, so the
 * host tests in tests/host/test_ir_file.cpp can compile ir_file.cpp straight
 * into a native binary together with their own small ir:: stubs.
 *
 * A `.ir` file is a sequence of "button" records separated by '#' comment
 * lines:
 *
 *     Filetype: IR signals file
 *     Version: 1
 *     #
 *     name: Button_1
 *     type: parsed
 *     protocol: NECext
 *     address: EE 87 00 00
 *     command: 5D A0 00 00
 *     #
 *     name: Button_2
 *     type: raw
 *     frequency: 38000
 *     duty_cycle: 0.330000
 *     data: 504 3432 502 483 ...
 *     #
 *
 * `Header` is the cheap row the button list screen needs; `Entry` is the full
 * record (with the raw timing list) and is only built for the selected button.
 *
 * Byte order: `address`/`command` are written as four 2-digit hex bytes with
 * the least significant byte first, so "EE 87 00 00" is 0x000087EE and
 * formatFile() writes it back in the same order.
 */

#pragma once

#include <stdint.h>

#include "ir_protocol.h"

namespace irfile {

/// One row of the button list, cheap to copy.
struct Header {
    char name[24];
    bool raw;
};

/// A complete button record.  ~1.1 KB, so allocate it once, not per row.
struct Entry {
    char name[24];
    bool raw = false;
    ir::Signal signal;          // meaningful when raw == false
    uint32_t frequency = 38000; // meaningful when raw == true
    uint8_t dutyPercent = 33;
    uint16_t timings[ir::kMaxTimings];
    int count = 0;              // timings used when raw == true
};

/// Parse the list of buttons in `text` (not NUL terminated; `len` bytes).
/// Returns the number of headers written (capped at maxOut), or -1 when the
/// buffer is not an IR signals file at all.
int listButtons(const char* text, int len, Header* out, int maxOut);

/// Re-scan `text` and fill `out` with the button at `index` (0-based, as
/// returned by listButtons).  Returns false when the index does not exist or
/// the button references a protocol this firmware cannot transmit -- check
/// `entry.raw` / `ir::encodable()` yourself if you need to tell those apart.
bool loadButton(const char* text, int len, int index, Entry* out);

/// Serialise `n` entries as a Flipper file.  Returns the number of characters
/// written (excluding the NUL), or -1 when `cap` is too small.
int formatFile(const Entry* entries, int n, char* out, int cap);

/// Serialise one parsed button *without* the file header: the '#' separator
/// line plus name/type/protocol/address/command.  Used to append to a file the
/// caller has already opened and seeded with "Filetype: ...\nVersion: 1\n"
/// (see ir_app's library export).  Returns the number of characters written
/// (excluding the NUL), or -1 when `cap` is too small.
int formatEntry(const ir::Signal& signal, const char* name, char* out, int cap);

}  // namespace irfile
