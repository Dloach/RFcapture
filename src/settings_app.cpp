#include "settings_app.h"

#include <ArduinoOTA.h>
#include <M5Cardputer.h>
#include <WiFi.h>

#include <cstring>
#include <vector>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "ui_hint.h"
#include "wifi_store.h"

namespace settingsapp {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;

constexpr int kFooterY = 118;

auto& lcd = M5Cardputer.Display;

enum class Scr : uint8_t {
    kSettings,
    kWifi,
    kScan,
    kPassword,
    kConnecting,
    kSaved,
    kTest,
    kOta,
};
Scr scr = Scr::kSettings;
bool dirty = true;

int settingsSel = 0;
int wifiSel     = 0;
int scanSel     = 0;
int scanTop     = 0;
int savedSel    = 0;
int savedTop    = 0;

char statusMsg[48] = {0};
char testResult[32] = {0};

// OTA uses ArduinoOTA over the same station link.  The password must match the
// --auth flag in the cardputer-ota PlatformIO environment.
constexpr char kOtaHostname[] = "cardputer";
constexpr char kOtaPassword[] = "cardputer-ota";
bool otaStarted  = false;
volatile bool otaUploading = false;
volatile int otaPercent    = 0;
char otaStatus[40] = "IDLE";

enum : uint32_t {
    kUp     = 1u << 0,
    kDown   = 1u << 1,
    kEnter  = 1u << 2,
    kBack   = 1u << 3,
    kDelete = 1u << 4,
    kScan   = 1u << 5,
    kForget = 1u << 6,
};

struct ScanItem {
    char ssid[wifistore::kSsidMax + 1];
    int16_t rssi;
    uint8_t enc;
};
constexpr int kMaxScan = 24;
ScanItem scanItems[kMaxScan];
int scanCount      = 0;
bool scanning      = false;
bool scanReady     = false;
uint32_t scanStart = 0;

// Password entry.
char passBuf[wifistore::kPassMax + 1];
int passLen = 0;
char passSsid[wifistore::kSsidMax + 1];
bool passOpen = false;
std::vector<char> prevWord;
bool prevDel   = false;
bool prevEnter = false;

// Connection state.  The credentials are held here so the success path can
// save exactly what was attempted, not whatever the UI has moved on to.
char pendingSsid[wifistore::kSsidMax + 1];
char pendingPass[wifistore::kPassMax + 1];
bool pendingSave = false;
bool connecting  = false;
bool exitRequested = false;
bool autoConnecting = false;
uint32_t connectStart = 0;

// First D arms, second D within 2 s forgets.
bool forgetArmed = false;
uint32_t forgetAt = 0;

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
void header(const char* title)
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(title, 3, 2);

    const int level = M5.Power.getBatteryLevel();
    char bat[12];
    snprintf(bat, sizeof(bat), level < 0 ? "--" : "%d%%", level);
    lcd.drawRightString(bat, lcd.width() - 3, 2);
}

struct Hint {
    const char* key;
    const char* text;
};

void hints(const Hint* h, int n)
{
    lcd.drawFastHLine(0, kFooterY, lcd.width(), kDim);
    int x = 3;
    for (int i = 0; i < n; ++i) {
        lcd.setTextColor(kOk, kBg);
        if (h[i].key[0] == ';') {
            drawMoveTriangles(lcd, x, kFooterY + 4, kOk);
            x += kMoveTrianglesW;
        } else {
            lcd.drawString(h[i].key, x, kFooterY + 4);
            x += lcd.textWidth(h[i].key);
        }
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(h[i].text, x + 2, kFooterY + 4);
        x += lcd.textWidth(h[i].text) + 10;
    }
}

