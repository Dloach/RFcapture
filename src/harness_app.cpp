#include "harness_app.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "ui_hint.h"
#include "wifi_store.h"

namespace harnessapp {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;
constexpr uint16_t kPort   = 8765;
constexpr int kChatLines   = 8;

auto& lcd = M5Cardputer.Display;
enum class Screen : uint8_t { kChat, kSessions, kConfig };
Screen screen = Screen::kChat;
bool dirty = true;
bool wifiOk = false;
bool bridgeOk = false;
bool busy = false;

String host = "192.168.0.88";
String token = "cardputer-dev";
String sessionId;
String status = "STARTING";
String input;
String lastError;

struct Msg {
    long seq = 0;
    String role;
    String text;
};
std::vector<Msg> msgs;
std::vector<String> chatLines;
std::vector<String> sessionIds;
int lineScroll = 0;
int msgScroll = 0;
int sessSel = 0;
int configField = 0;
String configEdit;
std::vector<char> prevWord;
bool prevEnter = false;
bool prevEsc = false;

struct Hint {
    const char* key;
    const char* text;
};

void hints(const Hint* h, int n)
{
    lcd.drawFastHLine(0, 118, lcd.width(), kDim);
    int x = 3;
    for (int i = 0; i < n; ++i) {
        lcd.setTextColor(kOk, kBg);
        lcd.drawString(h[i].key, x, 122);
        x += lcd.textWidth(h[i].key) + 4;
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(h[i].text, x, 122);
        x += lcd.textWidth(h[i].text) + 8;
    }
}

void header()
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("HARNESS", 3, 2);
    char b[20];
    snprintf(b, sizeof(b), "%d%%", M5.Power.getBatteryLevel());
    lcd.drawRightString(b, lcd.width() - 3, 2);
}

void loadPrefs()
{
    Preferences p;
    if (!p.begin("harness", true)) return;
    host = p.getString("host", host);
    token = p.getString("token", token);
    sessionId = p.getString("session", "");
    p.end();
}

void savePrefs()
{
    Preferences p;
    if (!p.begin("harness", false)) return;
    p.putString("host", host);
    p.putString("token", token);
    p.putString("session", sessionId);
    p.end();
}

String baseUrl()
{
    return String("http://") + host + ":" + String(kPort);
}

bool requestJson(const String& path, JsonDocument& doc, bool post = false, const String& body = "")
{
    WiFiClient client;
    HTTPClient http;
    const String url = baseUrl() + path;
    if (!http.begin(client, url)) return false;
    http.setTimeout(6000);
    http.addHeader("X-Cardputer-Token", token);
    int code;
    if (post) {
        http.addHeader("Content-Type", "application/json");
        code = http.POST(body);
    } else {
        code = http.GET();
    }
    const String payload = http.getString();
    http.end();
    if (code < 200 || code >= 300) {
        lastError = String("HTTP ") + code;
        return false;
    }
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        lastError = err.c_str();
        return false;
    }
    return true;
}

bool fetchSessions()
{
    JsonDocument doc;
    if (!requestJson("/cardputer/sessions", doc)) return false;
    sessionIds.clear();
    for (JsonObject item : doc["sessions"].as<JsonArray>()) {
        sessionIds.push_back(item["sessionId"].as<String>());
    }
    return true;
}

bool createSession()
{
    JsonDocument doc;
    JsonDocument body;
    body["cwd"] = "Y:\\2026\\cardputer";
    String out;
    serializeJson(body, out);
    if (!requestJson("/cardputer/new", doc, true, out)) return false;
    sessionId = doc["sessionId"].as<String>();
    savePrefs();
    return !sessionId.isEmpty();
}

bool fetchMessages()
{
    if (sessionId.isEmpty()) return false;
    JsonDocument doc;
    char path[96];
    snprintf(path, sizeof(path), "/cardputer/messages?session=%s&limit=80", sessionId.c_str());
    if (!requestJson(path, doc)) return false;
    msgs.clear();
    for (JsonObject item : doc["messages"].as<JsonArray>()) {
        Msg m;
        m.seq = item["seq"] | 0;
        m.role = item["role"].as<String>();
        m.text = item["text"].as<String>();
        if (!m.text.isEmpty()) msgs.push_back(m);
    }
    return true;
}

bool sendPrompt(const String& text)
{
    if (sessionId.isEmpty()) return false;
    JsonDocument doc;
    JsonDocument body;
    body["session"] = sessionId;
    body["text"] = text;
    String out;
    serializeJson(body, out);
    return requestJson("/cardputer/prompt", doc, true, out);
}

