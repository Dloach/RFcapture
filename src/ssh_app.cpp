#include "ssh_app.h"

#include <M5Cardputer.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>

#include <cstring>
#include <vector>

#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "ssh_terminal.h"
#include "ui_hint.h"
#include "wifi_store.h"

namespace sshapp {
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

struct Profile {
    char host[64] = {0};
    char user[32] = {0};
    char pass[64] = {0};
    uint16_t port = 22;
};

Profile profile;
bool profileLoaded = false;

enum class Scr : uint8_t {
    kProfile,
    kEdit,
    kWifiWait,
    kConnecting,
    kTerminal,
    kError,
};
Scr scr = Scr::kProfile;
bool dirty = true;
int sel = 0;

char note[64] = {0};
bool wifiConnecting = false;
uint32_t wifiStart = 0;
bool wifiForSsh = false;

// SSH task state is written by the task and polled by the UI.
volatile int sshPhase = 0;  // 0 idle, 1 TCP, 2 auth, 3 shell, 4 failed, 5 closed
volatile bool sshRunning = false;
volatile bool sshStop = false;
char sshError[64] = {0};
QueueHandle_t keyQ = nullptr;

char connectHost[64] = {0};
char connectUser[32] = {0};
char connectPass[64] = {0};
uint16_t connectPort = 22;

// Editing.
int editField = 0;
char editBuf[64] = {0};
int editLen = 0;
std::vector<char> prevWord;
bool prevDel = false;
bool prevEnter = false;
bool prevTab = false;
uint8_t prevArrow = 0;

// ---------------------------------------------------------------------------
// Small UI helpers
// ---------------------------------------------------------------------------
void header(const char* title)
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(title, 3, 2);

    const bool wifi = WiFi.status() == WL_CONNECTED;
    const uint16_t wc = wifi ? kOk : kDim;
    lcd.fillRect(190, 7, 2, 4, wc);
    lcd.fillRect(194, 5, 2, 6, wc);
    lcd.fillRect(198, 3, 2, 8, wc);
    if (!wifi) lcd.drawLine(189, 11, 201, 2, kWarn);

    const int level = M5.Power.getBatteryLevel();
    char bat[12];
    snprintf(bat, sizeof(bat), level < 0 ? "--" : "%d%%", level);
    lcd.setTextColor(kText, kHead);
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

void copyString(char* dst, size_t cap, const String& src)
{
    if (cap == 0) return;
    const size_t n = src.length();
    const size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src.c_str(), c);
    dst[c] = 0;
}

void copyCString(char* dst, size_t cap, const char* src)
{
    if (cap == 0) return;
    if (src == nullptr) src = "";
    const size_t n = strlen(src);
    const size_t c = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, c);
    dst[c] = 0;
}

// ---------------------------------------------------------------------------
// Profile persistence (one profile for the MVP; the NVS keys are already
// namespaced so a multi-profile list can be added without touching WiFi).
// ---------------------------------------------------------------------------
void loadProfile()
{
    if (profileLoaded) return;
    profileLoaded = true;
    Preferences p;
    if (!p.begin("ssh", true)) return;
    copyString(profile.host, sizeof(profile.host), p.getString("host", ""));
    copyString(profile.user, sizeof(profile.user), p.getString("user", ""));
    copyString(profile.pass, sizeof(profile.pass), p.getString("pass", ""));
    profile.port = p.getUShort("port", 22);
    p.end();
    if (profile.port == 0) profile.port = 22;
}

