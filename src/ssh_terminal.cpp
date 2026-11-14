#include "ssh_terminal.h"

#include <cstring>

namespace sshterm {
namespace {

constexpr int kHistoryRows = 64;

Cell screen[kRows][kCols];
Cell saved[kRows][kCols];
Cell history[kHistoryRows][kCols];
bool altActive  = false;
int histCount    = 0;
int historyNext  = 0;
int row          = 0;
int col          = 0;
int scrollTop    = 0;
int scrollBot    = kRows - 1;
int savedRow     = 0;
int savedCol     = 0;
uint8_t curFg    = 7;
uint8_t curBg    = 0;
bool reverse     = false;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool dirty = true;

// Escape parsing state must survive a read boundary: libssh can split a CSI
// sequence across two reads, and resetting per chunk would print the tail.
int parserState = 0;  // 0 ground, 1 esc, 2 csi, 3 osc, 4 osc-esc
char csiArgs[24];
int csiLen = 0;

// UTF-8 decode state (the cell stores one character; multi-byte glyphs are
// mapped to an ASCII fallback).
int utf8Need = 0;
uint32_t utf8Code = 0;

int parseNum(const char* s, int def)
{
    if (s[0] == 0) return def;
    int v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; ++i) {
        v = v * 10 + (s[i] - '0');
        if (v > 9999) return 9999;  // saturate: callers clamp to a screen bound
    }
    return v;
}

void blankRow(int r)
{
    for (int c = 0; c < kCols; ++c) screen[r][c] = Cell{' ', 7, 0};
}

Cell eraseCell()
{
    return Cell{' ', 7, curBg};
}

void eraseRow(int r)
{
    for (int c = 0; c < kCols; ++c) screen[r][c] = eraseCell();
}

void clearLocked()
{
    for (int r = 0; r < kRows; ++r) blankRow(r);
    row      = 0;
    col      = 0;
    scrollTop = 0;
    scrollBot = kRows - 1;
    savedRow  = 0;
    savedCol  = 0;
    curFg    = 7;
    curBg    = 0;
    reverse  = false;
    histCount    = 0;
    historyNext  = 0;
    altActive    = false;
    utf8Need     = 0;
    utf8Code     = 0;
}

void pushHistory(const Cell* line)
{
    memcpy(history[historyNext], line, sizeof(history[historyNext]));
    historyNext = (historyNext + 1) % kHistoryRows;
    if (histCount < kHistoryRows) ++histCount;
}

void scrollLocked()
{
    pushHistory(screen[scrollTop]);
    for (int r = scrollTop + 1; r <= scrollBot; ++r) {
        memcpy(screen[r - 1], screen[r], sizeof(screen[r]));
    }
    eraseRow(scrollBot);
    row = scrollBot;
}

void newlineLocked()
{
    col = 0;
    if (row >= scrollBot) {
        scrollLocked();
    } else {
        ++row;
    }
}

void putCellLocked(char c)
{
    uint8_t fg = curFg;
    uint8_t bg = curBg;
    if (reverse) {
        const uint8_t t = fg;
        fg              = bg;
        bg              = t;
    }
    screen[row][col++] = Cell{c, fg, bg};
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
    putCellLocked(c);
}

// ASCII fallback for the box-drawing characters used by ncurses/htop.  The
// panel font is 6x8; replacing them keeps menus readable instead of showing
// replacement boxes.
void putCodepointLocked(uint32_t cp)
{
    char c = '?';
    if (cp >= 0x2500 && cp <= 0x257f) {
        if (cp == 0x2502 || cp == 0x2503) c = '|';
        else if (cp == 0x2500 || cp == 0x2501) c = '-';
        else c = '+';
    } else if (cp >= 0x2580 && cp <= 0x259f) {
        c = '#';
    } else if (cp == 0x2026) {
        c = '.';
    } else if (cp == 0x00a0) {
        c = ' ';
    } else if (cp < 0x80) {
        c = static_cast<char>(cp);
    }
    if (col >= kCols) newlineLocked();
    putCellLocked(c);
}

void applySgrLocked(const char* args)
{
    int values[8];
    int n = 0;
    const char* p = args;
    while (n < 8) {
        values[n++] = parseNum(p, 0);
        const char* semi = strchr(p, ';');
        if (semi == nullptr) break;
        p = semi + 1;
    }
    if (n == 0) values[n++] = 0;
    for (int i = 0; i < n; ++i) {
        const int v = values[i];
        if (v == 0) {
            curFg   = 7;
            curBg   = 0;
            reverse = false;
        } else if (v == 1) {
            curFg |= 8;
        } else if (v == 7) {
            reverse = true;
        } else if (v == 27) {
            reverse = false;
        } else if (v == 39) {
            curFg = 7;
        } else if (v == 49) {
            curBg = 0;
        } else if (v >= 30 && v <= 37) {
            curFg = static_cast<uint8_t>(v - 30);
        } else if (v >= 40 && v <= 47) {
            curBg = static_cast<uint8_t>(v - 40);
        } else if (v >= 90 && v <= 97) {
            curFg = static_cast<uint8_t>(v - 90 + 8);
        } else if (v >= 100 && v <= 107) {
            curBg = static_cast<uint8_t>(v - 100 + 8);
        }
    }
}

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
        const int mode = parseNum(args, 0);
        if (mode == 2) {
            for (int r = 0; r < kRows; ++r) eraseRow(r);
        } else if (mode == 1) {
            for (int r = 0; r < row; ++r) eraseRow(r);
            for (int c = 0; c <= col && c < kCols; ++c) screen[row][c] = eraseCell();
        } else {
            for (int c = col; c < kCols; ++c) screen[row][c] = eraseCell();
            for (int r = row + 1; r < kRows; ++r) eraseRow(r);
        }
    } else if (final == 'K') {
        const int mode = parseNum(args, 0);
        if (mode == 2) {
            eraseRow(row);
        } else if (mode == 1) {
            for (int c = 0; c <= col && c < kCols; ++c) screen[row][c] = eraseCell();
        } else {
            for (int c = col; c < kCols; ++c) screen[row][c] = eraseCell();
        }
    } else if (final == 'S') {
        int n = parseNum(args, 1);
        if (n < 1) n = 1;
        if (n > kRows) n = kRows;
        for (; n > 0; --n) scrollLocked();
        row = scrollBot;
        col = 0;
    } else if (final == 'T') {
        int n = parseNum(args, 1);
        if (n < 1) n = 1;
        if (n > kRows) n = kRows;
        for (; n > 0; --n) {
            for (int r = scrollBot; r > scrollTop; --r) memcpy(screen[r], screen[r - 1], sizeof(screen[r]));
            eraseRow(scrollTop);
        }
        row = scrollBot;
        col = 0;
    } else if (final == 'L') {
        int n = parseNum(args, 1);
        if (n < 1) n = 1;
        if (n > kRows) n = kRows;
        for (; n > 0; --n) {
            for (int r = scrollBot; r > row; --r) memcpy(screen[r], screen[r - 1], sizeof(screen[r]));
            eraseRow(row);
        }
    } else if (final == 'M') {
        int n = parseNum(args, 1);
        if (n < 1) n = 1;
        if (n > kRows) n = kRows;
        for (; n > 0; --n) {
            for (int r = row; r < scrollBot; ++r) memcpy(screen[r], screen[r + 1], sizeof(screen[r]));
            eraseRow(scrollBot);
        }
    } else if (final == 'P') {
        int n = parseNum(args, 1);
        if (n < 1) n = 1;
        if (n > kCols - col) n = kCols - col;
        for (int c = col; c < kCols - n; ++c) screen[row][c] = screen[row][c + n];
        for (int c = kCols - n; c < kCols; ++c) screen[row][c] = eraseCell();
    } else if (final == '@') {
        int n = parseNum(args, 1);
        if (n < 1) n = 1;
        if (n > kCols - col) n = kCols - col;
        for (int c = kCols - 1; c >= col + n; --c) screen[row][c] = screen[row][c - n];
        for (int c = col; c < col + n; ++c) screen[row][c] = eraseCell();
    } else if (final == 'r') {
        int top = 1, bottom = kRows;
        const char* semi = strchr(args, ';');
        if (semi != nullptr) {
            char a[12];
            const int n = semi - args;
            if (n > 0 && n < (int)sizeof(a)) {
                memcpy(a, args, n);
                a[n] = 0;
                top = parseNum(a, 1);
            }
            bottom = parseNum(semi + 1, kRows);
        } else {
            top = parseNum(args, 1);
        }
        if (top < 1) top = 1;
        if (bottom > kRows) bottom = kRows;
        if (top >= bottom) return;
        scrollTop = top - 1;
        scrollBot = bottom - 1;
        row       = scrollTop;
        col       = 0;
    } else if (final == 's') {
        savedRow = row;
        savedCol = col;
    } else if (final == 'u') {
        row = savedRow;
        col = savedCol;
        if (row < 0) row = 0;
        if (row >= kRows) row = kRows - 1;
        if (col < 0) col = 0;
        if (col >= kCols) col = kCols - 1;
    } else if (final == 'm') {
        applySgrLocked(args);
    } else if ((final == 'h' || final == 'l') && strstr(args, "1049") != nullptr) {
        if (final == 'h' && !altActive) {
            memcpy(saved, screen, sizeof(saved));
            altActive = true;
            for (int r = 0; r < kRows; ++r) blankRow(r);
            row = 0;
            col = 0;
        } else if (final == 'l' && altActive) {
            memcpy(screen, saved, sizeof(screen));
            altActive = false;
            row       = 0;
            col       = 0;
        }
    }
}

