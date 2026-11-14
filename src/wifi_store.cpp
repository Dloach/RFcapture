#include "wifi_store.h"

#include <Preferences.h>

#include <cstring>

namespace wifistore {
namespace {

constexpr char kNs[] = "wifi";

Network gNets[kMaxNetworks];
int gCount  = 0;
int gLast   = -1;
bool loaded = false;

void copyField(char* dst, size_t cap, const String& src)
{
    if (cap == 0) return;
    const size_t n = src.length();
    const size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src.c_str(), c);
    dst[c] = 0;
}

void loadFromNvs()
{
    gCount = 0;
    gLast  = -1;
    Preferences p;
    if (!p.begin(kNs, true)) return;

    int n = p.getUChar("count", 0);
    if (n < 0) n = 0;
    if (n > kMaxNetworks) n = kMaxNetworks;
    for (int i = 0; i < n; ++i) {
        char key[12];
        snprintf(key, sizeof(key), "ssid%d", i);
        copyField(gNets[i].ssid, sizeof(gNets[i].ssid), p.getString(key, ""));
        snprintf(key, sizeof(key), "pass%d", i);
        copyField(gNets[i].pass, sizeof(gNets[i].pass), p.getString(key, ""));
        if (gNets[i].ssid[0] == 0) {
            // A torn/partial write leaves an empty slot; compact it away.
            for (int j = i; j < n - 1; ++j) gNets[j] = gNets[j + 1];
            --n;
            --i;
        }
    }
    gCount = n;
    int last = p.getUChar("last", 0);
    if (last < 0) last = 0;
    if (last >= gCount) last = gCount > 0 ? 0 : -1;
    gLast = last;
    p.end();
}

void writeAll()
{
    Preferences p;
    if (!p.begin(kNs, false)) return;
    p.putUChar("count", static_cast<uint8_t>(gCount));
    p.putUChar("last", static_cast<uint8_t>(gLast < 0 ? 0 : gLast));
    for (int i = 0; i < kMaxNetworks; ++i) {
        char key[12];
        snprintf(key, sizeof(key), "ssid%d", i);
        p.putString(key, i < gCount ? gNets[i].ssid : "");
        snprintf(key, sizeof(key), "pass%d", i);
        p.putString(key, i < gCount ? gNets[i].pass : "");
    }
    p.end();
}

}  // namespace

void begin()
{
    if (loaded) return;
    loadFromNvs();
    loaded = true;
}

int count()
{
    return gCount;
}

const Network& at(int index)
{
    static Network empty;
    if (index < 0 || index >= gCount) return empty;
    return gNets[index];
}

int lastIndex()
{
    if (gCount == 0) return -1;
    if (gLast < 0 || gLast >= gCount) return 0;
    return gLast;
}

const Network* last()
{
    const int idx = lastIndex();
    return idx < 0 ? nullptr : &gNets[idx];
}

int find(const char* ssid)
{
    if (ssid == nullptr) return -1;
    for (int i = 0; i < gCount; ++i) {
        if (strcmp(gNets[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

bool save(const char* ssid, const char* pass)
{
    if (ssid == nullptr || ssid[0] == 0) return false;
    if (strlen(ssid) > kSsidMax) return false;
    if (pass == nullptr) pass = "";
    if (strlen(pass) > kPassMax) return false;

    int idx = find(ssid);
    if (idx < 0) {
        if (gCount < kMaxNetworks) {
            idx = gCount++;
        } else {
            // Drop the oldest entry; the array is in save/use order.
            for (int i = 0; i < kMaxNetworks - 1; ++i) gNets[i] = gNets[i + 1];
            idx = kMaxNetworks - 1;
        }
    }

    strncpy(gNets[idx].ssid, ssid, kSsidMax);
    gNets[idx].ssid[kSsidMax] = 0;
    strncpy(gNets[idx].pass, pass, kPassMax);
    gNets[idx].pass[kPassMax] = 0;
    gLast = idx;
    writeAll();
    return true;
}

bool removeAt(int index)
{
    if (index < 0 || index >= gCount) return false;
    for (int i = index; i < gCount - 1; ++i) gNets[i] = gNets[i + 1];
    --gCount;
    gNets[gCount].ssid[0] = 0;
    gNets[gCount].pass[0] = 0;
    if (gLast == index) {
        gLast = gCount > 0 ? 0 : -1;
    } else if (gLast > index) {
        --gLast;
    }
    writeAll();
    return true;
}

bool markLast(int index)
{
    if (index < 0 || index >= gCount) return false;
    gLast = index;
    writeAll();
    return true;
}

}  // namespace wifistore