void saveProfile()
{
    Preferences p;
    if (!p.begin("ssh", false)) return;
    p.putString("host", profile.host);
    p.putString("user", profile.user);
    p.putString("pass", profile.pass);
    p.putUShort("port", profile.port);
    p.end();
    Serial.printf("[ssh] profile saved host='%s' user='%s' port=%u\n", profile.host,
                  profile.user, static_cast<unsigned>(profile.port));
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void drawProfile()
{
    lcd.fillScreen(kBg);
    header("SSH");

    const bool wifi = WiFi.status() == WL_CONNECTED;
    lcd.setTextColor(wifi ? kOk : (wifiConnecting ? kAccent : kDim), kBg);
    lcd.drawString(wifi ? "WIFI CONNECTED" : (wifiConnecting ? "WIFI CONNECTING..." : "WIFI OFF"), 6,
                   16);

    const int rows = 5;
    for (int i = 0; i < rows; ++i) {
        const int y = 30 + i * 15;
        if (i == sel) lcd.fillRect(2, y - 2, lcd.width() - 4, 13, kBar);
        const uint16_t bg = i == sel ? kBar : kBg;
        const uint16_t fg = i == sel ? kAccent : kText;
        lcd.setTextColor(fg, bg);
        if (i == 0) {
            lcd.drawString("HOST", 6, y);
            char val[30];
            snprintf(val, sizeof(val), "%.26s", profile.host[0] ? profile.host : "(empty)");
            lcd.drawString(val, 58, y);
        } else if (i == 1) {
            char port[8];
            snprintf(port, sizeof(port), "%u", static_cast<unsigned>(profile.port));
            lcd.drawString("PORT", 6, y);
            lcd.drawString(port, 58, y);
        } else if (i == 2) {
            lcd.drawString("USER", 6, y);
            char val[30];
            snprintf(val, sizeof(val), "%.26s", profile.user[0] ? profile.user : "(empty)");
            lcd.drawString(val, 58, y);
        } else if (i == 3) {
            char stars[24];
            const int n = strlen(profile.pass);
            const int show = n < 20 ? n : 20;
            for (int k = 0; k < show; ++k) stars[k] = '*';
            if (show == 20) stars[19] = '+';
            stars[show] = 0;
            lcd.drawString("PASS", 6, y);
            lcd.drawString(profile.pass[0] ? stars : "(empty)", 58, y);
        } else {
            lcd.setTextColor(kOk, bg);
            lcd.drawString("CONNECT", 6, y);
            lcd.setTextColor(kDim, bg);
            lcd.drawString("ENTER", 80, y);
        }
    }

    static const Hint h[] = {{"; .", "move"}, {"ENT", "edit/connect"}, {"esc", "back"}};
    hints(h, 3);
}

void drawEdit()
{
    lcd.fillScreen(kBg);
    const char* titles[] = {"EDIT HOST", "EDIT PORT", "EDIT USER", "EDIT PASSWORD"};
    header(titles[editField]);

    char shown[44];
    int n = 0;
    if (editField == 3) {
        const int maxShow = 32;
        if (editLen > maxShow) {
            shown[n++] = '.';
            shown[n++] = '.';
            shown[n++] = '.';
            for (int i = editLen - (maxShow - 3); i < editLen; ++i) shown[n++] = '*';
        } else {
            for (int i = 0; i < editLen; ++i) shown[n++] = '*';
        }
    } else {
        for (int i = 0; i < editLen; ++i) shown[n++] = editBuf[i];
    }
    shown[n++] = '_';
    shown[n]   = 0;

    lcd.setTextColor(kText, kBg);
    lcd.drawString(shown, 6, 42);

    char count[32];
    snprintf(count, sizeof(count), "LEN %d", editLen);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(count, 6, 64);
    if (editField == 1) lcd.drawString("1-65535", 6, 78);

    static const Hint h[] = {{"ENT", "save"}, {"BKSP", "delete"}, {"FN BKSP", "cancel"}};
    hints(h, 3);
}

void drawWifiWait()
{
    lcd.fillScreen(kBg);
    header("SSH");
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString("CONNECTING WIFI", 6, 30);
    lcd.setTextColor(kText, kBg);
    const auto* n = wifistore::last();
    lcd.drawString(n ? n->ssid : "(no saved network)", 6, 52);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Waiting for DHCP / association...", 6, 72);

    static const Hint h[] = {{"esc", "cancel"}};
    hints(h, 1);
}

void drawConnecting()
{
    lcd.fillScreen(kBg);
    header("SSH");
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString("CONNECTING SSH", 6, 30);
    lcd.setTextColor(kText, kBg);
    char host[32];
    snprintf(host, sizeof(host), "%.26s", connectHost);
    lcd.drawString(host, 6, 50);
    const char* phase = "TCP...";
    if (sshPhase == 2) phase = "AUTHENTICATING...";
    if (sshPhase == 3) phase = "OPENING SHELL...";
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(phase, 6, 70);

    static const Hint h[] = {{"esc", "cancel"}};
    hints(h, 1);
}

void drawTerminal()
{
    lcd.fillScreen(kBg);
    char title[32];
    snprintf(title, sizeof(title), "SSH %.20s", connectHost[0] ? connectHost : "?");
    header(title);

    char snap[sshterm::kRows][sshterm::kCols + 1];
    sshterm::snapshot(snap);
    lcd.setTextColor(kText, kBg);
    for (int r = 0; r < sshterm::kRows; ++r) {
        const int y = 14 + r * 8;
        lcd.fillRect(0, y, lcd.width(), 8, kBg);
        lcd.drawString(snap[r], 0, y);
    }

    static const Hint h[] = {{"FN BKSP", "back"}, {"FN Q", "close"}};
    hints(h, 2);
}

void drawError()
{
    lcd.fillScreen(kBg);
    header("SSH ERROR");
    lcd.setTextColor(kWarn, kBg);
    char msg[38];
    snprintf(msg, sizeof(msg), "%.37s", note[0] ? note : "FAILED");
    lcd.drawString(msg, 6, 30);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Check profile, WiFi, host key.", 6, 58);
    lcd.drawString("ENT: profile   esc: launcher", 6, 76);

    static const Hint h[] = {{"ENT", "profile"}, {"esc", "launcher"}};
    hints(h, 2);
}

void drawCurrent()
{
    switch (scr) {
        case Scr::kProfile: drawProfile(); break;
        case Scr::kEdit: drawEdit(); break;
        case Scr::kWifiWait: drawWifiWait(); break;
        case Scr::kConnecting: drawConnecting(); break;
        case Scr::kTerminal: drawTerminal(); break;
        case Scr::kError: drawError(); break;
    }
}

// ---------------------------------------------------------------------------
// SSH task
// ---------------------------------------------------------------------------
void setError(const char* msg)
{
    copyCString(sshError, sizeof(sshError), msg);
    sshPhase = 4;
}

void sshTask(void*)
{
    ssh_session session = ssh_new();
    ssh_channel channel  = nullptr;
    if (session == nullptr) {
        setError("NO MEMORY (ssh_new)");
        sshRunning = false;
        vTaskDelete(nullptr);
        return;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, connectHost);
    unsigned int port = connectPort;
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, connectUser);
    long timeout = 10;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    if (!SPIFFS.begin(true) || !SPIFFS.exists("/ssh")) {
        if (!SPIFFS.mkdir("/ssh")) {
            setError("SPIFFS /ssh unavailable");
            goto done;
        }
    }
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, "/spiffs/ssh/known_hosts");

    sshPhase = 1;
    if (ssh_connect(session) != SSH_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "TCP: %.30s", ssh_get_error(session));
        setError(msg);
        goto done;
    }

    {
        const enum ssh_known_hosts_e known = ssh_session_is_known_server(session);
        if (known == SSH_KNOWN_HOSTS_CHANGED || known == SSH_KNOWN_HOSTS_OTHER) {
            setError("HOST KEY CHANGED");
            goto done;
        }
        if (known == SSH_KNOWN_HOSTS_UNKNOWN || known == SSH_KNOWN_HOSTS_NOT_FOUND) {
            // LibSSH-ESP32 5.10 uses the newer known-hosts update API; the old
            // ssh_write_knownhost symbol is declared but not provided.
            if (ssh_session_update_known_hosts(session) != SSH_OK) {
                setError("KNOWN_HOSTS WRITE FAILED");
                goto done;
            }
        }
    }

    sshPhase = 2;
    if (ssh_userauth_password(session, nullptr, connectPass) != SSH_AUTH_SUCCESS) {
        char msg[48];
        snprintf(msg, sizeof(msg), "AUTH: %.30s", ssh_get_error(session));
        setError(msg);
        goto done;
    }

    channel = ssh_channel_new(session);
    if (channel == nullptr || ssh_channel_open_session(channel) != SSH_OK) {
        setError("CHANNEL OPEN FAILED");
        goto done;
    }
    if (ssh_channel_request_pty_size(channel, "xterm", sshterm::kCols, sshterm::kRows) != SSH_OK) {
        setError("PTY FAILED");
        goto done;
    }
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        setError("SHELL FAILED");
        goto done;
    }

    sshterm::clear();
    sshPhase = 3;
    Serial.printf("[ssh] shell open %s@%s:%u\n", connectUser, connectHost,
                  static_cast<unsigned>(connectPort));

    char buf[512];
    while (!sshStop) {
        uint8_t key;
        while (keyQ != nullptr && xQueueReceive(keyQ, &key, 0) == pdTRUE) {
            ssh_channel_write(channel, &key, 1);
        }
        const int n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0);
        if (n > 0) {
            sshterm::feed(buf, n);
        } else if (n < 0 && n != SSH_AGAIN) {
            break;
        }
        if (ssh_channel_is_eof(channel)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

done:
    if (channel != nullptr) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    ssh_disconnect(session);
    ssh_free(session);
    sshRunning = false;
    // Preserve a failure phase so the UI can show sshError; only turn a normal
    // stop/EOF into "closed".
    if (sshPhase != 4) sshPhase = 5;
    vTaskDelete(nullptr);
}