void drawSettings()
{
    lcd.fillScreen(kBg);
    header("SETTINGS");

    const char* items[] = {"WIFI SETTINGS", "OTA UPDATE", "BACKUP / RESTORE"};
    lcd.setTextSize(2);
    for (int i = 0; i < 3; ++i) {
        const int y = 20 + i * 26;
        if (i == settingsSel) lcd.fillRect(4, y - 4, lcd.width() - 8, 21, kBar);
        lcd.setTextColor(i == settingsSel ? kAccent : kText, i == settingsSel ? kBar : kBg);
        lcd.drawString(items[i], 12, y);
    }
    lcd.setTextSize(1);

    lcd.setTextColor(kDim, kBg);
    if (settingsSel == 0) {
        lcd.drawString("Scan, connect and save 2.4 GHz networks.", 6, 100);
    } else if (settingsSel == 1) {
        lcd.drawString("Wireless flash; keep this page open.", 6, 100);
    } else {
        lcd.drawString("TF backup / restore RF, NFC, bindings.", 6, 100);
    }

    static const Hint h[] = {{"ENT", "open"}, {"esc", "back"}};
    hints(h, 2);
}

const char* wifiStatusText()
{
    if (connecting) return "CONNECTING...";
    if (WiFi.status() == WL_CONNECTED) return "CONNECTED";
    if (statusMsg[0]) return statusMsg;
    return "NOT CONNECTED";
}

uint16_t wifiStatusColor()
{
    if (connecting) return kAccent;
    return WiFi.status() == WL_CONNECTED ? kOk : kDim;
}

void drawWifi()
{
    lcd.fillScreen(kBg);
    header("WIFI");

    lcd.setTextColor(wifiStatusColor(), kBg);
    lcd.drawString(wifiStatusText(), 6, 17);

    if (WiFi.status() == WL_CONNECTED) {
        String ssid = WiFi.SSID();
        String ip   = WiFi.localIP().toString();
        lcd.setTextColor(kText, kBg);
        lcd.drawString(ssid.c_str(), 6, 31);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(ip.c_str(), 6, 43);
    } else if (connecting && pendingSsid[0]) {
        lcd.setTextColor(kText, kBg);
        lcd.drawString(pendingSsid, 6, 31);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("Waiting for DHCP / association...", 6, 43);
    } else if (statusMsg[0]) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(statusMsg, 6, 31);
    }

    const char* items[] = {"SCAN NETWORKS", "SAVED NETWORKS", "TEST CONNECTION", "FORGET CURRENT"};
    lcd.setTextSize(1);
    for (int i = 0; i < 4; ++i) {
        const int y = 56 + i * 15;
        if (i == wifiSel) lcd.fillRect(2, y - 2, lcd.width() - 4, 13, kBar);
        lcd.setTextColor(i == wifiSel ? kAccent : kText, i == wifiSel ? kBar : kBg);
        lcd.drawString(items[i], 8, y);
    }

    static const Hint h[] = {{"; .", "move"}, {"ENT", "open"}, {"esc", "back"}};
    hints(h, 3);
}

void drawScan()
{
    lcd.fillScreen(kBg);
    header("WIFI SCAN");

    lcd.setTextColor(scanning ? kAccent : kDim, kBg);
    if (scanning) {
        lcd.drawRightString("SCANNING", lcd.width() - 3, 18);
    } else {
        char n[16];
        snprintf(n, sizeof(n), "%d FOUND", scanCount);
        lcd.drawRightString(n, lcd.width() - 3, 18);
    }

    constexpr int kRows = 5;
    for (int r = 0; r < kRows; ++r) {
        const int idx = scanTop + r;
        if (idx >= scanCount) break;
        const int y = 20 + r * 18;
        if (idx == scanSel) lcd.fillRect(0, y - 2, lcd.width(), 16, kBar);
        const uint16_t fg = idx == scanSel ? kAccent : kText;
        const uint16_t bg = idx == scanSel ? kBar : kBg;

        char line[44];
        snprintf(line, sizeof(line), "%-23.23s %4d %c", scanItems[idx].ssid, scanItems[idx].rssi,
                 scanItems[idx].enc == WIFI_AUTH_OPEN ? ' ' : '*');
        lcd.setTextColor(fg, bg);
        lcd.drawString(line, 4, y);
    }

    if (!scanning && scanCount == 0) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("No networks found. Press S to rescan.", 6, 58);
    }

    static const Hint h[] = {{"S", "rescan"}, {"ENT", "select"}, {"esc", "back"}};
    hints(h, 3);
}

