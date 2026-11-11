// NFC card store -- definitions.
//
// A plain text file on SPIFFS, with one record per line and the name last so it
// may contain spaces.  save() writes a temp file and renames it into place, so an
// interrupted save cannot truncate the collection; load() falls back to the .bak
// left behind by that sequence.
//
// Its own file, not the RF firmware's: two programs writing one path with two
// formats would corrupt each other.  The main firmware's file is only read once,
// to import what is already there.

#include "nfc_cards.h"

#include <SPIFFS.h>

#include <cstring>

#include "sd_mount.h"

namespace nfccards {

Card cards[kMaxCards];
uint8_t count      = 0;
const char* lastError = nullptr;

namespace {

constexpr char kDir[]   = "/nfc";
constexpr char kPath[]  = "/nfc/retry.txt";
constexpr char kTmp[]   = "/nfc/retry.tmp";
constexpr char kBak[]   = "/nfc/retry.bak";
constexpr char kImport[] = "/nfc/cards.txt";

constexpr int kSdCs      = 12;
constexpr char kSdDir[]  = "/nfc";
constexpr char kSdPath[] = "/nfc/cards.bak";

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

// The last four hex digits of the UID: short enough for the list column, unique
// enough across a handful of cards.  A placeholder -- the UI shows the full UID
// until the operator renames the record.
void defaultName(const Card& c, char* out, size_t n)
{
    char hex[24];
    uidHex(c, hex, sizeof(hex));
    const size_t l   = strlen(hex);
    const char* tail = l > 4 ? hex + l - 4 : hex;
    snprintf(out, n, "%s", tail);
}

int hexDigit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

// This project's format: len;uid;atqa;sak;hits;name
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
        const int hi = hexDigit(s[0]), lo = hexDigit(s[1]);
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
        const int hi = hexDigit(s[0]), lo = hexDigit(s[1]);
        if (hi < 0 || lo < 0) break;
        out.uid[got++] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (got != out.len) return false;

    out.atqa = static_cast<uint16_t>(strtoul(f[3], nullptr, 16) & 0xFFFF);
    out.sak  = static_cast<uint8_t>(strtoul(f[4], nullptr, 16) & 0xFF);
    out.hits = static_cast<uint16_t>(atoi(f[5]));
    snprintf(out.type, sizeof(out.type), "%s", f[6]);
    snprintf(out.name, sizeof(out.name), "%s", f[7]);
    sanitize(out.name);
    if (out.name[0] == 0) defaultName(out, out.name, sizeof(out.name));
    return true;
}

void importFromMain()
{
    if (!SPIFFS.exists(kImport)) return;
    File f = SPIFFS.open(kImport, FILE_READ);
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
    Serial.printf("[nfc] imported %u card(s) from %s\n", static_cast<unsigned>(imported), kImport);
    if (imported) save();
}

}  // namespace

int8_t find(const uint8_t len, const uint8_t* uid)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (cards[i].len == len && memcmp(cards[i].uid, uid, len) == 0) return static_cast<int8_t>(i);
    }
    return -1;
}

bool save()
{
    File f = SPIFFS.open(kTmp, FILE_WRITE);
    if (!f) {
        lastError = "write failed";
        return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        char hex[24];
        uidHex(cards[i], hex, sizeof(hex));
        f.printf("%u;%s;%04X;%02X;%u;%s\n", static_cast<unsigned>(cards[i].len), hex,
                 static_cast<unsigned>(cards[i].atqa), static_cast<unsigned>(cards[i].sak),
                 static_cast<unsigned>(cards[i].hits), cards[i].name);
    }
    f.close();

    if (SPIFFS.exists(kPath)) {
        if (SPIFFS.exists(kBak)) SPIFFS.remove(kBak);
        SPIFFS.rename(kPath, kBak);
    }
    if (!SPIFFS.rename(kTmp, kPath)) {
        lastError = "rename failed";
        return false;
    }
    if (SPIFFS.exists(kBak)) SPIFFS.remove(kBak);
    lastError = nullptr;
    return true;
}

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
    if (cards[count].name[0] == 0) {
        defaultName(cards[count], cards[count].name, sizeof(cards[count].name));
    }
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
    sanitize(cards[i].name);
    save();
}

// Only a 4 or 7 byte UID can go back on the air; load_config() rejects 10.
bool replayable(const Card& c)
{
    return c.len == 4 || c.len == 7;
}

void uidToString(const Card& c, char* out, size_t n)
{
    uidHex(c, out, n);
}

// ---------------------------------------------------------------------------
// SD backup / restore
// ---------------------------------------------------------------------------
bool sdReady()
{
    return sdmount::ensure();
}