bool startSsh()
{
    if (sshRunning) return true;
    if (profile.host[0] == 0) {
        copyCString(note, sizeof(note), "HOST IS EMPTY");
        scr = Scr::kError;
        dirty = true;
        return false;
    }

    copyCString(connectHost, sizeof(connectHost), profile.host);
    copyCString(connectUser, sizeof(connectUser), profile.user);
    copyCString(connectPass, sizeof(connectPass), profile.pass);
    connectPort = profile.port;
    sshError[0] = 0;
    sshStop     = false;
    sshPhase    = 1;
    sshterm::clear();

    if (keyQ == nullptr) keyQ = xQueueCreate(128, sizeof(uint8_t));
    if (keyQ == nullptr) {
        copyCString(note, sizeof(note), "NO MEMORY (key queue)");
        scr   = Scr::kError;
        dirty = true;
        return false;
    }

    const BaseType_t ok =
        xTaskCreatePinnedToCore(sshTask, "ssh", 56 * 1024, nullptr, 5, nullptr, 1);
    if (ok != pdPASS) {
        copyCString(note, sizeof(note), "NO MEMORY (ssh task)");
        scr   = Scr::kError;
        dirty = true;
        return false;
    }

    sshRunning = true;
    scr        = Scr::kConnecting;
    dirty      = true;
    return true;
}