void drawPassword()
{
    lcd.fillScreen(kBg);
    header("WIFI PASSWORD");

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("SSID", 6, 18);
    lcd.setTextColor(kText, kBg);
    lcd.drawString(passSsid, 6, 30);

    // Show a moving window of stars: 63 characters at 6 px would run off the
    // panel, and seeing the tail is more useful than a fixed truncation.
    char shown[40];
    int n = 0;
    const int maxShow = 26;
    if (passLen > maxShow) {
        shown[n++] = '.';
        shown[n++] = '.';
        shown[n++] = '.';
        for (int i = passLen - (maxShow - 3); i < passLen; ++i) shown[n++] = '*';
    } else {
        for (int i = 0; i < passLen; ++i) shown[n++] = '*';
    }
    shown[n++] = '_';
    shown[n]   = 0;

    lcd.setTextColor(kAccent, kBg);
    lcd.drawString(shown, 6, 50);

    char count[32];
    snprintf(count, sizeof(count), "LEN %d / %d", passLen, wifistore::kPassMax);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(count, 6, 66);
    lcd.drawString("WPA2/WPA3 passphrase, 8-63 chars.", 6, 80);

    static const Hint h[] = {{"ENT", "connect"}, {"BKSP", "delete"}, {"FN BKSP", "cancel"}};
    hints(h, 3);
}

void drawConnecting()
{
    lcd.fillScreen(kBg);
    header("WIFI");

    lcd.setTextColor(kAccent, kBg);
    lcd.drawString("CONNECTING", 6, 26);

    lcd.setTextColor(kText, kBg);
    lcd.drawString(pendingSsid, 6, 46);

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Trying to join the access point...", 6, 64);
    lcd.drawString("Timeout: 15 s", 6, 78);

    static const Hint h[] = {{"esc", "cancel"}};
    hints(h, 1);
}

void drawTest()
{
    lcd.fillScreen(kBg);
    header("WIFI TEST");

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("TCP test: example.com:80", 6, 20);

    if (WiFi.status() == WL_CONNECTED) {
        lcd.setTextColor(kText, kBg);
        lcd.drawString(WiFi.SSID().c_str(), 6, 38);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(WiFi.localIP().toString().c_str(), 6, 50);
    } else {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString("NOT CONNECTED", 6, 38);
    }

    lcd.setTextColor(testResult[0] ? kText : kDim, kBg);
    lcd.drawString(testResult[0] ? testResult : "PRESS ENT TO TEST", 6, 72);

    static const Hint h[] = {{"ENT", "run"}, {"esc", "back"}};
    hints(h, 2);
}

void drawSaved()
{
    lcd.fillScreen(kBg);
    header("SAVED NETWORKS");

    constexpr int kRows = 5;
    for (int r = 0; r < kRows; ++r) {
        const int idx = savedTop + r;
        if (idx >= wifistore::count()) break;
        const int y = 20 + r * 18;
        if (idx == savedSel) lcd.fillRect(0, y - 2, lcd.width(), 16, kBar);
        const uint16_t fg = idx == savedSel ? kAccent : kText;
        const uint16_t bg = idx == savedSel ? kBar : kBg;
        lcd.setTextColor(fg, bg);
        lcd.drawString(wifistore::at(idx).ssid, 6, y);
        if (idx == wifistore::lastIndex()) {
            lcd.setTextColor(kOk, bg);
            lcd.drawRightString("LAST", lcd.width() - 4, y);
        }
    }

    if (wifistore::count() == 0) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("No saved networks.", 6, 58);
    }

    static const Hint h[] = {{"ENT", "connect"}, {"D", "forget"}, {"esc", "back"}};
    hints(h, 3);
}

