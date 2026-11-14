#pragma once

#include <Arduino.h>

namespace sshterm {

constexpr int kCols = 40;
constexpr int kRows = 13;

struct Cell {
    char ch;
    uint8_t fg;
    uint8_t bg;
};

void clear();
void feed(const char* data, int len);

/// Copy a visible window.  scrollOffset=0 is the live screen; 1 shows one line
/// from the history above it.
void snapshot(Cell out[kRows][kCols], int scrollOffset);
int historyCount();

bool takeDirty();

}  // namespace sshterm