void feedGroundLocked(char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x80) {
        putLocked(c);
        return;
    }
    if (utf8Need == 0) {
        if ((uc & 0xE0) == 0xC0) {
            utf8Need = 1;
            utf8Code = uc & 0x1F;
        } else if ((uc & 0xF0) == 0xE0) {
            utf8Need = 2;
            utf8Code = uc & 0x0F;
        } else if ((uc & 0xF8) == 0xF0) {
            utf8Need = 3;
            utf8Code = uc & 0x07;
        }
        return;
    }
    if ((uc & 0xC0) != 0x80) {
        utf8Need = 0;
        return;
    }
    utf8Code = (utf8Code << 6) | (uc & 0x3F);
    if (--utf8Need == 0) putCodepointLocked(utf8Code);
}

}  // namespace

void clear()
{
    portENTER_CRITICAL(&mux);
    clearLocked();
    parserState = 0;
    csiLen      = 0;
    csiArgs[0]  = 0;
    dirty       = true;
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
            } else {
                feedGroundLocked(c);
            }
        } else if (parserState == 1) {
            if (c == '[') {
                parserState = 2;
                csiLen      = 0;
                csiArgs[0]  = 0;
            } else if (c == ']') {
                parserState = 3;
            } else if (c == '7') {
                savedRow    = row;
                savedCol    = col;
                parserState = 0;
            } else if (c == '8') {
                row         = savedRow;
                col         = savedCol;
                parserState = 0;
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
            parserState = 0;
        }
    }
    dirty = true;
    portEXIT_CRITICAL(&mux);
}

void snapshot(Cell out[kRows][kCols], int scrollOffset)
{
    portENTER_CRITICAL(&mux);
    int offset = scrollOffset;
    if (offset < 0) offset = 0;
    if (offset > histCount) offset = histCount;

    const int total = histCount + kRows;
    int start = total - kRows - offset;
    if (start < 0) start = 0;
    for (int r = 0; r < kRows; ++r) {
        const int logical = start + r;
        if (logical < histCount) {
            const int idx = (historyNext - histCount + logical + kHistoryRows) % kHistoryRows;
            memcpy(out[r], history[idx], sizeof(out[r]));
        } else {
            memcpy(out[r], screen[logical - histCount], sizeof(out[r]));
        }
    }
    portEXIT_CRITICAL(&mux);
}

int historyCount()
{
    int n;
    portENTER_CRITICAL(&mux);
    n = histCount;
    portEXIT_CRITICAL(&mux);
    return n;
}

bool takeDirty()
{
    bool d;
    portENTER_CRITICAL(&mux);
    d     = dirty;
    dirty = false;
    portEXIT_CRITICAL(&mux);
    return d;
}

}  // namespace sshterm
