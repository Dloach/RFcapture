// Card storage: a plain text file on SPIFFS.
//
// Deliberately the simplest thing that cannot lose data silently:
//
//   * one record per line, ';' separated, name last so it may contain spaces
//   * written to a temp file and renamed, so an interrupted save cannot truncate
//     the collection
//   * the previous file is kept as .bak until the rename succeeded
//
// No SD card: an optional card is one more thing that can be absent, and the
// collection is small (a few hundred bytes).
#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>

namespace store {

constexpr uint8_t kMaxCards = 16;
constexpr uint8_t kUidMax   = 10;
constexpr size_t kNameLen   = 17;
constexpr size_t kTypeLen   = 24;

struct Card {
    uint8_t uid[kUidMax];
    uint8_t len;
    uint16_t atqa;
    uint8_t sak;
    uint16_t hits;
    char type[kTypeLen];
    char name[kNameLen];
};

Card cards[kMaxCards];
uint8_t count = 0;
const char* lastError = nullptr;

namespace detail {

// Its own file on purpose: the main firmware keeps /nfc/cards.txt with eight
// fields per line, and two programs writing one path with two formats would
// corrupt each other's collection.  The main file is only ever read, once, to
// import what is already there.
constexpr char kDir[]    = "/nfc";
constexpr char kPath[]   = "/nfc/retry.txt";
constexpr char kTmp[]    = "/nfc/retry.tmp";
constexpr char kBak[]    = "/nfc/retry.bak";
constexpr char kImport[] = "/nfc/cards.txt";

void uidHex(const Card& c, char* out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (uint8_t i = 0; i < c.len && o + 2 < n; ++i) {
        out[o++] = hex[c.uid[i] >> 4];
        out[o++] = hex[c.uid[i] & 0x0F];
    }
    out[o] = '\0';
}

void sanitize(char* s)
{
    char* w = s;
    for (char* r = s; *r; ++r) {
        if (*r == ';' || *r == '\n' || *r == '\r') continue;
        *w++ = *r;
    }
    *w = '\0';
    while (w > s && w[-1] == ' ') *--w = '\0';
}

// Default name is the last four hex digits of the UID: short enough for the list
// column and unique enough across a handful of cards.  It is a placeholder -- the
// UI shows the full UID until the operator renames the record.
void defaultName(const Card& c, char* out, size_t n)
{
    char hex[24];
    uidHex(c, hex, sizeof(hex));
    const size_t l    = strlen(hex);
    const char* tail  = l > 4 ? hex + l - 4 : hex;
    snprintf(out, n, "%s", tail);
}

bool parseLine(const char* line, Card& out)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", line);

    char* f[6];
    uint8_t n = 0;
    char* p   = buf;
    f[n++]    = p;
    while (*p && n < 6) {
        if (*p == ';') {
            *p     = '\0';
            f[n++] = p + 1;
        }
        ++p;
    }
    if (n != 6) return false;

    memset(&out, 0, sizeof(out));
    out.len = static_cast<uint8_t>(atoi(f[0]));
    if (out.len == 0 || out.len > kUidMax) return false;

    uint8_t got = 0;
    for (const char* s = f[1]; s[0] && s[1] && got < kUidMax; s += 2) {
        auto hv = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        };
        const int hi = hv(s[0]), lo = hv(s[1]);
        if (hi < 0 || lo < 0) break;
        out.uid[got++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (got != out.len) return false;

    out.atqa = static_cast<uint16_t>(strtoul(f[2], nullptr, 16) & 0xFFFF);
    out.sak  = static_cast<uint8_t>(strtoul(f[3], nullptr, 16) & 0xFF);
    out.hits = static_cast<uint16_t>(atoi(f[4]));
    snprintf(out.name, sizeof(out.name), "%s", f[5]);
    sanitize(out.name);
    if (out.name[0] == 0) defaultName(out, out.name, sizeof(out.name));
    snprintf(out.type, sizeof(out.type), "%s", "NFC-A");
    return true;
}

}  // namespace detail

int8_t find(const uint8_t len, const uint8_t* uid)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (cards[i].len == len && memcmp(cards[i].uid, uid, len) == 0) return static_cast<int8_t>(i);
    }
    return -1;
}

bool save()
{
    File f = SPIFFS.open(detail::kTmp, FILE_WRITE);
    if (!f) {
        lastError = "write failed";
        return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        char hex[24];
        detail::uidHex(cards[i], hex, sizeof(hex));
        // len;uid;atqa;sak;hits;name
        f.printf("%u;%s;%04X;%02X;%u;%s\n", static_cast<unsigned>(cards[i].len), hex,
                 static_cast<unsigned>(cards[i].atqa), static_cast<unsigned>(cards[i].sak),
                 static_cast<unsigned>(cards[i].hits), cards[i].name);
    }
    f.close();

    if (SPIFFS.exists(detail::kPath)) {
        if (SPIFFS.exists(detail::kBak)) SPIFFS.remove(detail::kBak);
        SPIFFS.rename(detail::kPath, detail::kBak);
    }
    if (!SPIFFS.rename(detail::kTmp, detail::kPath)) {
        lastError = "rename failed";
        return false;
    }
    if (SPIFFS.exists(detail::kBak)) SPIFFS.remove(detail::kBak);
    lastError = nullptr;
    return true;
}