void stopSsh()
{
    sshStop  = true;
    sshPhase = 0;
    scr      = Scr::kProfile;
    dirty    = true;
}

void startWifiForSsh()
{
    if (WiFi.status() == WL_CONNECTED) {
        startSsh();
        return;
    }

    wifistore::begin();
    const auto* net = wifistore::last();
    if (net == nullptr) {
        copyCString(note, sizeof(note), "NO WIFI - USE SETTINGS");
        scr   = Scr::kError;
        dirty = true;
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.disconnect(false, false);
    WiFi.begin(net->ssid, net->pass);
    wifiConnecting = true;
    wifiStart      = millis();
    wifiForSsh     = true;
    copyCString(note, sizeof(note), "");
    scr   = Scr::kWifiWait;
    dirty = true;
    Serial.printf("[ssh] wifi connect '%s'\n", net->ssid);
}

void pollWifi()
{
    if (!wifiConnecting) return;
    if (WiFi.status() == WL_CONNECTED) {
        wifiConnecting = false;
        if (wifiForSsh) {
            wifiForSsh = false;
            startSsh();
        }
        dirty = true;
        return;
    }
    if (millis() - wifiStart > 15000) {
        wifiConnecting = false;
        copyCString(note, sizeof(note), "WIFI CONNECT TIMEOUT");
        scr   = Scr::kError;
        dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
enum : uint32_t {
    kUp     = 1u << 0,
    kDown   = 1u << 1,
    kEnter  = 1u << 2,
    kBack   = 1u << 3,
    kDelete = 1u << 4,
};

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
            default: break;
        }
    }
    return a;
}

