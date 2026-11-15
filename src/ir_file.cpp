/*
 * Flipper Zero .ir remote file parser / serialiser.
 *
 * Format ("IR signals file", Version 1).  A file starts with two header lines
 * that carry no signal data, then a '#' separator line before every button
 * record; a record is made of `key: value` lines:
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
 * `name` is free text (trimmed, printable ASCII).  `type` is `parsed` or `raw`.
 * A parsed record carries `protocol`, `address` and `command`; a raw record
 * carries `frequency`, `duty_cycle` and `data` (alternating mark/space
 * microseconds, starting with a mark).  Records are delimited by '#' lines, and
 * also by a new `name:` line so a file that forgot a separator still parses.
 * Blank lines are ignored, unknown keys are ignored, and both CRLF and LF line
 * endings are accepted.  A missing `Filetype`/`Version` header is tolerated:
 * the file is recognised by any of the keys above.
 *
 * Little-endian byte order
 * ------------------------
 * Flipper writes `address` and `command` as four space separated 2-digit hex
 * bytes, least significant byte first: "EE 87 00 00" is 0x000087EE, not
 * 0xEE870000.  This file assembles/emits the bytes in exactly that order, so a
 * parsed record survives a write/read round trip bit for bit.
 *
 * No allocation, no Arduino, no mutable globals: every buffer is a local of
 * fixed size (one Record, which is Entry plus bookkeeping) and parsing is a
 * single pass over the caller's text.
 */

#include "ir_file.h"

#include <cstdarg>
#include <cstdio>

