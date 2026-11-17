#include "ota_service.h"

#include <ArduinoOTA.h>
#include <WiFi.h>

#include "wifi_store.h"

namespace otaservice {
namespace {

constexpr char kHostname[] = "cardputer";
constexpr char kPassword[] = "cardputer-ota";
bool started = false;
bool wifiStarted = false;

void startWifi()
{
    if (wifiStarted || WiFi.status() == WL_CONNECTED) return;
    wifistore::begin();
    const auto* n = wifistore::last();
    if (n == nullptr) return;
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(n->ssid, n->pass);
    wifiStarted = true;
}

void startOta()
{
    if (started || WiFi.status() != WL_CONNECTED) return;
    ArduinoOTA.setHostname(kHostname);
    ArduinoOTA.setPassword(kPassword);
    ArduinoOTA.setPort(3232);
    ArduinoOTA.onStart([]() { Serial.println("[ota] upload start"); });
    ArduinoOTA.onEnd([]() { Serial.println("[ota] upload done"); });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[ota] error %d\n", static_cast<int>(e));
    });
    ArduinoOTA.begin();
    started = true;
    Serial.printf("[ota] ready http://%s.local:3232 (ip=%s)\n", kHostname,
                  WiFi.localIP().toString().c_str());
}

}  // namespace

void begin()
{
    startWifi();
}

void tick()
{
    startWifi();
    if (WiFi.status() != WL_CONNECTED) return;
    startOta();
    if (started) ArduinoOTA.handle();
}

}  // namespace otaservice