void startOta()
{
    if (otaStarted || WiFi.status() != WL_CONNECTED) return;
    ArduinoOTA.setHostname(kOtaHostname);
    ArduinoOTA.setPassword(kOtaPassword);
    ArduinoOTA.setPort(3232);
    ArduinoOTA.onStart([]() {
        otaUploading = true;
        otaPercent   = 0;
        snprintf(otaStatus, sizeof(otaStatus), "UPLOADING");
        dirty = true;
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        otaPercent = total ? static_cast<int>(progress * 100 / total) : 0;
        dirty      = true;
    });
    ArduinoOTA.onEnd([]() {
        otaUploading = false;
        otaPercent   = 100;
        snprintf(otaStatus, sizeof(otaStatus), "DONE - REBOOTING");
        dirty = true;
    });
    ArduinoOTA.onError([](ota_error_t error) {
        otaUploading = false;
        snprintf(otaStatus, sizeof(otaStatus), "ERROR %d", static_cast<int>(error));
        dirty = true;
    });
    ArduinoOTA.begin();
    otaStarted = true;
    snprintf(otaStatus, sizeof(otaStatus), "READY");
    Serial.printf("[ota] ready host=%s.local ip=%s\n", kOtaHostname,
                  WiFi.localIP().toString().c_str());
    dirty = true;
}

void drawOta()
{
    lcd.fillScreen(kBg);
    header("OTA UPDATE");

    const bool wifi = WiFi.status() == WL_CONNECTED;
    lcd.setTextColor(wifi ? kOk : kWarn, kBg);
    lcd.drawString(wifi ? "WIFI CONNECTED" : "WIFI REQUIRED", 6, 18);

    if (wifi) {
        lcd.setTextColor(kText, kBg);
        lcd.drawString(WiFi.localIP().toString().c_str(), 6, 32);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(WiFi.SSID().c_str(), 6, 44);
    }

    lcd.setTextColor(otaUploading ? kAccent : kText, kBg);
    lcd.drawString(otaStatus, 6, 60);
    if (otaUploading || otaPercent > 0) {
        lcd.drawRect(6, 72, 228, 10, kDim);
        lcd.fillRect(7, 73, 226 * otaPercent / 100, 8, kOk);
    }

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("HOST cardputer.local", 6, 84);
    lcd.drawString("PORT 3232   AUTH ON", 6, 94);
    lcd.drawString("Keep this page open while flashing.", 6, 104);

    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void enterOta()
{
    otaPercent = 0;
    otaUploading = false;
    snprintf(otaStatus, sizeof(otaStatus), "WAITING FOR WIFI");
    scr   = Scr::kOta;
    dirty = true;
    if (WiFi.status() == WL_CONNECTED) startOta();
}

void handleOta(uint32_t pressed)
{
    if (pressed & kBack && !otaUploading) {
        if (otaStarted) {
            ArduinoOTA.end();
            otaStarted = false;
        }
        scr   = Scr::kSettings;
        dirty = true;
    }
}

void drawCurrent()
{
    switch (scr) {
        case Scr::kSettings: drawSettings(); break;
        case Scr::kWifi: drawWifi(); break;
        case Scr::kScan: drawScan(); break;
        case Scr::kPassword: drawPassword(); break;
        case Scr::kConnecting: drawConnecting(); break;
        case Scr::kSaved: drawSaved(); break;
        case Scr::kTest: drawTest(); break;
        case Scr::kOta: drawOta(); break;
    }
}

void runTest()
{
    scr = Scr::kTest;
    if (WiFi.status() != WL_CONNECTED) {
        snprintf(testResult, sizeof(testResult), "NOT CONNECTED");
        dirty = true;
        return;
    }

    snprintf(testResult, sizeof(testResult), "TESTING...");
    dirty = true;
    drawCurrent();

    WiFiClient client;
    client.setTimeout(5000);
    const bool ok = client.connect("example.com", 80, 5000);
    if (ok) {
        client.print("HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n");
        delay(50);
        client.stop();
    }
    snprintf(testResult, sizeof(testResult), ok ? "INTERNET OK" : "NO INTERNET");
    Serial.printf("[wifi] tcp test example.com:80 -> %d\n", static_cast<int>(ok));
    dirty = true;
}

// ---------------------------------------------------------------------------
// Network list
// ---------------------------------------------------------------------------
void copySsid(char* dst, size_t cap, const String& src)
{
    if (cap == 0) return;
    const size_t n = src.length();
    const size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src.c_str(), c);
    dst[c] = 0;
}