namespace irfile {
namespace {

constexpr int kNameCap  = 24;  // Header::name / Entry::name
constexpr int kProtoCap = 24;  // longest Flipper protocol spelling + slack

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

inline bool isSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/// Case-insensitive compare of a (not NUL terminated) span against a literal.
bool matchKey(const char* p, int len, const char* lit) {
    int i = 0;
    for (; i < len; ++i) {
        char a = p[i];
        char b = lit[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (b == '\0' || a != b) return false;
    }
    return lit[i] == '\0';
}

/// Copy a trimmed, printable-ASCII-only name into a NUL terminated buffer.
void copyText(const char* p, int len, char* dst, int cap) {
    if (cap <= 0) return;
    int begin = 0;
    while (begin < len && isSpaceChar(p[begin])) ++begin;
    int end = len;
    while (end > begin && isSpaceChar(p[end - 1])) --end;
    int o = 0;
    for (int i = begin; i < end && o < cap - 1; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if (c < 0x20 || c > 0x7E) continue;  // printable ASCII only
        dst[o++] = static_cast<char>(c);
    }
    dst[o] = '\0';
}

void trimSpan(const char*& p, int& len) {
    while (len > 0 && isSpaceChar(*p)) {
        ++p;
        --len;
    }
    while (len > 0 && isSpaceChar(p[len - 1])) --len;
}

/// Split "key: value"; false when the line has no key at all.
bool splitKeyValue(const char* line, int len, const char** key, int* keyLen,
                   const char** val, int* valLen) {
    int colon = -1;
    for (int i = 0; i < len; ++i) {
        if (line[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon < 0) return false;
    *key    = line;
    *keyLen = colon;
    *val    = line + colon + 1;
    *valLen = len - colon - 1;
    trimSpan(*key, *keyLen);
    trimSpan(*val, *valLen);
    return *keyLen > 0;
}

bool isSeparatorLine(const char* line, int len) {
    int i = 0;
    while (i < len && isSpaceChar(line[i])) ++i;
    return i < len && line[i] == '#';
}

bool isBlankLine(const char* line, int len) {
    for (int i = 0; i < len; ++i) {
        if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r') return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Line cursor over the caller's (not NUL terminated) text
// ---------------------------------------------------------------------------

struct Cursor {
    const char* text;
    int len;
    int pos;
};

bool nextLine(Cursor& c, const char** line, int* lineLen) {
    if (c.pos >= c.len || c.pos < 0) return false;
    const int start = c.pos;
    int i           = start;
    while (i < c.len && c.text[i] != '\n') ++i;
    int end = i;
    if (end > start && c.text[end - 1] == '\r') --end;  // CRLF
    c.pos = (i < c.len) ? i + 1 : i;
    *line    = c.text + start;
    *lineLen = end - start;
    return true;
}

// ---------------------------------------------------------------------------
// Value parsers
// ---------------------------------------------------------------------------

/// Unsigned decimal.  Leading spaces are skipped; trailing junk is ignored.
bool parseU32(const char* p, int len, uint32_t* out) {
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) ++i;
    unsigned long long v = 0;
    bool any             = false;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        if (v <= 0xFFFFFFFFull) v = v * 10ull + static_cast<unsigned>(p[i] - '0');
        any = true;
        ++i;
    }
    if (!any) return false;
    if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;  // clamp, never wrap
    *out = static_cast<uint32_t>(v);
    return true;
}

/// One hex token: optional "0x", at least one hex digit, else 0.
uint32_t parseHexToken(const char* p, int len) {
    int i = 0;
    if (len >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) i = 2;
    uint32_t v = 0;
    bool any   = false;
    for (; i < len; ++i) {
        int d;
        const char c = p[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | static_cast<uint32_t>(d);
        any = true;
        if (v > 0xFFFFu) v = 0xFFFFu;  // clamp absurdly long tokens
    }
    return any ? v : 0;
}

/// Four space separated hex bytes, least significant byte first.
uint32_t parseHexField(const char* p, int len) {
    uint32_t result = 0;
    int byteIndex   = 0;
    int i           = 0;
    while (i < len && byteIndex < 4) {
        while (i < len && (p[i] == ' ' || p[i] == '\t')) ++i;
        if (i >= len) break;
        const int start = i;
        while (i < len && p[i] != ' ' && p[i] != '\t') ++i;
        uint32_t v = parseHexToken(p + start, i - start);
        if (v > 0xFFu) v = 0xFFu;  // clamp, keeps the byte slot
        result |= v << (8 * byteIndex);
        ++byteIndex;
    }
    return result;
}

/// Plain decimal fraction, e.g. "0.330000".  No <cmath>, no strtod on
/// non-NUL-terminated text.
bool parseFraction(const char* p, int len, double* out) {
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) ++i;
    double v  = 0.0;
    bool any  = false;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        v = v * 10.0 + static_cast<double>(p[i] - '0');
        any = true;
        ++i;
    }
    if (i < len && p[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            v += static_cast<double>(p[i] - '0') * scale;
            scale *= 0.1;
            ++i;
            any = true;
        }
    }
    if (!any) return false;
    *out = v;
    return true;
}

uint8_t clampDuty(double duty) {
    double pct = duty * 100.0 + 0.5;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return static_cast<uint8_t>(pct);
}

/// Append up to `room` decimal timings from a `data:` value.  Returns the
/// number appended; values above uint16 range are clamped to 0xFFFF.
int parseTimings(const char* p, int len, uint16_t* dst, int room) {
    int count = 0;
    if (room <= 0) return 0;
    int i = 0;
    while (i < len && count < room) {
        while (i < len && (p[i] == ' ' || p[i] == '\t')) ++i;
        if (i >= len) break;
        const int start = i;
        while (i < len && p[i] != ' ' && p[i] != '\t') ++i;
        uint32_t v = 0;
        if (!parseU32(p + start, i - start, &v)) continue;  // junk token
        if (v > 0xFFFFu) v = 0xFFFFu;
        dst[count++] = static_cast<uint16_t>(v);
    }
    return count;
}

// ---------------------------------------------------------------------------
// Record bookkeeping
// ---------------------------------------------------------------------------

struct Record {
    char name[kNameCap];
    bool hasName;
    bool hasType;
    bool typeRaw;
    bool hasData;
    uint32_t frequency;
    uint8_t dutyPercent;
    int count;
    uint16_t timings[ir::kMaxTimings];
    char proto[kProtoCap];
    bool hasProto;
    uint32_t address;
    uint32_t command;
};

void recordReset(Record* r) {
    for (int i = 0; i < kNameCap; ++i) r->name[i] = '\0';
    for (int i = 0; i < kProtoCap; ++i) r->proto[i] = '\0';
    r->hasName     = false;
    r->hasType     = false;
    r->typeRaw     = false;
    r->hasData     = false;
    r->frequency   = 38000;
    r->dutyPercent = 33;
    r->count       = 0;
    r->hasProto    = false;
    r->address     = 0;
    r->command     = 0;
}

/// raw unless the file explicitly says parsed.
bool recordIsRaw(const Record& r) {
    return r.hasType ? r.typeRaw : r.hasData;
}

/// Finalise a parsed record into `out`.  Returns false for a protocol this
/// firmware cannot transmit (the entry is still filled in, with kUnknown when
/// even the name was not recognised).
bool applyRecord(const Record& r, Entry* out) {
    for (int i = 0; i < kNameCap; ++i) out->name[i] = r.name[i];
    out->raw         = recordIsRaw(r);
    out->frequency   = r.frequency;
    out->dutyPercent = r.dutyPercent;
    out->count       = 0;
    out->signal.proto   = ir::Protocol::kUnknown;
    out->signal.address = 0;
    out->signal.command = 0;
    out->signal.toggle  = false;
    out->signal.repeat  = false;

    if (out->raw) {
        out->count = r.count;
        for (int i = 0; i < r.count; ++i) out->timings[i] = r.timings[i];
        return true;
    }
    out->signal.address = r.address;
    out->signal.command = r.command;
    if (!r.hasProto) return false;
    const ir::Protocol p = ir::protocolFromName(r.proto);
    out->signal.proto    = p;
    return p != ir::Protocol::kUnknown && ir::protocolSupported(r.proto);
}

// ---------------------------------------------------------------------------
// Header accumulation for listButtons()
// ---------------------------------------------------------------------------

struct HeaderState {
    char name[kNameCap];
    bool hasName;
    bool hasType;
    bool typeRaw;
    bool hasData;

    void reset() {
        for (int i = 0; i < kNameCap; ++i) name[i] = '\0';
        hasName = false;
        hasType = false;
        typeRaw = false;
        hasData = false;
    }
    bool raw() const { return hasType ? typeRaw : hasData; }
};

void flushHeader(HeaderState* st, Header* out, int maxOut, int* total) {
    if (!st->hasName) {
        st->reset();
        return;
    }
    if (out != nullptr && *total < maxOut) {
        for (int i = 0; i < kNameCap; ++i) out[*total].name[i] = st->name[i];
        out[*total].raw = st->raw();
    }
    ++(*total);
    st->reset();
}

// ---------------------------------------------------------------------------
// Output helpers for formatFile()
// ---------------------------------------------------------------------------

bool emitText(char* out, int cap, int* used, const char* s) {
    int len = 0;
    while (s[len] != '\0') ++len;
    if (*used + len + 1 > cap) return false;
    for (int i = 0; i < len; ++i) out[*used + i] = s[i];
    *used += len;
    return true;
}

bool emitFmt(char* out, int cap, int* used, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(out + *used, static_cast<size_t>(cap - *used), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= cap - *used) return false;  // -1: cap too small
    *used += n;
    return true;
}

/// protocol/address/command lines of a parsed record.  The four bytes are
/// written least significant first, matching Flipper.
bool emitParsedFields(const ir::Signal& sig, char* out, int cap, int* used) {
    const char* pn = ir::protocolName(sig.proto);
    if (pn == nullptr || pn[0] == '\0') pn = "UNKNOWN";
    if (!emitText(out, cap, used, "type: parsed\n")) return false;
    if (!emitFmt(out, cap, used, "protocol: %s\n", pn)) return false;
    if (!emitFmt(out, cap, used, "address: %02X %02X %02X %02X\n",
                 static_cast<unsigned>(sig.address & 0xFFu),
                 static_cast<unsigned>((sig.address >> 8) & 0xFFu),
                 static_cast<unsigned>((sig.address >> 16) & 0xFFu),
                 static_cast<unsigned>((sig.address >> 24) & 0xFFu)))
        return false;
    if (!emitFmt(out, cap, used, "command: %02X %02X %02X %02X\n",
                 static_cast<unsigned>(sig.command & 0xFFu),
                 static_cast<unsigned>((sig.command >> 8) & 0xFFu),
                 static_cast<unsigned>((sig.command >> 16) & 0xFFu),
                 static_cast<unsigned>((sig.command >> 24) & 0xFFu)))
        return false;
    return true;
}

/// frequency/duty_cycle/data lines of a raw record.
bool emitRawFields(uint32_t frequency, uint8_t dutyPercent, const uint16_t* timings, int count,
                   char* out, int cap, int* used) {
    if (!emitText(out, cap, used, "type: raw\n")) return false;
    if (!emitFmt(out, cap, used, "frequency: %u\n", static_cast<unsigned>(frequency))) return false;
    if (!emitFmt(out, cap, used, "duty_cycle: %.6f\n", static_cast<double>(dutyPercent) / 100.0))
        return false;
    if (!emitText(out, cap, used, "data:")) return false;
    for (int t = 0; t < count; ++t) {
        if (!emitFmt(out, cap, used, " %u", static_cast<unsigned>(timings[t]))) return false;
    }
    return emitText(out, cap, used, "\n");
}

/// One complete record, including its leading '#' separator line.
bool emitEntryBlock(const Entry& e, char* out, int cap, int* used) {
    if (!emitText(out, cap, used, "#\n")) return false;
    if (!emitFmt(out, cap, used, "name: %s\n", e.name)) return false;
    if (e.raw) return emitRawFields(e.frequency, e.dutyPercent, e.timings, e.count, out, cap, used);
    return emitParsedFields(e.signal, out, cap, used);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int listButtons(const char* text, int len, Header* out, int maxOut) {
    if (text == nullptr || len < 0) return -1;

    Cursor c = {text, len, 0};
    HeaderState st;
    st.reset();
    bool recognised = false;
    int total       = 0;

    const char* line;
    int lineLen;
    while (nextLine(c, &line, &lineLen)) {
        if (isBlankLine(line, lineLen)) continue;
        if (isSeparatorLine(line, lineLen)) {
            flushHeader(&st, out, maxOut, &total);
            continue;
        }
        const char* key;
        int keyLen;
        const char* val;
        int valLen;
        if (!splitKeyValue(line, lineLen, &key, &keyLen, &val, &valLen)) continue;

        if (matchKey(key, keyLen, "name")) {
            recognised = true;
            flushHeader(&st, out, maxOut, &total);
            copyText(val, valLen, st.name, kNameCap);
            st.hasName = true;
        } else if (matchKey(key, keyLen, "type")) {
            recognised = true;
            st.hasType = true;
            st.typeRaw = matchKey(val, valLen, "raw");
        } else if (matchKey(key, keyLen, "data")) {
            recognised = true;
            st.hasData = true;
        } else if (matchKey(key, keyLen, "protocol") || matchKey(key, keyLen, "address") ||
                   matchKey(key, keyLen, "command") || matchKey(key, keyLen, "frequency") ||
                   matchKey(key, keyLen, "duty_cycle") || matchKey(key, keyLen, "filetype") ||
                   matchKey(key, keyLen, "version")) {
            recognised = true;
        }
    }
    flushHeader(&st, out, maxOut, &total);

    if (!recognised) return -1;  // nothing that looks like an IR signals file
    if (total > maxOut) total = maxOut;
    if (total < 0) total = 0;
    return total;
}

bool loadButton(const char* text, int len, int index, Entry* out) {
    if (text == nullptr || len < 0 || out == nullptr || index < 0) return false;

    // Defined default so a false return still leaves a usable entry.
    out->name[0]       = '\0';
    out->raw           = false;
    out->frequency     = 38000;
    out->dutyPercent   = 33;
    out->count         = 0;
    out->signal.proto   = ir::Protocol::kUnknown;
    out->signal.address = 0;
    out->signal.command = 0;
    out->signal.toggle  = false;
    out->signal.repeat  = false;

    Record r;
    recordReset(&r);

    Cursor c = {text, len, 0};
    int indexOfName = 0;
    bool active     = false;

    const char* line;
    int lineLen;
    while (nextLine(c, &line, &lineLen)) {
        if (isBlankLine(line, lineLen)) continue;
        if (isSeparatorLine(line, lineLen)) {
            if (active) return applyRecord(r, out);
            continue;
        }
        const char* key;
        int keyLen;
        const char* val;
        int valLen;
        if (!splitKeyValue(line, lineLen, &key, &keyLen, &val, &valLen)) continue;

        if (matchKey(key, keyLen, "name")) {
            if (active) return applyRecord(r, out);
            if (indexOfName == index) {
                active = true;
                recordReset(&r);
                copyText(val, valLen, r.name, kNameCap);
                r.hasName = true;
            } else {
                ++indexOfName;
            }
            continue;
        }
        if (!active) continue;  // fields of a record we skipped

        if (matchKey(key, keyLen, "type")) {
            r.hasType = true;
            r.typeRaw = matchKey(val, valLen, "raw");
        } else if (matchKey(key, keyLen, "data")) {
            r.hasData = true;
            r.count += parseTimings(val, valLen, r.timings + r.count, ir::kMaxTimings - r.count);
        } else if (matchKey(key, keyLen, "protocol")) {
            copyText(val, valLen, r.proto, kProtoCap);
            r.hasProto = true;
        } else if (matchKey(key, keyLen, "address")) {
            r.address = parseHexField(val, valLen);
        } else if (matchKey(key, keyLen, "command")) {
            r.command = parseHexField(val, valLen);
        } else if (matchKey(key, keyLen, "frequency")) {
            uint32_t f = 0;
            if (parseU32(val, valLen, &f)) r.frequency = (f == 0) ? 38000u : f;
        } else if (matchKey(key, keyLen, "duty_cycle")) {
            double d = 0.0;
            if (parseFraction(val, valLen, &d)) r.dutyPercent = clampDuty(d);
        }
    }

    if (active) return applyRecord(r, out);
    return false;
}

int formatFile(const Entry* entries, int n, char* out, int cap) {
    if (out == nullptr || cap <= 0) return -1;
    if (n < 0) n = 0;
    if (entries == nullptr && n > 0) return -1;

    int used = 0;
    if (!emitText(out, cap, &used, "Filetype: IR signals file\nVersion: 1\n")) return -1;

    for (int i = 0; i < n; ++i) {
        if (!emitEntryBlock(entries[i], out, cap, &used)) return -1;
    }

    if (!emitText(out, cap, &used, "#\n")) return -1;
    out[used] = '\0';
    return used;
}

int formatEntry(const ir::Signal& signal, const char* name, char* out, int cap) {
    if (out == nullptr || cap <= 0) return -1;
    if (name == nullptr) name = "";

    int used = 0;
    if (!emitText(out, cap, &used, "#\n")) return -1;
    if (!emitFmt(out, cap, &used, "name: %s\n", name)) return -1;
    if (!emitParsedFields(signal, out, cap, &used)) return -1;
    out[used] = '\0';
    return used;
}

}  // namespace irfile