constexpr size_t kVoiceSamples = 16000 * 2;  // 2 s mono
uint8_t voiceWav[44 + kVoiceSamples * 2];

bool recordAndRecognize(String& recognized)
{
    if (!bridgeOk) return false;
    M5.Speaker.end();
    delay(100);
    M5.Mic.end();
    if (!M5.Mic.begin()) {
        lastError = "MIC FAILED";
        return false;
    }
    M5.Mic.setSampleRate(16000);
    delay(1200);
    int16_t* pcm = reinterpret_cast<int16_t*>(voiceWav + 44);
    size_t at = 0;
    while (at + 1600 <= kVoiceSamples) {
        if (!M5.Mic.record(pcm + at, 1600, 16000, false)) {
            delay(5);
            continue;
        }
        at += 1600;
        delay(105);
        M5Cardputer.update();
    }
    M5.Mic.end();

    struct __attribute__((packed)) WavHeader {
        char riff[4];
        uint32_t riffSize;
        char wave[4];
        char fmt[4];
        uint32_t fmtSize;
        uint16_t format;
        uint16_t channels;
        uint32_t rate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bits;
        char data[4];
        uint32_t dataSize;
    } h{};
    memcpy(h.riff, "RIFF", 4);
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    memcpy(h.data, "data", 4);
    h.fmtSize = 16;
    h.format = 1;
    h.channels = 1;
    h.rate = 16000;
    h.byteRate = 32000;
    h.blockAlign = 2;
    h.bits = 16;
    h.dataSize = kVoiceSamples * 2;
    h.riffSize = 36 + h.dataSize;
    memcpy(voiceWav, &h, sizeof(h));

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, baseUrl() + "/cardputer/stt")) {
        lastError = "STT CONNECT";
        return false;
    }
    http.setTimeout(60000);
    http.addHeader("X-Cardputer-Token", token);
    http.addHeader("Content-Type", "audio/wav");
    const int code = http.POST(voiceWav, sizeof(h) + kVoiceSamples * 2);
    const String payload = http.getString();
    http.end();
    if (code < 200 || code >= 300) {
        lastError = String("STT HTTP ") + code;
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, payload)) {
        lastError = "STT JSON";
        return false;
    }
    recognized = doc["text"].as<String>();
    return !recognized.isEmpty();
}

void connectWifi()
{
    wifistore::begin();
    const auto* n = wifistore::last();
    if (n == nullptr) {
        status = "NO WIFI";
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(n->ssid, n->pass);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        M5Cardputer.update();
        delay(50);
    }
    wifiOk = WiFi.status() == WL_CONNECTED;
    status = wifiOk ? "WIFI OK" : "WIFI FAILED";
}

int utf8Len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

void addWrapped(std::vector<String>& out, const String& text, const String& prefix)
{
    String line = prefix;
    int i = 0;
    while (i < (int)text.length()) {
        const unsigned char c = text[i];
        const int n = utf8Len(c);
        String part = text.substring(i, i + n);
        String next = line + part;
        if (lcd.textWidth(next) > lcd.width() - 10 && line.length() > prefix.length()) {
            out.push_back(line);
            line = "  " + part;
        } else {
            line = next;
        }
        i += n;
    }
    if (line.length() > prefix.length()) out.push_back(line);
}

void rebuildChat()
{
    chatLines.clear();
    lcd.setFont(&lgfx::fonts::efontCN_12);
    const int begin = 0;
    for (int i = begin; i < (int)msgs.size(); ++i) {
        const String prefix = msgs[i].role == "user" ? "You: " : "AI:  ";
        addWrapped(chatLines, msgs[i].text, prefix);
    }
    if (chatLines.empty()) chatLines.push_back("No messages. Type below.");
    lcd.setTextFont(1);
}