void collectScan(int found)
{
    scanCount = 0;
    for (int i = 0; i < found && i < 64; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        if (ssid.length() > wifistore::kSsidMax) continue;

        int existing = -1;
        for (int j = 0; j < scanCount; ++j) {
            if (strcmp(scanItems[j].ssid, ssid.c_str()) == 0) {
                existing = j;
                break;
            }
        }
        const int32_t rssi = WiFi.RSSI(i);
        const uint8_t enc  = static_cast<uint8_t>(WiFi.encryptionType(i));
        if (existing >= 0) {
            if (rssi > scanItems[existing].rssi) {
                scanItems[existing].rssi = static_cast<int16_t>(rssi);
                scanItems[existing].enc  = enc;
            }
            continue;
        }
        if (scanCount >= kMaxScan) continue;
        copySsid(scanItems[scanCount].ssid, sizeof(scanItems[scanCount].ssid), ssid);
        scanItems[scanCount].rssi = static_cast<int16_t>(rssi);
        scanItems[scanCount].enc  = enc;
        ++scanCount;
    }

    // Strongest first.  The list is tiny, so insertion sort is clearer than
    // dragging in <algorithm> for one call.
    for (int i = 1; i < scanCount; ++i) {
        ScanItem key = scanItems[i];
        int j        = i - 1;
        while (j >= 0 && scanItems[j].rssi < key.rssi) {
            scanItems[j + 1] = scanItems[j];
            --j;
        }
        scanItems[j + 1] = key;
    }
    WiFi.scanDelete();
}

void startScan()
{
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    const int16_t rc = WiFi.scanNetworks(true, false, false, 300);
    if (rc == WIFI_SCAN_FAILED) {
        snprintf(statusMsg, sizeof(statusMsg), "SCAN FAILED");
        scanReady = true;
        dirty     = true;
        return;
    }
    scanning  = true;
    scanReady = false;
    scanCount = 0;
    scanStart = millis();
    scanSel   = 0;
    scanTop   = 0;
    snprintf(statusMsg, sizeof(statusMsg), "");
    scr   = Scr::kScan;
    dirty = true;
    Serial.println("[wifi] scan started");
}