uint32_t prevActions = 0;

void sendKeyByte(uint8_t b)
{
    if (keyQ != nullptr) xQueueSend(keyQ, &b, 0);
}

void resetEditEdges()
{
    prevWord.clear();
    prevDel   = false;
    prevEnter = false;
}

void beginEdit(int field)
{
    editField = field;
    editLen   = 0;
    editBuf[0] = 0;
    const char* src = field == 0 ? profile.host : field == 2 ? profile.user : profile.pass;
    if (field == 1) {
        snprintf(editBuf, sizeof(editBuf), "%u", static_cast<unsigned>(profile.port));
    } else {
        copyCString(editBuf, sizeof(editBuf), src);
    }
    editLen = strlen(editBuf);
    resetEditEdges();
    // The ENTER that opened this page is still held on the next loop; require
    // a release before it can be interpreted as "save".
    prevEnter = true;
    scr   = Scr::kEdit;
    dirty = true;
}

void finishEdit()
{
    if (editField == 1) {
        const long v = atol(editBuf);
        if (v < 1 || v > 65535) {
            copyCString(note, sizeof(note), "PORT 1-65535");
            dirty = true;
            return;
        }
        profile.port = static_cast<uint16_t>(v);
    } else if (editField == 0) {
        copyCString(profile.host, sizeof(profile.host), editBuf);
    } else if (editField == 2) {
        copyCString(profile.user, sizeof(profile.user), editBuf);
    } else {
        copyCString(profile.pass, sizeof(profile.pass), editBuf);
    }
    saveProfile();
    scr   = Scr::kProfile;
    dirty = true;
}

void handleEdit()
{
    const auto& ks = M5Cardputer.Keyboard.keysState();
    if (ks.fn && ks.del) {
        scr   = Scr::kProfile;
        dirty = true;
        return;
    }

    bool changed = false;
    for (char c : ks.word) {
        if (c < 32 || c >= 127) continue;
        if (editField == 1 && (c < '0' || c > '9')) continue;
        bool already = false;
        for (char p : prevWord) {
            if (p == c) {
                already = true;
                break;
            }
        }
        if (!already && editLen < (int)sizeof(editBuf) - 1) {
            editBuf[editLen++] = c;
            editBuf[editLen]   = 0;
            changed            = true;
        }
    }
    prevWord.assign(ks.word.begin(), ks.word.end());

    if (ks.del && !prevDel && editLen > 0) {
        editBuf[--editLen] = 0;
        changed            = true;
    }
    prevDel = ks.del;

    if (ks.enter && !prevEnter) {
        prevEnter = true;
        finishEdit();
        return;
    }
    prevEnter = ks.enter;

    if (changed) dirty = true;
}

void handleProfile(uint32_t pressed)
{
    if (pressed & kBack) {
        launcher::rebootInto(launcher::kAppMenu);
        return;
    }
    if (pressed & kUp) {
        sel   = (sel + 4) % 5;
        dirty = true;
    }
    if (pressed & kDown) {
        sel   = (sel + 1) % 5;
        dirty = true;
    }
    if (!(pressed & kEnter)) return;
    if (sel == 4) {
        startWifiForSsh();
    } else {
        beginEdit(sel);
    }
}

