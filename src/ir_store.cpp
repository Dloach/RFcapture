#include "ir_store.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

#include "ir_protocol.h"

namespace irstore {
namespace {

constexpr char kNamespace[] = "ir";
constexpr char kKey[]       = "codes";
constexpr uint32_t kMagic   = 0x49524331u;  // "IRC1"
constexpr uint8_t kVersion  = 1;

struct Blob {
    uint32_t magic;
    uint8_t version;
    uint8_t count;
    uint16_t reserved;
    Code codes[kMaxCodes];
    uint32_t checksum;
};

Blob g_blob;
bool g_loaded = false;

uint32_t checksumOf(const Blob& b)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&b);
    const size_t len = offsetof(Blob, checksum);
    uint32_t h       = 2166136261u;  // FNV-1a
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void blank()
{
    std::memset(&g_blob, 0, sizeof(g_blob));
    g_blob.magic   = kMagic;
    g_blob.version = kVersion;
    g_blob.count   = 0;
}

void load()
{
    if (g_loaded) return;
    g_loaded = true;
    blank();

    Preferences p;
    if (!p.begin(kNamespace, true)) return;
    const size_t len = p.getBytesLength(kKey);
    if (len == sizeof(Blob)) {
        Blob tmp;
        if (p.getBytes(kKey, &tmp, sizeof(tmp)) == sizeof(tmp) && tmp.magic == kMagic &&
            tmp.version == kVersion && tmp.count <= kMaxCodes && tmp.checksum == checksumOf(tmp)) {
            g_blob = tmp;
            Serial.printf("[ir] library loaded: %u codes\n", static_cast<unsigned>(g_blob.count));
        } else {
            Serial.printf("[ir] library rejected (bad header or checksum)\n");
        }
    } else if (len != 0) {
        Serial.printf("[ir] library ignored: %u bytes\n", static_cast<unsigned>(len));
    }
    p.end();
}

void store()
{
    g_blob.checksum = checksumOf(g_blob);
    Preferences p;
    if (!p.begin(kNamespace, false)) return;
    p.putBytes(kKey, &g_blob, sizeof(g_blob));
    p.end();
}

void trimName(char* dst, const char* src, size_t cap)
{
    size_t i = 0;
    if (src != nullptr) {
        for (; src[i] != 0 && i + 1 < cap; ++i) {
            const char c = src[i];
            // Flipper names allow printable ASCII only; strip everything else
            // so a UTF-8 file name cannot corrupt the list rendering.
            dst[i] = (c >= 0x20 && c < 0x7F) ? c : '_';
        }
    }
    dst[i] = 0;
}

}  // namespace

void begin() { load(); }

int count()
{
    load();
    return g_blob.count;
}

const Code& at(int i)
{
    load();
    static Code dummy{};
    if (i < 0 || i >= g_blob.count) return dummy;
    return g_blob.codes[i];
}

int add(const char* name, const ir::Signal& signal)
{
    load();
    if (g_blob.count >= kMaxCodes) return -1;

    Code& c = g_blob.codes[g_blob.count];
    std::memset(&c, 0, sizeof(c));
    trimName(c.name, name, sizeof(c.name));
    if (c.name[0] == 0) {
        std::snprintf(c.name, sizeof(c.name), "%s", ir::protocolName(signal.proto));
    }
    c.proto   = static_cast<uint8_t>(signal.proto);
    c.toggle  = signal.toggle;
    c.address = signal.address;
    c.command = signal.command;

    ++g_blob.count;
    store();
    return g_blob.count - 1;
}

bool remove(int i)
{
    load();
    if (i < 0 || i >= g_blob.count) return false;
    for (int k = i; k + 1 < g_blob.count; ++k) g_blob.codes[k] = g_blob.codes[k + 1];
    --g_blob.count;
    std::memset(&g_blob.codes[g_blob.count], 0, sizeof(Code));
    store();
    return true;
}

bool rename(int i, const char* name)
{
    load();
    if (i < 0 || i >= g_blob.count || name == nullptr) return false;
    trimName(g_blob.codes[i].name, name, sizeof(g_blob.codes[i].name));
    store();
    return true;
}

ir::Signal signalOf(const Code& c)
{
    ir::Signal s;
    s.proto   = static_cast<ir::Protocol>(c.proto);
    s.address = c.address;
    s.command = c.command;
    s.toggle  = c.toggle;
    return s;
}

int find(const ir::Signal& signal)
{
    load();
    for (int i = 0; i < g_blob.count; ++i) {
        const Code& c = g_blob.codes[i];
        if (c.proto == static_cast<uint8_t>(signal.proto) && c.address == signal.address &&
            c.command == signal.command) {
            return i;
        }
    }
    return -1;
}

void clear()
{
    load();
    blank();
    store();
}

}  // namespace irstore