void pollScan()
{
    if (!scanning) return;
    const int16_t res = WiFi.scanComplete();
    if (res == WIFI_SCAN_RUNNING) {
        if (millis() - scanStart > 15000) {
            scanning = false;
            WiFi.scanDelete();
            snprintf(statusMsg, sizeof(statusMsg), "SCAN TIMEOUT");
            scanReady = true;
            dirty     = true;
        }
        return;
    }
    if (res == WIFI_SCAN_FAILED) {
        scanning = false;
        snprintf(statusMsg, sizeof(statusMsg), "SCAN FAILED");
        scanReady = true;
        dirty     = true;
        return;
    }
    if (res >= 0) {
        scanning  = false;
        scanReady = true;
        collectScan(res);
        Serial.printf("[wifi] scan done: %d networks\n", scanCount);
        dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
const char* connectError()
{
    switch (WiFi.status()) {
        case WL_NO_SSID_AVAIL: return "NO AP FOUND";
        case WL_CONNECT_FAILED: return "WRONG PASSWORD";
        case WL_CONNECTION_LOST: return "CONNECTION LOST";
        default: return "TIMEOUT";
    }
}

void startConnect(const char* ssid, const char* pass, bool saveCreds, bool showPage)
{
    if (ssid == nullptr || ssid[0] == 0) return;
    copySsid(pendingSsid, sizeof(pendingSsid), String(ssid));
    copySsid(pendingPass, sizeof(pendingPass), String(pass == nullptr ? "" : pass));
    pendingSave  = saveCreds;
    connecting   = true;
    autoConnecting = !showPage;
    connectStart = millis();
    snprintf(statusMsg, sizeof(statusMsg), "CONNECTING...");
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.disconnect(false, false);
    WiFi.begin(pendingSsid, pendingPass);
    Serial.printf("[wifi] connect ssid='%s' save=%d page=%d\n", pendingSsid,
                  static_cast<int>(saveCreds), static_cast<int>(showPage));
    if (showPage) scr = Scr::kConnecting;
    dirty = true;
}

void pollConnect()
{
    if (!connecting) return;
    if (WiFi.status() == WL_CONNECTED) {
        if (pendingSave) {
            if (wifistore::save(pendingSsid, pendingPass)) {
                snprintf(statusMsg, sizeof(statusMsg), "SAVED");
            }
        } else {
            const int idx = wifistore::find(pendingSsid);
            if (idx >= 0) wifistore::markLast(idx);
        }
        connecting     = false;
        autoConnecting = false;
        WiFi.setAutoReconnect(true);
        snprintf(statusMsg, sizeof(statusMsg), "CONNECTED");
        Serial.printf("[wifi] connected ssid='%s' ip=%s\n", pendingSsid,
                      WiFi.localIP().toString().c_str());
        if (scr == Scr::kConnecting) scr = Scr::kWifi;
        dirty = true;
        return;
    }
    if (millis() - connectStart > 15000 || WiFi.status() == WL_NO_SSID_AVAIL ||
        WiFi.status() == WL_CONNECT_FAILED) {
        connecting     = false;
        autoConnecting = false;
        snprintf(statusMsg, sizeof(statusMsg), "%s", connectError());
        Serial.printf("[wifi] connect failed ssid='%s' status=%d msg=%s\n", pendingSsid,
                      static_cast<int>(WiFi.status()), statusMsg);
        if (scr == Scr::kConnecting) scr = Scr::kWifi;
        dirty = true;
    }
}

void cancelConnect()
{
    connecting     = false;
    autoConnecting = false;
    WiFi.disconnect(false, false);
    snprintf(statusMsg, sizeof(statusMsg), "CANCELLED");
    scr   = Scr::kWifi;
    dirty = true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            case ';': a |= kUp; break;
            case '.': a |= kDown; break;
            case KEY_ENTER: a |= kEnter; break;
            case '`': case '~': case 0x1B: a |= kBack; break;
            case 0x2A: a |= kDelete; break;
            case 's': case 'S': a |= kScan; break;
            case 'd': case 'D': a |= kForget; break;
            default: break;
        }
    }
    return a;
}

uint32_t prevActions = 0;

void resetInputEdges()
{
    prevWord.clear();
    prevDel    = false;
    prevEnter  = false;
}

void clampScroll(int sel, int& top, int total, int rows)
{
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (top < 0) top = 0;
    if (top > total - rows && total > rows) top = total - rows;
    if (top < 0) top = 0;
}

void forgetCurrent()
{
    const int idx = WiFi.status() == WL_CONNECTED ? wifistore::find(WiFi.SSID().c_str()) : -1;
    if (idx < 0) {
        snprintf(statusMsg, sizeof(statusMsg), "NOT CONNECTED");
        dirty = true;
        return;
    }
    if (!forgetArmed) {
        forgetArmed = true;
        forgetAt    = millis();
        snprintf(statusMsg, sizeof(statusMsg), "PRESS D AGAIN");
        dirty = true;
        return;
    }
    wifistore::removeAt(idx);
    WiFi.disconnect(false, false);
    forgetArmed = false;
    snprintf(statusMsg, sizeof(statusMsg), "FORGOTTEN");
    dirty = true;
}

void enterWifi()
{
    wifiSel = 0;
    statusMsg[0] = 0;
    scr   = Scr::kWifi;
    dirty = true;
}