// Adds or bumps a card.  Returns the index, or -1 when the collection is full.
int8_t add(const Card& c)
{
    const int8_t at = find(c.len, c.uid);
    if (at >= 0) {
        Card& e = cards[at];
        if (e.hits < 0xFFFF) ++e.hits;
        e.atqa = c.atqa;
        e.sak  = c.sak;
        snprintf(e.type, sizeof(e.type), "%s", c.type);
        save();
        return at;
    }
    if (count >= kMaxCards) {
        lastError = "collection full";
        return -1;
    }
    cards[count] = c;
    if (cards[count].hits == 0) cards[count].hits = 1;
    if (cards[count].name[0] == 0) detail::defaultName(cards[count], cards[count].name,
                                                       sizeof(cards[count].name));
    const int8_t idx = static_cast<int8_t>(count);
    ++count;
    save();
    return idx;
}

void remove(const int8_t i)
{
    if (i < 0 || i >= count) return;
    for (uint8_t k = static_cast<uint8_t>(i); k + 1 < count; ++k) cards[k] = cards[k + 1];
    --count;
    memset(&cards[count], 0, sizeof(Card));
    save();
}

void rename(const int8_t i, const char* name)
{
    if (i < 0 || i >= count || name == nullptr || name[0] == 0) return;
    snprintf(cards[i].name, sizeof(cards[i].name), "%s", name);
    detail::sanitize(cards[i].name);
    save();
}

// Only a 4 or 7 byte UID can go back on the air; load_config() rejects 10.
bool replayable(const Card& c)
{
    return c.len == 4 || c.len == 7;
}

void uidToString(const Card& c, char* out, size_t n)
{
    detail::uidHex(c, out, n);
}

// The main firmware's format: 1;uid;len;atqa;sak;hits;type;name
bool importLine(const char* line, Card& out)
{
    char buf[192];
    snprintf(buf, sizeof(buf), "%s", line);
    char* f[8];
    uint8_t n = 0;
    char* p   = buf;
    f[n++]    = p;
    while (*p && n < 8) {
        if (*p == ';') {
            *p     = '\0';
            f[n++] = p + 1;
        }
        ++p;
    }
    if (n != 8) return false;

    memset(&out, 0, sizeof(out));
    out.len = static_cast<uint8_t>(atoi(f[2]));
    if (out.len == 0 || out.len > kUidMax) return false;

    uint8_t got = 0;
    for (const char* s = f[1]; s[0] && s[1] && got < kUidMax; s += 2) {
        auto hv = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            return -1;
        };
        const int hi = hv(s[0]), lo = hv(s[1]);
        if (hi < 0 || lo < 0) break;
        out.uid[got++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (got != out.len) return false;

    out.atqa = static_cast<uint16_t>(strtoul(f[3], nullptr, 16) & 0xFFFF);
    out.sak  = static_cast<uint8_t>(strtoul(f[4], nullptr, 16) & 0xFF);
    out.hits = static_cast<uint16_t>(atoi(f[5]));
    snprintf(out.type, sizeof(out.type), "%s", f[6]);
    snprintf(out.name, sizeof(out.name), "%s", f[7]);
    detail::sanitize(out.name);
    if (out.name[0] == 0) detail::defaultName(out, out.name, sizeof(out.name));
    return true;
}

// One-time import from the main firmware's collection, so the card that was
// captured there does not have to be re-scanned.
void importFromMain()
{
    if (!SPIFFS.exists(detail::kImport)) return;
    File f = SPIFFS.open(detail::kImport, FILE_READ);
    if (!f) return;
    char line[192];
    uint8_t imported = 0;
    while (f.available() > 0 && count < kMaxCards) {
        size_t n = 0;
        while (f.available() > 0 && n + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[n++] = static_cast<char>(ch);
        }
        line[n] = '\0';
        if (n == 0) continue;
        Card c;
        if (importLine(line, c)) {
            cards[count++] = c;
            ++imported;
        }
    }
    f.close();
    Serial.printf("[store] imported %u card(s) from %s\n", static_cast<unsigned>(imported),
                  detail::kImport);
    if (imported) save();
}

int8_t load()
{
    count = 0;
    if (!SPIFFS.begin(false) && !SPIFFS.begin(true)) {
        lastError = "SPIFFS not mounted";
        return 0;
    }
    if (!SPIFFS.exists(detail::kDir)) SPIFFS.mkdir(detail::kDir);

    const char* path = nullptr;
    if (SPIFFS.exists(detail::kPath)) {
        path = detail::kPath;
    } else if (SPIFFS.exists(detail::kBak)) {
        path      = detail::kBak;
        lastError = "recovered from backup";
    } else {
        importFromMain();
        return static_cast<int8_t>(count);
    }

    File f = SPIFFS.open(path, FILE_READ);
    if (!f) return 0;
    char line[160];
    while (f.available() > 0 && count < kMaxCards) {
        size_t n = 0;
        while (f.available() > 0 && n + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[n++] = static_cast<char>(ch);
        }
        line[n] = '\0';
        if (n == 0) continue;
        Card c;
        if (detail::parseLine(line, c)) cards[count++] = c;
    }
    f.close();
    return static_cast<int8_t>(count);
}

}  // namespace store