void renderChat()
{
    lcd.fillScreen(kBg);
    header();
    lcd.setTextFont(1);
    lcd.setTextColor(bridgeOk ? kOk : kWarn, kBg);
    lcd.drawString(bridgeOk ? status : (lastError.isEmpty() ? status : lastError), 3, 15);
    lcd.setTextColor(kDim, kBg);
    lcd.drawRightString(sessionId.isEmpty() ? "no session" : sessionId.substring(0, 8),
                        lcd.width() - 3, 15);

    lcd.setFont(&lgfx::fonts::efontCN_12);
    lcd.setTextColor(kText, kBg);
    const int lineH = 12;
    const int maxLines = 6;
    int first = (int)chatLines.size() - maxLines - lineScroll;
    if (first < 0) first = 0;
    int y = 26;
    for (int i = first; i < (int)chatLines.size() && y < 100; ++i) {
        lcd.drawString(chatLines[i], 3, y);
        y += lineH;
    }

    lcd.setFont(&lgfx::fonts::efontCN_12);
    lcd.fillRect(0, 105, lcd.width(), 12, kBg);
    lcd.setTextColor(kAccent, kBg);
    String shown = "> " + input;
    while (lcd.textWidth(shown) > lcd.width() - 6 && shown.length() > 2) shown.remove(0, 1);
    lcd.drawString(shown, 3, 105);
    lcd.setTextFont(1);
    static const Hint h[] = {{"ENT", "send"}, {"FnV", "voice"}, {"esc", "back"}};
    hints(h, 3);
    dirty = false;
}

void renderSessions()
{
    lcd.fillScreen(kBg);
    header();
    lcd.setTextFont(1);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("SESSIONS", 3, 16);
    if (sessionIds.empty()) {
        lcd.drawString("none  N new", 3, 32);
    }
    const int first = msgScroll;
    for (int r = 0; r < 7; ++r) {
        const int idx = first + r;
        if (idx >= (int)sessionIds.size()) break;
        const int y = 28 + r * 12;
        if (idx == sessSel) lcd.fillRect(0, y - 2, lcd.width(), 12, kBar);
        lcd.setTextColor(idx == sessSel ? kAccent : kText, idx == sessSel ? kBar : kBg);
        lcd.drawString(sessionIds[idx], 4, y);
    }
    static const Hint h[] = {{"ENT", "open"}, {"N", "new"}, {"esc", "back"}};
    hints(h, 3);
}

void renderConfig()
{
    lcd.fillScreen(kBg);
    header();
    lcd.setTextFont(1);
    lcd.setTextColor(kText, kBg);
    lcd.drawString("BRIDGE CONFIG", 3, 16);
    lcd.setTextColor(configField == 0 ? kAccent : kDim, kBg);
    lcd.drawString("HOST", 3, 32);
    lcd.setTextColor(kText, kBg);
    lcd.drawString(configField == 0 ? configEdit : host, 44, 32);
    lcd.setTextColor(configField == 1 ? kAccent : kDim, kBg);
    lcd.drawString("TOKEN", 3, 46);
    lcd.setTextColor(kText, kBg);
    lcd.drawString(configField == 1 ? configEdit : token, 44, 46);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("ENT save  ; . field  esc back", 3, 84);
}

void render()
{
    switch (screen) {
        case Screen::kChat: renderChat(); break;
        case Screen::kSessions: renderSessions(); break;
        case Screen::kConfig: renderConfig(); break;
    }
    dirty = false;
}

void startChat()
{
    status = "CONNECTING";
    if (!wifiOk) connectWifi();
    if (!wifiOk) {
        bridgeOk = false;
        dirty = true;
        return;
    }
    if (!fetchSessions()) {
        bridgeOk = false;
        status = "BRIDGE OFFLINE";
        dirty = true;
        return;
    }
    bridgeOk = true;
    if (sessionId.isEmpty() || std::find(sessionIds.begin(), sessionIds.end(), sessionId) == sessionIds.end()) {
        sessionId.clear();
        for (const auto& id : sessionIds) {
            if (!id.isEmpty()) {
                sessionId = id;
                break;
            }
        }
        if (sessionId.isEmpty()) createSession();
        savePrefs();
    }
    fetchMessages();
    rebuildChat();
    lineScroll = 0;
    status = "CONNECTED";
    dirty = true;
}

void sendInput()
{
    if (input.isEmpty() || !bridgeOk) return;
    busy = true;
    status = "SENDING";
    if (!sendPrompt(input)) {
        status = "SEND FAILED";
        lastError = "prompt rejected";
        busy = false;
        dirty = true;
        return;
    }
    input = "";
    status = "SENT";
    busy = false;
    fetchMessages();
    rebuildChat();
    lineScroll = 0;
    dirty = true;
}

void poll()
{
    if (!bridgeOk || busy) return;
    if (fetchMessages()) {
        rebuildChat();
        dirty = true;
    }
}

