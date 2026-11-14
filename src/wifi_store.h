// Saved WiFi networks.  Kept out of the WiFi driver's own NVS so the firmware
// has one explicit, inspectable list (and so WiFi.persistent(false) cannot
// create a second copy).
#pragma once

#include <Arduino.h>

namespace wifistore {

constexpr int kMaxNetworks = 5;
constexpr int kSsidMax     = 32;
constexpr int kPassMax     = 63;

struct Network {
    char ssid[kSsidMax + 1] = {0};
    char pass[kPassMax + 1] = {0};
};

void begin();

int count();
const Network& at(int index);
int lastIndex();
const Network* last();
int find(const char* ssid);

/// Add or update a network and make it the last-used one.  Returns false on an
/// empty SSID or an over-long password.
bool save(const char* ssid, const char* pass);

bool removeAt(int index);
bool markLast(int index);

}  // namespace wifistore
