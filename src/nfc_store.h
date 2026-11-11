#ifndef NFC_STORE_H
#define NFC_STORE_H

// Saved NFC cards: the user's own named collection, held on SPIFFS with the SD
// card as an explicit backup target.
//
// This deliberately has no dependency on the M5 NFC library -- it is records and
// files, nothing else.  Reader mode and emulation mode are mutually exclusive
// configurations of the same chip (docs/DEV-STATE.md section 12.4.1), and it is
// not yet settled whether one firmware can hold both; keeping the store free of
// any NFC type means it compiles and behaves identically whichever way that
// goes, and both halves of the menu operate on the same records.
//
// Include from exactly one translation unit per build.  Nothing here is inline
// or static, so a second includer fails loudly at link time rather than quietly
// giving each file its own copy of the collection.

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>

namespace nfcstore {

constexpr uint8_t kMaxCards = 24;
constexpr size_t kNameLen   = 17;  // 16 characters plus NUL
constexpr size_t kTypeLen   = 24;
constexpr uint8_t kUidMax   = 10;

struct Card {
    uint8_t uid[kUidMax];
    uint8_t len;      // 4, 7 or 10 as captured; only 4 and 7 are replayable
    uint16_t atqa;
    uint8_t sak;
    uint16_t hits;    // how many times this card has been read
    char type[kTypeLen];
    char name[kNameLen];
};

Card cards[kMaxCards];
uint8_t count = 0;

fs::FS* primary          = nullptr;  // SPIFFS, the collection's home
const char* primaryName  = "NONE";
bool sdReady             = false;
const char* lastError    = nullptr;

constexpr int kSdCsPin    = 12;  // same pin as src/nfc_spike.cpp:130
constexpr uint32_t kSdHz  = 10000000;
constexpr char kDir[]     = "/nfc";
constexpr char kPath[]    = "/nfc/cards.txt";
constexpr char kTmpPath[] = "/nfc/cards.tmp";
constexpr char kBakPath[] = "/nfc/cards.bak";  // SPIFFS: last-good copy
constexpr char kSdBak[]   = "/nfc/cards.bak";  // SD: the explicit backup slot
constexpr char kOldLog[]  = "/nfc/capture.log";

// The collection is records the operator walked around collecting, so it is not
// regenerable -- the same reasoning DEV-STATE section 5 applies to the NVS RF
// records.  A plain truncate-and-rewrite loses everything if power drops
// mid-write, so save() writes a temp file, demotes the current one to .bak,
// promotes the temp, then drops the .bak.  load() falls back to .bak when the
// main file is missing, which covers every point in that sequence.

// `;` separates fields and the name is last, so a name may contain spaces.  It
// may not contain the separator or a newline; sanitizeName() strips those, and
// the name editor never emits them in the first place.
void sanitizeName(char* s)
{
    char* w = s;
    for (char* r = s; *r; ++r) {
        if (*r == ';' || *r == '\n' || *r == '\r') continue;
        *w++ = *r;
    }
    *w = '\0';
    while (w > s && w[-1] == ' ') *--w = '\0';
    size_t i = 0;
    while (s[i] == ' ') ++i;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

void uidHex(const Card& c, char* out, size_t n)
{
    size_t o = 0;
    for (uint8_t i = 0; i < c.len && o + 2 < n; ++i) {
        static const char hex[] = "0123456789ABCDEF";
        out[o++] = hex[c.uid[i] >> 4];
        out[o++] = hex[c.uid[i] & 0x0F];
    }
    out[o] = '\0';
}

// Only a 4 or 7 byte UID can go back on the air: load_config() rejects 10
// outright (emulation_layer_a_ST25R3916.cpp:141).  A 10-byte card is still worth
// keeping -- it identifies the object -- so the record stays and the UI says why
// it cannot be replayed rather than hiding it.
bool replayable(const Card& c) { return c.len == 4 || c.len == 7; }

// Last four hex digits of the UID.  Short enough to fit the list column, unique
// enough across a handful of cards to be a usable placeholder before renaming.
void defaultName(const Card& c, char* out, size_t n)
{
    char hex[24];
    uidHex(c, hex, sizeof(hex));
    const size_t l = strlen(hex);
    const char* tail = l > 4 ? hex + l - 4 : hex;
    snprintf(out, n, "%s", tail);
}

int8_t find(const uint8_t len, const uint8_t* uid)
{
    for (uint8_t i = 0; i < count; ++i) {
        if (cards[i].len == len && memcmp(cards[i].uid, uid, len) == 0) return static_cast<int8_t>(i);
    }
    return -1;
}

// One record per physical object.  Re-reading a card bumps hits and refreshes
// atqa/sak/type but keeps the name -- the name is the operator's, not the
// card's, and a re-read that silently renamed things would be worse than one
// that did nothing.
int8_t add(const Card& c)
{
    const int8_t existing = find(c.len, c.uid);
    if (existing >= 0) {
        Card& e = cards[existing];
        if (e.hits < 0xFFFF) ++e.hits;
        e.atqa = c.atqa;
        e.sak  = c.sak;
        snprintf(e.type, sizeof(e.type), "%s", c.type);
        return existing;
    }
    if (count >= kMaxCards) {
        lastError = "COLLECTION FULL (24)";
        return -1;
    }
    cards[count] = c;
    if (cards[count].hits == 0) cards[count].hits = 1;
    return static_cast<int8_t>(count++);
}

bool remove(int8_t i)
{
    if (i < 0 || i >= count) return false;
    for (uint8_t k = static_cast<uint8_t>(i); k + 1 < count; ++k) cards[k] = cards[k + 1];
    --count;
    memset(&cards[count], 0, sizeof(Card));
    return true;
}

bool rename(int8_t i, const char* newName)
{
    if (i < 0 || i >= count) return false;
    snprintf(cards[i].name, sizeof(cards[i].name), "%s", newName);
    sanitizeName(cards[i].name);
    return true;
}

// ---------------------------------------------------------------------------
// File format
//
// One record per line, `;` separated, fixed order, name last:
//   1;3CDEAD01;4;0004;08;2;MIFARE Classic 1K;Front door
//   ^version   ^len ^atqa ^sak ^hits ^type         ^name
// Text fields may contain spaces.  A line that does not yield eight fields, or
// whose version is not 1, is skipped rather than guessed at -- a half-written
// tail from an interrupted save must not turn into a phantom card.
// ---------------------------------------------------------------------------
constexpr char kVersion = '1';

bool ch_isBlank(const char* s)
{
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\r') return false;
        ++s;
    }
    return true;
}

// Pure: fills `out` and reports whether the line was usable.  Kept separate from
// parseLine() so the restore confirm screen can count what a backup holds
// without touching the live collection.
bool parseLineInto(const char* line, Card& out)
{
    char buf[160];
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
    if (f[0][0] != kVersion || f[0][1] != '\0') return false;

    memset(&out, 0, sizeof(out));
    out.len = static_cast<uint8_t>(atoi(f[2]));
    if (out.len == 0 || out.len > kUidMax) return false;

    // The UID is hex; a short or non-hex token means the line is damaged, and a
    // damaged UID is worse than a missing card because it replays the wrong
    // identity at a door.
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

    out.atqa      = static_cast<uint16_t>(strtoul(f[3], nullptr, 16) & 0xFFFF);
    out.sak       = static_cast<uint8_t>(strtoul(f[4], nullptr, 16) & 0xFF);
    const long ht = atol(f[5]);
    out.hits      = static_cast<uint16_t>(ht > 0 && ht < 0xFFFF ? ht : 1);
    snprintf(out.type, sizeof(out.type), "%s", f[6]);
    // The name is the last field, so it keeps whatever trailing text the line
    // had -- including any separator that survived, since the splitter stops at
    // eight fields.  sanitizeName() strips that and the newline.
    snprintf(out.name, sizeof(out.name), "%s", f[7]);
    sanitizeName(out.name);
    if (out.name[0] == '\0') defaultName(out, out.name, sizeof(out.name));
    return true;
}

bool parseLine(const char* line)
{
    Card c;
    if (!parseLineInto(line, c)) return false;
    // add() would fold a duplicate onto an existing record; during load the file
    // is the authority and already deduplicated, so append directly.
    if (count >= kMaxCards) {
        lastError = "FILE HAS MORE THAN 24";
        return false;
    }
    cards[count++] = c;
    return true;
}

uint8_t loadFrom(fs::FS* fs, const char* path)
{
    File f = fs->open(path, FILE_READ);
    if (!f) return 0;
    const uint8_t before = count;
    char line[160];
    while (f.available() > 0 && count < kMaxCards) {
        size_t n = 0;
        while (f.available() > 0 && n + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[n++] = static_cast<char>(ch);
        }
        line[n] = '\0';
        if (ch_isBlank(line)) continue;
        parseLine(line);
    }
    f.close();
    return static_cast<uint8_t>(count - before);
}

bool saveTo(fs::FS* fs, const char* path)
{
    File f = fs->open(path, FILE_WRITE);  // FILE_WRITE truncates on ESP32
    if (!f) return false;
    char line[192];
    bool ok = true;
    for (uint8_t i = 0; i < count && ok; ++i) {
        char hex[24];
        uidHex(cards[i], hex, sizeof(hex));
        const int n = snprintf(line, sizeof(line), "%c;%s;%u;%04X;%02X;%u;%s;%s\n", kVersion, hex,
                               static_cast<unsigned>(cards[i].len),
                               static_cast<unsigned>(cards[i].atqa),
                               static_cast<unsigned>(cards[i].sak),
                               static_cast<unsigned>(cards[i].hits), cards[i].type, cards[i].name);
        if (n <= 0 || n >= static_cast<int>(sizeof(line))) {
            ok = false;
            break;
        }
        ok = f.write(reinterpret_cast<const uint8_t*>(line), n) == static_cast<size_t>(n);
    }
    f.close();
    return ok;
}

bool save()
{
    if (primary == nullptr) {
        lastError = "NO STORAGE MOUNTED";
        return false;
    }
    if (!saveTo(primary, kTmpPath)) {
        lastError = "WRITE FAILED";
        return false;
    }
    // Only demote the current file once the replacement is safely on disk.
    if (primary->exists(kPath)) {
        if (primary->exists(kBakPath)) primary->remove(kBakPath);
        primary->rename(kPath, kBakPath);
    }
    if (!primary->rename(kTmpPath, kPath)) {
        // Put the old one back rather than leave the collection unreachable.
        if (primary->exists(kBakPath) && !primary->exists(kPath)) primary->rename(kBakPath, kPath);
        primary->remove(kTmpPath);
        lastError = "RENAME FAILED";
        return false;
    }
    if (primary->exists(kBakPath)) primary->remove(kBakPath);
    lastError = nullptr;
    return true;
}

// ---------------------------------------------------------------------------
// One-time migration from the capture spike's log
//
// The reader spike already wrote eight real detections to /nfc/capture.log.
// Without this the operator would have to walk around and re-tap every card to
// get back to where they were.  It runs only when the collection file is absent,
// so it can never overwrite a name the user has set.
// ---------------------------------------------------------------------------

// Value for `key`, where the match must start a whitespace-delimited field.
// Without the leading-space test a search for "uid=" also matches inside
// "uidLen=", and "id=" matches inside "uid=" -- same trap as src/nfc_emu.cpp:302.
const char* findVal(const char* line, const char* key)
{
    const size_t klen = strlen(key);
    for (const char* p = line; *p; ++p) {
        if (strncmp(p, key, klen) == 0 && (p == line || p[-1] == ' ')) return p + klen;
    }
    return nullptr;
}

uint8_t hexBytes(const char* s, uint8_t* out, const uint8_t max)
{
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    uint8_t n = 0;
    while (s[0] && s[1] && n < max) {
        const int hi = hv(s[0]), lo = hv(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = static_cast<uint8_t>((hi << 4) | lo);
        s += 2;
    }
    return n;
}

// Accepts both log generations: the A-only spike wrote `uid=`, the
// multi-protocol one writes `proto=A id=`.  Only NFC-A lines are taken, because
// EmulationLayerA is the only emulation layer that exists for this chip.
void migrateLine(const char* line)
{
    if (line[0] != 'H' || line[1] != ' ') return;
    const char* proto = findVal(line, "proto=");
    if (proto && proto[0] != 'A') return;

    const char* idv = findVal(line, "id=");
    if (!idv) idv = findVal(line, "uid=");
    if (!idv) return;

    Card c;
    memset(&c, 0, sizeof(c));
    char tok[24];
    size_t i = 0;
    while (idv[i] && idv[i] != ' ' && i + 1 < sizeof(tok)) tok[i] = idv[i], ++i;
    tok[i] = '\0';
    c.len  = hexBytes(tok, c.uid, sizeof(c.uid));
    if (c.len != 4 && c.len != 7 && c.len != 10) return;

    const char* av = findVal(line, "atqa=");
    const char* sv = findVal(line, "sak=");
    if (av) c.atqa = static_cast<uint16_t>(strtoul(av, nullptr, 16) & 0xFFFF);
    if (sv) c.sak  = static_cast<uint8_t>(strtoul(sv, nullptr, 16) & 0xFF);

    // uidLen is the authority when present: a truncated hex token would
    // otherwise silently shorten a UID.
    const char* lv = findVal(line, "uidLen=");
    if (lv) {
        const int declared = atoi(lv);
        if (declared == 4 || declared == 7 || declared == 10) c.len = static_cast<uint8_t>(declared);
    }

    // type= runs to " atqa=" because type names contain spaces.
    const char* tv = findVal(line, "type=");
    const char* ae = strstr(line, " atqa=");
    if (tv && ae && ae > tv) {
        size_t n = static_cast<size_t>(ae - tv);
        if (n >= sizeof(c.type)) n = sizeof(c.type) - 1;
        memcpy(c.type, tv, n);
        c.type[n] = '\0';
    } else {
        snprintf(c.type, sizeof(c.type), "NFC-A");
    }

    c.hits = 1;
    defaultName(c, c.name, sizeof(c.name));
    // add() rather than a direct append, so two log lines for the same object
    // fold into one record the way they did in the spike.
    add(c);
}

uint8_t migrateFromLog()
{
    File f;
    if (primary && primary->exists(kOldLog)) {
        f = primary->open(kOldLog, FILE_READ);
    } else if (sdReady && SD.exists(kOldLog)) {
        f = SD.open(kOldLog, FILE_READ);
    }
    if (!f) return 0;
    const uint8_t before = count;
    char line[224];
    while (f.available() > 0 && count < kMaxCards) {
        size_t n = 0;
        while (f.available() > 0 && n + 1 < sizeof(line)) {
            const int ch = f.read();
            if (ch < 0 || ch == '\n') break;
            line[n++] = static_cast<char>(ch);
        }
        line[n] = '\0';
        migrateLine(line);
    }
    f.close();
    return static_cast<uint8_t>(count - before);
}

// ---------------------------------------------------------------------------
// Mount and load
// ---------------------------------------------------------------------------
// withSd=false skips the card entirely.  Emulation mode arms the NFC chip and
// then must not touch the shared SPI bus again; mounting SD there would put a
// second device (and SD.begin()/SD.end()) between arming and the first reader
// frame.
bool mount(const bool withSd = true)
{
    // SPIFFS is the collection's home: it is always present, so the records do
    // not depend on an optional SD card being inserted.  format=false first, so
    // an existing partition is appended to rather than erased.
    if (SPIFFS.begin(false) || SPIFFS.begin(true)) {
        primary     = &SPIFFS;
        primaryName = "SPIFFS";
    }
    // SD is mounted too, but only as the backup target.  cardType() short-
    // circuits a second begin(), mirroring src/nfc_spike.cpp:190.
    if (withSd) {
        sdReady = (SD.cardType() != CARD_NONE) || SD.begin(kSdCsPin, SPI, kSdHz);
        if (!sdReady) SD.end();
    }
    if (primary && !primary->exists(kDir)) primary->mkdir(kDir);
    if (sdReady && !SD.exists(kDir)) SD.mkdir(kDir);
    return primary != nullptr;
}

// Returns the number of records available afterwards.  Order matters: main
// file, then the last-good .bak, then a one-time migration from the capture
// log.  A migration that produced records is saved immediately, so the next
// boot takes the fast path and the names become the user's to edit.
uint8_t load()
{
    count = 0;
    if (primary == nullptr) {
        lastError = "NO STORAGE MOUNTED";
        return 0;
    }
    if (primary->exists(kPath)) {
        loadFrom(primary, kPath);
    } else if (primary->exists(kBakPath)) {
        lastError = "RECOVERED FROM .bak";
        loadFrom(primary, kBakPath);
        save();
    } else {
        const uint8_t migrated = migrateFromLog();
        if (migrated) {
            lastError = "IMPORTED FROM CAPTURE LOG";
            save();
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// SD backup / restore
// ---------------------------------------------------------------------------

// Copies the collection to the single SD backup slot.  The slot is overwritten
// on purpose: one obvious "the backup" is easier to reason about at a door than
// a pile of dated files, and the restore screen shows how many cards the backup
// holds so an empty or stale one is visible before it is allowed to overwrite
// anything.
bool backupToSd()
{
    if (!sdReady) {
        lastError = "NO SD CARD";
        return false;
    }
    if (!SD.exists(kDir)) SD.mkdir(kDir);
    if (!saveTo(&SD, kSdBak)) {
        lastError = "SD WRITE FAILED";
        return false;
    }
    lastError = nullptr;
    return true;
}

// How many records the SD backup holds, or -1 when there is none.  Read without
// disturbing the in-RAM collection, so the confirm screen can show it.
int8_t sdBackupCount()
{
    if (!sdReady || !SD.exists(kSdBak)) return -1;
    File f = SD.open(kSdBak, FILE_READ);
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
        if (ch_isBlank(line)) continue;
        // Count only lines that would actually parse, so a truncated tail from
        // an interrupted backup does not inflate the number shown.
        Card probe;
        memset(&probe, 0, sizeof(probe));
        const uint8_t savedCount = count;
        Card saved[kMaxCards];
        memcpy(saved, cards, sizeof(cards));
        const bool ok = parseLine(line);
        memcpy(cards, saved, sizeof(cards));
        count = savedCount;
        if (ok) ++n;
    }
    f.close();
    return n;
}

// Overwrites the in-RAM collection from the SD backup.  The current collection
// is saved to .bak first, because restore is destructive and the records are not
// regenerable -- a user who restores by mistake should still be able to get back
// to what they had.
bool restoreFromSd()
{
    if (!sdReady || !SD.exists(kSdBak)) {
        lastError = "NO SD BACKUP";
        return false;
    }
    Card keep[kMaxCards];
    const uint8_t keepCount = count;
    memcpy(keep, cards, sizeof(cards));

    count = 0;
    const uint8_t got = loadFrom(&SD, kSdBak);
    if (got == 0) {
        // Nothing usable in the backup: put the collection back exactly as it
        // was rather than leave the user with an empty list.
        memcpy(cards, keep, sizeof(cards));
        count     = keepCount;
        lastError = "SD BACKUP UNREADABLE";
        return false;
    }
    if (primary && primary->exists(kPath)) {
        if (primary->exists(kBakPath)) primary->remove(kBakPath);
        primary->rename(kPath, kBakPath);
    }
    if (!save()) {
        memcpy(cards, keep, sizeof(cards));
        count     = keepCount;
        return false;
    }
    if (primary && primary->exists(kBakPath)) primary->remove(kBakPath);
    lastError = nullptr;
    return true;
}

}  // namespace nfcstore

#endif  // NFC_STORE_H
