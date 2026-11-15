/*
 * Saved IR codes (NVS).
 *
 * Only *parsed* codes live here: a protocol plus its address/command is 14
 * bytes, so the whole library is one small NVS blob.  Raw captures are far
 * bigger (a full A/C frame is 200-600 timings) and belong on the TF card as a
 * Flipper `.ir` file; see ir_file.h.  That split keeps this store cheap enough
 * to rewrite on every change.
 *
 * The blob carries a magic, a version and a checksum so an interrupted write
 * (battery pulled mid-save) is detected instead of loading garbage.
 */

#pragma once

#include <stdint.h>

#include "ir_protocol.h"

namespace irstore {

constexpr int kMaxCodes = 48;
constexpr int kNameLen  = 20;

struct Code {
    char name[kNameLen];
    uint8_t proto;  // ir::Protocol
    bool toggle;
    uint32_t address;
    uint32_t command;
};

void begin();

int count();
const Code& at(int i);

/// Returns the new index, or -1 when the library is full.
int add(const char* name, const ir::Signal& signal);

bool remove(int i);
bool rename(int i, const char* name);

/// Signal view of a stored code.
ir::Signal signalOf(const Code& c);

/// Find a code with the same protocol/address/command, or -1.  Used to avoid
/// storing the same button twice when importing a file.
int find(const ir::Signal& signal);

void clear();

}  // namespace irstore