void handleText()
{
    const auto& ks = M5Cardputer.Keyboard.keysState();

    if (ks.fn && ks.del) {  // cancel, never type a backspace
        resetInputEdges();
        snprintf(statusMsg, sizeof(statusMsg), "CANCELLED");
        scr   = Scr::kWifi;
        dirty = true;
        return;
    }

    bool changed = false;
    for (char c : ks.word) {
        if (c < 32 || c >= 127) continue;
        bool already = false;
        for (char p : prevWord) {
            if (p == c) {
                already = true;
                break;
            }
        }
        if (!already && passLen < wifistore::kPassMax) {
            passBuf[passLen++] = c;
            passBuf[passLen]   = 0;
            changed            = true;
        }
    }
    prevWord.assign(ks.word.begin(), ks.word.end());

    if (ks.del && !prevDel && passLen > 0) {
        passBuf[--passLen] = 0;
        changed            = true;
    }
    prevDel = ks.del;

    if (ks.enter && !prevEnter) {
        prevEnter = true;
        if (passLen > 0 && passLen < 8) {
            snprintf(statusMsg, sizeof(statusMsg), "MIN 8 CHARS");
            dirty = true;
        } else if (passLen == 0 && !passOpen) {
            snprintf(statusMsg, sizeof(statusMsg), "PASSWORD REQUIRED");
            dirty = true;
        } else if (passLen <= wifistore::kPassMax) {
            startConnect(passSsid, passBuf, true, true);
            return;
        }
    }
    prevEnter = ks.enter;

    if (changed) {
        snprintf(statusMsg, sizeof(statusMsg), "");
        dirty = true;
    }
}

void handleWifiPage(uint32_t pressed)
{
    if (pressed & kBack) {
        scr   = Scr::kSettings;
        dirty = true;
        return;
    }
    if (pressed & kUp) {
        wifiSel = (wifiSel + 3) % 4;
        dirty   = true;
    }
    if (pressed & kDown) {
        wifiSel = (wifiSel + 1) % 4;
        dirty   = true;
    }
    if (pressed & kScan) {
        startScan();
        return;
    }
    if (pressed & kForget) {
        forgetCurrent();
        return;
    }
    if (!(pressed & kEnter)) return;

    if (wifiSel == 0) {
        startScan();
    } else if (wifiSel == 1) {
        savedSel = 0;
        savedTop = 0;
        scr      = Scr::kSaved;
        dirty    = true;
    } else if (wifiSel == 2) {
        runTest();
    } else {
        forgetCurrent();
    }
}

void handleScanPage(uint32_t pressed)
{
    if (pressed & kBack) {
        scr   = Scr::kWifi;
        dirty = true;
        return;
    }
    if (pressed & kScan) {
        startScan();
        return;
    }
    if (scanCount > 0) {
        if (pressed & kUp) {
            scanSel = (scanSel + scanCount - 1) % scanCount;
            dirty   = true;
        }
        if (pressed & kDown) {
            scanSel = (scanSel + 1) % scanCount;
            dirty   = true;
        }
        clampScroll(scanSel, scanTop, scanCount, 5);
    }
    if (!(pressed & kEnter) || scanCount == 0) return;

    const ScanItem& it = scanItems[scanSel];
    copySsid(passSsid, sizeof(passSsid), String(it.ssid));
    passOpen = it.enc == WIFI_AUTH_OPEN;
    passLen  = 0;
    passBuf[0] = 0;
    statusMsg[0] = 0;
    resetInputEdges();
    prevEnter = true;  // the ENTER used to select the AP is still held
    if (passOpen) {
        startConnect(passSsid, "", true, true);
    } else {
        scr   = Scr::kPassword;
        dirty = true;
    }
}

