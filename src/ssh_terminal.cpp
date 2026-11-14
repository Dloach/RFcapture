#include "ssh_terminal.h"

#include <cstring>

namespace sshterm {
namespace {

char screen[kRows][kCols + 1];
int row = 0;
int col = 0;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool dirty = true;

// Escape parsing state must survive a read boundary: libssh can split a CSI
// sequence across two reads, and resetting per chunk would print the tail.
int parserState = 0;  // 0 ground, 1 esc, 2 csi, 3 osc, 4 osc-esc
char csiArgs[24];
int csiLen = 0;

int parseNum(const char* s, int def)
{
    if (s[0] == 0) return def;
    int v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

void blankRow(int r)
{
    memset(screen[r], ' ', kCols);
    screen[r][kCols] = 0;
}

void clearLocked()
{
    for (int r = 0; r < kRows; ++r) blankRow(r);
    row = 0;
    col = 0;
}

void scrollLocked()
{
    for (int r = 1; r < kRows; ++r) memcpy(screen[r - 1], screen[r], kCols + 1);
    blankRow(kRows - 1);
    row = kRows - 1;
}

void newlineLocked()
{
    col = 0;
    if (++row >= kRows) scrollLocked();
}

void putLocked(char c)
{
    if (c == '\r') {
        col = 0;
        return;
    }
    if (c == '\n') {
        newlineLocked();
        return;
    }
    if (c == '\b') {
        if (col > 0) --col;
        return;
    }
    if (c == '\t') {
        col = (col + 8) & ~7;
        if (col >= kCols) newlineLocked();
        return;
    }
    if (static_cast<unsigned char>(c) < 32) return;
    if (col >= kCols) newlineLocked();
    screen[row][col++] = c;
}

// Minimal VT100: enough for a shell, ls, nano and most prompts.  Unknown CSI
// finals are ignored, which is safer than dumping escape text on the panel.
void csiFinalLocked(char final, const char* args)
{
    if (final == 'A') {
        const int n = parseNum(args, 1);
        row -= n;
        if (row < 0) row = 0;
    } else if (final == 'B') {
        const int n = parseNum(args, 1);
        row += n;
        if (row >= kRows) row = kRows - 1;
    } else if (final == 'C') {
        const int n = parseNum(args, 1);
        col += n;
        if (col >= kCols) col = kCols - 1;
    } else if (final == 'D') {
        const int n = parseNum(args, 1);
        col -= n;
        if (col < 0) col = 0;
    } else if (final == 'H' || final == 'f') {
        int r = 1, c = 1;
        const char* semi = strchr(args, ';');
        if (semi != nullptr) {
            char a[12];
            const int n = semi - args;
            if (n > 0 && n < (int)sizeof(a)) {
                memcpy(a, args, n);
                a[n] = 0;
                r = parseNum(a, 1);
            }
            c = parseNum(semi + 1, 1);
        } else {
            r = parseNum(args, 1);
        }
        row = r - 1;
        col = c - 1;
        if (row < 0) row = 0;
        if (row >= kRows) row = kRows - 1;
        if (col < 0) col = 0;
        if (col >= kCols) col = kCols - 1;
    } else if (final == 'J') {
        if (parseNum(args, 0) == 2 || args[0] == 0) {
            clearLocked();
        } else {
            for (int c = col; c < kCols; ++c) screen[row][c] = ' ';
            for (int r = row + 1; r < kRows; ++r) blankRow(r);
        }
    } else if (final == 'K') {
        if (parseNum(args, 0) == 2) {
            for (int c = 0; c < kCols; ++c) screen[row][c] = ' ';
        } else {
            for (int c = col; c < kCols; ++c) screen[row][c] = ' ';
        }
    } else if (final == 'S') {
        for (int n = parseNum(args, 1); n > 0; --n) scrollLocked();
        row = kRows - 1;
        col = 0;
    } else if (final == 'T') {
        for (int n = parseNum(args, 1); n > 0; --n) {
            for (int r = kRows - 1; r > 0; --r) memcpy(screen[r], screen[r - 1], kCols + 1);
            blankRow(0);
        }
        row = kRows - 1;
        col = 0;
    } else if (final == 'm' || final == 'r' || final == 'h' || final == 'l' ||
               final == 'L' || final == 'M' || final == 'P' || final == '@') {
        // SGR / scroll region / mode / insert-delete: no-op for this panel.
    }
}

}  // namespace

void clear()
{
    portENTER_CRITICAL(&mux);
    clearLocked();
    parserState = 0;
    csiLen      = 0;
    csiArgs[0]  = 0;
    dirty = true;
    portEXIT_CRITICAL(&mux);
}

void feed(const char* data, int len)
{
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < len; ++i) {
        const char c = data[i];
        if (parserState == 0) {
            if (c == 0x1b) {
                parserState = 1;
            } else if (static_cast<unsigned char>(c) < 0x80) {
                putLocked(c);
            }
        } else if (parserState == 1) {
            if (c == '[') {
                parserState = 2;
                csiLen      = 0;
                csiArgs[0]  = 0;
            } else if (c == ']') {
                parserState = 3;
            } else {
                parserState = 0;
            }
        } else if (parserState == 2) {
            if (c >= 0x40 && c <= 0x7e) {
                csiFinalLocked(c, csiArgs);
                parserState = 0;
            } else if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
                if (csiLen < (int)sizeof(csiArgs) - 1) csiArgs[csiLen++] = c;
                csiArgs[csiLen] = 0;
            }
        } else if (parserState == 3) {
            if (c == 0x07) {
                parserState = 0;
            } else if (c == 0x1b) {
                parserState = 4;
            }
        } else if (parserState == 4) {
            // ESC \ terminates an OSC string; either way the next byte starts
            // a fresh ground state.
            parserState = 0;
        }
    }
    dirty = true;
    portEXIT_CRITICAL(&mux);
}

void snapshot(char out[kRows][kCols + 1])
{
    portENTER_CRITICAL(&mux);
    memcpy(out, screen, sizeof(screen));
    portEXIT_CRITICAL(&mux);
}

bool takeDirty()
{
    bool d = dirty;
    dirty  = false;
    return d;
}

}  // namespace sshterm
