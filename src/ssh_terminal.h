#pragma once

#include <Arduino.h>

namespace sshterm {

constexpr int kCols = 40;
constexpr int kRows = 13;

void clear();
void feed(const char* data, int len);

/// Copy the visible screen for the UI.  The SSH task owns writes; this function
/// takes the same critical section so a draw never sees a half-scrolled row.
void snapshot(char out[kRows][kCols + 1]);

bool takeDirty();

}  // namespace sshterm