void handleChat()
{
    const auto ks = M5Cardputer.Keyboard.keysState();
    const bool fn = ks.fn;

    for (char c : ks.word) {
        if (c < 32 || c >= 127) continue;
        bool already = false;
        for (char p : prevWord) if (p == c) { already = true; break; }
        if (already) continue;
        if (fn && (c == 'v' || c == 'V')) {
            status = "LISTEN 2s";
            dirty = true;
            render();
            String recognized;
            if (recordAndRecognize(recognized)) {
                input += recognized;
                status = "VOICE OK";
            } else {
                status = lastError.isEmpty() ? "VOICE FAILED" : lastError;
            }
            dirty = true;
            continue;
        }
        if (fn && (c == 's' || c == 'S')) {
            screen = Screen::kSessions;
            sessSel = 0;
            msgScroll = 0;
            configEdit.clear();
            dirty = true;
            continue;
        }
        if (fn && (c == 'c' || c == 'C')) {
            screen = Screen::kConfig;
            configField = 0;
            configEdit = host;
            dirty = true;
            continue;
        }
        if (c == ';' || c == '.') {
            if (fn) {
                lineScroll += (c == ';') ? 1 : -1;
                if (lineScroll < 0) lineScroll = 0;
                dirty = true;
            } else {
                input += c;
                dirty = true;
            }
            continue;
        }
        input += c;
        dirty = true;
    }
    prevWord.assign(ks.word.begin(), ks.word.end());

    if (ks.del && input.length() > 0) {
        input.remove(input.length() - 1);
        dirty = true;
    }
    if (ks.enter && !prevEnter) {
        sendInput();
        prevEnter = true;
    }
    if (!ks.enter) prevEnter = false;

    if (M5Cardputer.Keyboard.isKeyPressed('`') && !prevEsc) {
        launcher::rebootInto(launcher::kAppMenu);
    }
    prevEsc = M5Cardputer.Keyboard.isKeyPressed('`');
}

void handleSessions()
{
    const auto ks = M5Cardputer.Keyboard.keysState();
    if (ks.enter) {
        if (sessSel >= 0 && sessSel < (int)sessionIds.size()) {
            sessionId = sessionIds[sessSel];
            savePrefs();
            fetchMessages();
            rebuildChat();
            lineScroll = 0;
            screen = Screen::kChat;
            dirty = true;
        }
        return;
    }
    for (char c : ks.word) {
        if (c == 'N' || c == 'n') {
            if (createSession()) {
                fetchSessions();
                sessSel = 0;
            }
        }
    }
    if (ks.fn) {
        // reserved
    } else if (M5Cardputer.Keyboard.isKeyPressed(';') && sessSel > 0) {
        --sessSel;
        dirty = true;
    } else if (M5Cardputer.Keyboard.isKeyPressed('.') &&
               sessSel + 1 < (int)sessionIds.size()) {
        ++sessSel;
        dirty = true;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('`')) {
        screen = Screen::kChat;
        dirty = true;
    }
}

void handleConfig()
{
    const auto ks = M5Cardputer.Keyboard.keysState();
    for (char c : ks.word) {
        if (c < 32 || c >= 127) continue;
        bool already = false;
        for (char p : prevWord) if (p == c) { already = true; break; }
        if (already) continue;
        if (ks.fn && (c == ';' || c == '.')) {
            configField ^= 1;
            configEdit = configField == 0 ? host : token;
        } else if (c == '`') {
            screen = Screen::kChat;
            dirty = true;
        } else {
            configEdit += c;
        }
        dirty = true;
    }
    prevWord.assign(ks.word.begin(), ks.word.end());
    if (ks.del && !configEdit.isEmpty()) {
        configEdit.remove(configEdit.length() - 1);
        dirty = true;
    }
    if (ks.enter) {
        if (configField == 0) host = configEdit;
        else token = configEdit;
        savePrefs();
        screen = Screen::kChat;
        bridgeOk = false;
        status = "RECONNECT";
        startChat();
    }
}

void pollIfNeeded()
{
    static uint32_t last = 0;
    if (screen != Screen::kChat) return;
    if (millis() - last < 1500) return;
    last = millis();
    poll();
}

}  // namespace

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);
    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));
    lcd.setRotation(rfcfg::screenRotation);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    lcd.fillScreen(kBg);

    loadPrefs();
    screen = Screen::kChat;
    startChat();
    render();
}

void loop()
{
    M5Cardputer.update();
    switch (screen) {
        case Screen::kChat:
            handleChat();
            pollIfNeeded();
            break;
        case Screen::kSessions:
            handleSessions();
            break;
        case Screen::kConfig:
            handleConfig();
            break;
    }
    if (dirty) render();
    delay(5);
}

}  // namespace harnessapp