void handleSavedPage(uint32_t pressed)
{
    if (pressed & kBack) {
        scr   = Scr::kWifi;
        dirty = true;
        return;
    }
    const int total = wifistore::count();
    if (total > 0) {
        if (pressed & kUp) {
            savedSel = (savedSel + total - 1) % total;
            dirty    = true;
        }
        if (pressed & kDown) {
            savedSel = (savedSel + 1) % total;
            dirty    = true;
        }
        clampScroll(savedSel, savedTop, total, 5);
    }
    if (pressed & kForget && total > 0) {
        wifistore::removeAt(savedSel);
        if (savedSel >= wifistore::count()) savedSel = wifistore::count() - 1;
        if (savedSel < 0) savedSel = 0;
        clampScroll(savedSel, savedTop, wifistore::count(), 5);
        snprintf(statusMsg, sizeof(statusMsg), "REMOVED");
        dirty = true;
        return;
    }
    if (!(pressed & kEnter) || total == 0) return;
    const auto& n = wifistore::at(savedSel);
    startConnect(n.ssid, n.pass, false, true);
}

void handleConnectingPage(uint32_t pressed)
{
    if (pressed & kBack) {
        cancelConnect();
    }
}

void handleTestPage(uint32_t pressed)
{
    if (pressed & kBack) {
        scr   = Scr::kWifi;
        dirty = true;
        return;
    }
    if (pressed & kEnter) runTest();
}

}  // namespace

void setup()
{
    exitRequested = false;
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));

    lcd.setRotation(rfcfg::screenRotation);
    lcd.setBrightness(90);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    lcd.fillScreen(kBg);

    wifistore::begin();

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);

    Serial.printf("[wifi] saved=%d\n", wifistore::count());
    if (wifistore::count() > 0) {
        const auto* n = wifistore::last();
        if (n != nullptr) {
            // Background auto-connect: keep the SETTINGS page visible, then show
            // the result when the operator opens WIFI.
            startConnect(n->ssid, n->pass, false, false);
        }
    }

    scr   = Scr::kSettings;
    dirty = true;
}

bool loop()
{
    M5Cardputer.update();

    pollScan();
    pollConnect();

    if (scr == Scr::kOta) {
        if (WiFi.status() == WL_CONNECTED && !otaStarted) startOta();
        if (otaStarted) ArduinoOTA.handle();
        static uint32_t lastOtaDraw = 0;
        if (otaUploading && millis() - lastOtaDraw >= 100) {
            lastOtaDraw = millis();
            dirty       = true;
        }
    }

    if (forgetArmed && millis() - forgetAt > 2000) {
        forgetArmed = false;
        if (strcmp(statusMsg, "PRESS D AGAIN") == 0) statusMsg[0] = 0;
        dirty = true;
    }

    const uint32_t a       = readActions();
    const uint32_t pressed = a & ~prevActions;
    prevActions            = a;

    switch (scr) {
        case Scr::kSettings:
            if (pressed & kBack) {
                // Do not reboot: keeping the WiFi stack alive is what lets the
                // launcher header show the real link state after returning.
                exitRequested = true;
                return true;
            }
            if (pressed & kUp) {
                settingsSel = (settingsSel + 2) % 3;
                dirty       = true;
            }
            if (pressed & kDown) {
                settingsSel = (settingsSel + 1) % 3;
                dirty       = true;
            }
            if (pressed & kEnter) {
                if (settingsSel == 0) {
                    enterWifi();
                } else if (settingsSel == 1) {
                    enterOta();
                } else {
                    launcher::rebootInto(launcher::kAppBackup);
                }
            }
            break;
        case Scr::kOta:
            handleOta(pressed);
            break;
        case Scr::kWifi:
            handleWifiPage(pressed);
            break;
        case Scr::kScan:
            handleScanPage(pressed);
            break;
        case Scr::kPassword:
            handleText();
            break;
        case Scr::kConnecting:
            handleConnectingPage(pressed);
            break;
        case Scr::kSaved:
            handleSavedPage(pressed);
            break;
        case Scr::kTest:
            handleTestPage(pressed);
            break;
    }

    if (dirty) {
        dirty = false;
        drawCurrent();
    }
    delay(5);
    return false;
}

}  // namespace settingsapp