void handleTerminal()
{
    const auto& ks = M5Cardputer.Keyboard.keysState();

    if (ks.fn && ks.del) {
        stopSsh();
        return;
    }

    // Fn+Q closes the session but leaves the SSH app open.
    for (char c : ks.word) {
        if (ks.fn && (c == 'q' || c == 'Q')) {
            stopSsh();
            return;
        }
    }

    if (ks.enter && !prevEnter) sendKeyByte('\r');
    if (ks.del && !prevDel) sendKeyByte(0x7f);
    if (ks.tab && !prevTab) sendKeyByte('\t');
    prevTab = ks.tab;

    uint8_t arrow = 0;
    if (ks.fn) {
        for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
            const uint8_t code =
                static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
            if (code == ';') arrow = 'A';
            else if (code == '.') arrow = 'B';
            else if (code == ',') arrow = 'D';
            else if (code == '/') arrow = 'C';
        }
    }
    if (arrow != 0 && arrow != prevArrow) {
        sendKeyByte(0x1b);
        sendKeyByte('[');
        sendKeyByte(arrow);
    }
    prevArrow = arrow;

    for (char c : ks.word) {
        bool already = false;
        for (char p : prevWord) {
            if (p == c) {
                already = true;
                break;
            }
        }
        if (already) continue;

        if (c == '`') {
            sendKeyByte(0x1b);
            continue;
        }
        if (ks.fn && (c == ';' || c == '.' || c == ',' || c == '/')) continue;
        if (ks.fn && (c == 'q' || c == 'Q')) continue;
        if (c >= 32 && c < 127) {
            if (ks.ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                sendKeyByte(static_cast<uint8_t>(c & 0x1f));
            } else {
                sendKeyByte(static_cast<uint8_t>(c));
            }
        }
    }
    prevWord.assign(ks.word.begin(), ks.word.end());
    prevEnter = ks.enter;
    prevDel   = ks.del;
}

void handleConnecting(uint32_t pressed)
{
    if (pressed & kBack) stopSsh();
}

void handleError(uint32_t pressed)
{
    if (pressed & kBack) {
        launcher::rebootInto(launcher::kAppMenu);
        return;
    }
    if (pressed & kEnter) {
        scr   = Scr::kProfile;
        dirty = true;
    }
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
    lcd.setBrightness(90);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    lcd.fillScreen(kBg);

    libssh_begin();
    loadProfile();

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    wifistore::begin();
    if (wifistore::count() > 0) {
        const auto* n = wifistore::last();
        if (n != nullptr) {
            WiFi.begin(n->ssid, n->pass);
            wifiConnecting = true;
            wifiStart      = millis();
            wifiForSsh     = false;
            Serial.printf("[ssh] wifi auto '%s'\n", n->ssid);
        }
    }

    scr   = Scr::kProfile;
    dirty = true;
}

void loop()
{
    M5Cardputer.update();
    pollWifi();

    // The task flips phases asynchronously; the UI only reacts to transitions.
    if (sshPhase == 3 && scr != Scr::kTerminal) {
        scr   = Scr::kTerminal;
        dirty = true;
        resetEditEdges();
        prevArrow = 0;
        prevTab   = false;
    } else if (sshPhase == 4) {
        copyCString(note, sizeof(note), sshError[0] ? sshError : "SSH FAILED");
        sshPhase = 0;
        scr      = Scr::kError;
        dirty    = true;
    } else if (sshPhase == 5) {
        sshPhase = 0;
        if (scr == Scr::kTerminal || scr == Scr::kConnecting) {
            copyCString(note, sizeof(note), "SESSION CLOSED");
            scr   = Scr::kProfile;
            dirty = true;
        }
    }

    const uint32_t a       = readActions();
    const uint32_t pressed = a & ~prevActions;
    prevActions            = a;

    switch (scr) {
        case Scr::kProfile:
            handleProfile(pressed);
            break;
        case Scr::kEdit:
            handleEdit();
            break;
        case Scr::kWifiWait:
            if (pressed & kBack) {
                wifiConnecting = false;
                wifiForSsh     = false;
                scr            = Scr::kProfile;
                dirty          = true;
            }
            break;
        case Scr::kConnecting:
            handleConnecting(pressed);
            break;
        case Scr::kTerminal:
            handleTerminal();
            if (sshterm::takeDirty()) dirty = true;
            break;
        case Scr::kError:
            handleError(pressed);
            break;
    }

    if (dirty) {
        dirty = false;
        drawCurrent();
    }
    delay(5);
}

}  // namespace sshapp