int8_t sdBackupCount()
{
    if (!sdReady() || !SD.exists(kSdPath)) return -1;
    File f = SD.open(kSdPath, FILE_READ);
    if (!f) return -1;
    int8_t n = 0;
    char line[160];
    while (f.available() > 0) {
        size_t k = 0;
        while (f.available() > 0 && k + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[k++] = static_cast<char>(ch);
        }
        line[k] = '\0';
        if (k == 0) continue;
        Card probe;
        if (parseLine(line, probe)) ++n;
    }
    f.close();
    return n;
}

bool backupToSd()
{
    if (!sdReady()) {
        lastError = "no TF card";
        return false;
    }
    if (!SD.exists(kSdDir)) SD.mkdir(kSdDir);
    File f = SD.open(kSdPath, FILE_WRITE);
    if (!f) {
        lastError = "TF write failed";
        return false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        char hex[24];
        uidHex(cards[i], hex, sizeof(hex));
        f.printf("%u;%s;%04X;%02X;%u;%s\n", static_cast<unsigned>(cards[i].len), hex,
                 static_cast<unsigned>(cards[i].atqa), static_cast<unsigned>(cards[i].sak),
                 static_cast<unsigned>(cards[i].hits), cards[i].name);
    }
    f.close();
    lastError = nullptr;
    Serial.printf("[nfc] backup %u card(s)\n", static_cast<unsigned>(count));
    return true;
}

bool restoreFromSd()
{
    if (!sdReady() || !SD.exists(kSdPath)) {
        lastError = "no TF backup";
        return false;
    }
    File f = SD.open(kSdPath, FILE_READ);
    if (!f) {
        lastError = "TF read failed";
        return false;
    }
    Card keep[kMaxCards];
    const uint8_t keepCount = count;
    memcpy(keep, cards, sizeof(cards));

    count = 0;
    char line[160];
    while (f.available() > 0 && count < kMaxCards) {
        size_t k = 0;
        while (f.available() > 0 && k + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[k++] = static_cast<char>(ch);
        }
        line[k] = '\0';
        if (k == 0) continue;
        Card c;
        if (parseLine(line, c)) cards[count++] = c;
    }
    f.close();

    if (count == 0) {
        memcpy(cards, keep, sizeof(cards));
        count     = keepCount;
        lastError = "TF backup empty";
        return false;
    }
    save();
    lastError = nullptr;
    Serial.printf("[nfc] restored %u card(s)\n", static_cast<unsigned>(count));
    return true;
}

// ---------------------------------------------------------------------------
// Deferred SD actions
//
// The ST25R3916 runs this bus in SPI mode 1 at 10 MHz; an SD card wants mode 0.
// Mixing them in one session latched the card hard enough that it stayed dead for
// the rest of the power cycle, even for the RF feature.
//
// So an SD action is never performed while the NFC chip is up: the menu records
// what to do in NVS and reboots, and setup() performs it before the NFC unit is
// initialised, while the bus is still untouched.
// ---------------------------------------------------------------------------
namespace {
constexpr char kActionNs[] = "nfcsd";
}

SdAction pendingAction()
{
    Preferences p;
    if (!p.begin(kActionNs, true)) return SdAction::kNone;
    const uint8_t v = p.getUChar("act", 0);
    p.end();
    return v <= 2 ? static_cast<SdAction>(v) : SdAction::kNone;
}

void clearAction()
{
    Preferences p;
    if (!p.begin(kActionNs, false)) return;
    p.remove("act");
    p.end();
}

void requestAction(SdAction a)
{
    Preferences p;
    if (!p.begin(kActionNs, false)) return;
    p.putUChar("act", static_cast<uint8_t>(a));
    p.end();
}

bool runPendingAction(char* msg, size_t msgLen)
{
    const SdAction a = pendingAction();
    if (a == SdAction::kNone) return false;
    clearAction();

    if (!sdmount::ensure(true)) {
        snprintf(msg, msgLen, "no TF card");
        sdmount::radioPowerOn();
        return true;
    }
    if (a == SdAction::kBackup) {
        if (backupToSd()) {
            snprintf(msg, msgLen, "backed up %u card(s)", static_cast<unsigned>(count));
        } else {
            snprintf(msg, msgLen, "%s", lastError ? lastError : "backup failed");
        }
    } else {
        if (restoreFromSd()) {
            snprintf(msg, msgLen, "restored %u card(s)", static_cast<unsigned>(count));
        } else {
            snprintf(msg, msgLen, "%s", lastError ? lastError : "restore failed");
        }
    }
    SD.end();
    return true;
}

int8_t load()
{
    count = 0;
    if (!SPIFFS.begin(false) && !SPIFFS.begin(true)) {
        lastError = "SPIFFS not mounted";
        return 0;
    }
    if (!SPIFFS.exists(kDir)) SPIFFS.mkdir(kDir);

    const char* path = nullptr;
    if (SPIFFS.exists(kPath)) {
        path = kPath;
    } else if (SPIFFS.exists(kBak)) {
        path      = kBak;
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
        if (parseLine(line, c)) cards[count++] = c;
    }
    f.close();
    return static_cast<int8_t>(count);
}

}  // namespace nfccards