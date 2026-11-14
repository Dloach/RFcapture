#include "ssh_app.h"

// Optional: set to 1 to compile the TF-imported Ed25519 key path back into the
// UI and authentication flow.  The code stays in the repository either way.
#ifndef SSH_ENABLE_ED25519
#define SSH_ENABLE_ED25519 0
#endif

#include <M5Cardputer.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>

#include <cstring>
#include <vector>

#include "libssh_esp32.h"
#include <libssh/libssh.h>
#include <libssh/server.h>

#if SSH_ENABLE_ED25519
#include <SD.h>
#include "sd_mount.h"
#endif

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
    char name[20] = {0};
    char host[64] = {0};
    char user[32] = {0};
    char pass[64] = {0};
    char keyPath[48] = "/spiffs/ssh/id_ed25519";
    uint16_t port = 22;
    uint8_t auth = 0;  // 0 password, 1 Ed25519 key
};

constexpr int kMaxProfiles = 4;
Profile profiles[kMaxProfiles];
int profileCount = 0;
int current      = 0;
bool profileLoaded = false;

enum class Scr : uint8_t {
    kList,
    kProfile,
    kEdit,
    kWifiWait,
    kConnecting,
    kTerminal,
    kError,
    kHostKey,
};
Scr scr = Scr::kList;
bool dirty = true;
int sel = 0;
int listSel = 0;
bool deleteArmed = false;
uint32_t deleteAt = 0;

char note[64] = {0};
bool wifiConnecting = false;
uint32_t wifiStart = 0;
bool wifiForSsh = false;
int termScroll = 0;
uint8_t prevScroll = 0;

// SSH task state is written by the task and polled by the UI.
volatile int sshPhase = 0;  // 0 idle, 1 TCP, 2 auth, 3 shell, 4 failed, 5 closed
volatile bool sshRunning = false;
volatile bool sshStop = false;
char sshError[64] = {0};
QueueHandle_t keyQ = nullptr;
bool rxDumped = false;

char connectHost[64] = {0};
char connectUser[32] = {0};
char connectPass[64] = {0};
char connectKeyPath[48] = {0};
uint16_t connectPort = 22;
uint8_t connectAuth = 0;

// Host-key confirmation, written by the SSH task and answered by the UI.
char hostKeyFingerprint[65] = {0};
char hostKeyHost[64] = {0};
volatile uint8_t hostKeyDecision = 0;  // 0 wait, 1 trust, 2 reject

// Editing.
int editField = 0;
char editBuf[64] = {0};
int editLen = 0;
int editPos = 0;
bool editCursorOn = true;
uint32_t editCursorMs = 0;
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
// Profile persistence: NVS holds up to four profiles.  The first version
// stored a single host/user/pass/port; loadProfiles() migrates that into
// profile 0 so an existing SSH setup survives the upgrade.
// ---------------------------------------------------------------------------
Profile* currentProfile()
{
    if (profileCount <= 0) return nullptr;
    if (current < 0 || current >= profileCount) current = 0;
    return &profiles[current];
}

void loadProfiles()
{
    if (profileLoaded) return;
    profileLoaded = true;
    current       = 0;
    Preferences p;
    if (!p.begin("ssh", true)) return;

    int n = p.getUChar("count", 0);
    if (n <= 0) {
        char host[64];
        copyString(host, sizeof(host), p.getString("host", ""));
        if (host[0] != 0) {
            copyCString(profiles[0].name, sizeof(profiles[0].name), "DEFAULT");
            copyString(profiles[0].host, sizeof(profiles[0].host), host);
            copyString(profiles[0].user, sizeof(profiles[0].user), p.getString("user", ""));
            copyString(profiles[0].pass, sizeof(profiles[0].pass), p.getString("pass", ""));
            profiles[0].port = p.getUShort("port", 22);
            if (profiles[0].port == 0) profiles[0].port = 22;
            profiles[0].auth = p.getUChar("auth", 0);
            copyString(profiles[0].keyPath, sizeof(profiles[0].keyPath),
                       p.getString("key", "/spiffs/ssh/id_ed25519"));
            profileCount = 1;
        }
        p.end();
        return;
    }

    if (n > kMaxProfiles) n = kMaxProfiles;
    for (int i = 0; i < n; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "name%d", i);
        copyString(profiles[i].name, sizeof(profiles[i].name), p.getString(key, ""));
        snprintf(key, sizeof(key), "host%d", i);
        copyString(profiles[i].host, sizeof(profiles[i].host), p.getString(key, ""));
        snprintf(key, sizeof(key), "user%d", i);
        copyString(profiles[i].user, sizeof(profiles[i].user), p.getString(key, ""));
        snprintf(key, sizeof(key), "pass%d", i);
        copyString(profiles[i].pass, sizeof(profiles[i].pass), p.getString(key, ""));
        snprintf(key, sizeof(key), "port%d", i);
        profiles[i].port = p.getUShort(key, 22);
        if (profiles[i].port == 0) profiles[i].port = 22;
        snprintf(key, sizeof(key), "auth%d", i);
        profiles[i].auth = p.getUChar(key, 0);
        snprintf(key, sizeof(key), "key%d", i);
        copyString(profiles[i].keyPath, sizeof(profiles[i].keyPath),
                   p.getString(key, "/spiffs/ssh/id_ed25519"));
        if (profiles[i].name[0] == 0 && profiles[i].host[0] == 0) {
            // Torn write: count was already new but this slot never landed.
            // Compact it away, same policy as wifi_store.
            for (int j = i; j < n - 1; ++j) profiles[j] = profiles[j + 1];
            --n;
            --i;
            continue;
        }
        if (profiles[i].name[0] == 0) snprintf(profiles[i].name, sizeof(profiles[i].name), "PROFILE %d", i + 1);
    }
    profileCount = n;
    current      = p.getUChar("current", 0);
    if (current < 0 || current >= profileCount) current = 0;
    if (current < 0 || current >= profileCount) current = 0;
    p.end();
}

void saveProfiles()
{
    Preferences p;
    if (!p.begin("ssh", false)) return;
    for (int i = 0; i < kMaxProfiles; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "name%d", i);
        p.putString(key, i < profileCount ? profiles[i].name : "");
        snprintf(key, sizeof(key), "host%d", i);
        p.putString(key, i < profileCount ? profiles[i].host : "");
        snprintf(key, sizeof(key), "user%d", i);
        p.putString(key, i < profileCount ? profiles[i].user : "");
        snprintf(key, sizeof(key), "pass%d", i);
        p.putString(key, i < profileCount ? profiles[i].pass : "");
        snprintf(key, sizeof(key), "port%d", i);
        p.putUShort(key, i < profileCount ? profiles[i].port : 0);
        snprintf(key, sizeof(key), "auth%d", i);
        p.putUChar(key, i < profileCount ? profiles[i].auth : 0);
        snprintf(key, sizeof(key), "key%d", i);
        p.putString(key, i < profileCount ? profiles[i].keyPath : "");
    }
    p.putUChar("current", static_cast<uint8_t>(current < 0 ? 0 : current));
    // Commit count last: a torn write leaves the old count, and loadProfiles()
    // compacts any empty slot if the new count did land.
    p.putUChar("count", static_cast<uint8_t>(profileCount));
    p.end();
    Serial.printf("[ssh] profiles saved count=%d current=%d\n", profileCount, current);
}

bool addProfile()
{
    if (profileCount >= kMaxProfiles) return false;
    Profile& p = profiles[profileCount];
    p = Profile();
    snprintf(p.name, sizeof(p.name), "PROFILE %d", profileCount + 1);
    p.port = 22;
    current = profileCount++;
    saveProfiles();
    return true;
}

void deleteProfile(int index)
{
    if (index < 0 || index >= profileCount) return;
    for (int i = index; i < profileCount - 1; ++i) profiles[i] = profiles[i + 1];
    --profileCount;
    profiles[profileCount] = Profile();
    if (current >= profileCount) current = profileCount > 0 ? profileCount - 1 : 0;
    if (current < 0) current = 0;
    saveProfiles();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void drawList()
{
    lcd.fillScreen(kBg);
    header("SSH");

    const bool wifi = WiFi.status() == WL_CONNECTED;
    lcd.setTextColor(wifi ? kOk : (wifiConnecting ? kAccent : kDim), kBg);
    lcd.drawString(wifi ? "WIFI CONNECTED" : (wifiConnecting ? "WIFI CONNECTING..." : "WIFI OFF"), 6,
                   16);

    const int total = profileCount + (profileCount < kMaxProfiles ? 1 : 0);
    for (int i = 0; i < total; ++i) {
        const int y = 30 + i * 15;
        if (i == listSel) lcd.fillRect(2, y - 2, lcd.width() - 4, 13, kBar);
        const uint16_t bg = i == listSel ? kBar : kBg;
        const uint16_t fg = i == listSel ? kAccent : kText;
        lcd.setTextColor(fg, bg);
        if (i < profileCount) {
            char line[28];
            snprintf(line, sizeof(line), "%c %.18s", i == current ? '*' : ' ', profiles[i].name);
            lcd.drawString(line, 6, y);
        } else {
            lcd.drawString("+ NEW PROFILE", 6, y);
        }
    }

    if (note[0]) {
        lcd.setTextColor(kWarn, kBg);
        char line[40];
        snprintf(line, sizeof(line), "%.38s", note);
        lcd.drawString(line, 6, 104);
    }

#if SSH_ENABLE_ED25519
    static const Hint h[] = {{"ENT", "connect"}, {"E", "edit"}, {"D", "del"}, {"K", "key"}, {"esc", "back"}};
    hints(h, 5);
#else
    static const Hint h[] = {{"ENT", "connect"}, {"E", "edit"}, {"D", "del"}, {"esc", "back"}};
    hints(h, 4);
#endif
}

void drawProfile()
{
    lcd.fillScreen(kBg);
    header("SSH PROFILE");

    Profile* p = currentProfile();
    if (p == nullptr) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("No profile. Go back and choose NEW PROFILE.", 6, 40);
        static const Hint h[] = {{"esc", "back"}};
        hints(h, 1);
        return;
    }

#if SSH_ENABLE_ED25519
    const int rows = 7;
#else
    const int rows = 6;
#endif
    for (int i = 0; i < rows; ++i) {
        const int y = 20 + i * 12;
        if (i == sel) lcd.fillRect(2, y - 2, lcd.width() - 4, 11, kBar);
        const uint16_t bg = i == sel ? kBar : kBg;
        const uint16_t fg = i == sel ? kAccent : kText;
        lcd.setTextColor(fg, bg);
        if (i == 0) {
            lcd.drawString("NAME", 6, y);
            char val[24];
            snprintf(val, sizeof(val), "%.18s", p->name[0] ? p->name : "(empty)");
            lcd.drawString(val, 58, y);
        } else if (i == 1) {
            lcd.drawString("HOST", 6, y);
            char val[30];
            snprintf(val, sizeof(val), "%.26s", p->host[0] ? p->host : "(empty)");
            lcd.drawString(val, 58, y);
        } else if (i == 2) {
            char port[8];
            snprintf(port, sizeof(port), "%u", static_cast<unsigned>(p->port));
            lcd.drawString("PORT", 6, y);
            lcd.drawString(port, 58, y);
        } else if (i == 3) {
            lcd.drawString("USER", 6, y);
            char val[30];
            snprintf(val, sizeof(val), "%.26s", p->user[0] ? p->user : "(empty)");
            lcd.drawString(val, 58, y);
#if SSH_ENABLE_ED25519
        } else if (i == 4) {
            lcd.drawString("AUTH", 6, y);
            lcd.drawString(p->auth == 1 ? "ED25519 KEY" : "PASSWORD", 58, y);
        } else if (i == 5) {
            if (p->auth == 1) {
                lcd.drawString("KEY", 6, y);
                lcd.drawString("ED25519", 58, y);
            } else {
                char stars[24];
                const int n = strlen(p->pass);
                const int show = n < 20 ? n : 20;
                for (int k = 0; k < show; ++k) stars[k] = '*';
                if (show == 20) stars[19] = '+';
                stars[show] = 0;
                lcd.drawString("PASS", 6, y);
                lcd.drawString(p->pass[0] ? stars : "(empty)", 58, y);
            }
        } else {
            lcd.setTextColor(kOk, bg);
            lcd.drawString("CONNECT", 6, y);
            lcd.setTextColor(kDim, bg);
            lcd.drawString("ENTER", 80, y);
        }
#else
        } else if (i == 4) {
            char stars[24];
            const int n = strlen(p->pass);
            const int show = n < 20 ? n : 20;
            for (int k = 0; k < show; ++k) stars[k] = '*';
            if (show == 20) stars[19] = '+';
            stars[show] = 0;
            lcd.drawString("PASS", 6, y);
            lcd.drawString(p->pass[0] ? stars : "(empty)", 58, y);
        } else {
            lcd.setTextColor(kOk, bg);
            lcd.drawString("CONNECT", 6, y);
            lcd.setTextColor(kDim, bg);
            lcd.drawString("ENTER", 80, y);
        }
#endif
    }

    static const Hint h[] = {{"; .", "move"}, {"ENT", "edit/connect"}, {"esc", "list"}};
    hints(h, 3);
    if (note[0]) {
        lcd.setTextColor(kWarn, kBg);
        char line[40];
        snprintf(line, sizeof(line), "%.38s", note);
        lcd.drawString(line, 6, 106);
    }
}

void drawEditLine()
{
    char shown[44];
    int n      = 0;
    int start  = 0;
    const int maxShow = 34;
    if (editPos > maxShow - 2) start = editPos - (maxShow - 2);
    for (int i = start; i <= editLen && n < (int)sizeof(shown) - 1; ++i) {
        if (i == editPos) shown[n++] = editCursorOn ? '_' : ' ';
        if (i < editLen && n < (int)sizeof(shown) - 1) {
            shown[n++] = (editField == 4) ? '*' : editBuf[i];
        }
    }
    shown[n] = 0;

    lcd.fillRect(0, 40, lcd.width(), 24, kBg);
    lcd.setTextColor(kText, kBg);
    lcd.drawString(shown, 6, 42);
}

void drawEdit()
{
    lcd.fillScreen(kBg);
#if SSH_ENABLE_ED25519
    const char* titles[] = {"EDIT NAME", "EDIT HOST", "EDIT PORT", "EDIT USER",
                            "EDIT PASSWORD", "EDIT KEY PATH"};
#else
    const char* titles[] = {"EDIT NAME", "EDIT HOST", "EDIT PORT", "EDIT USER",
                            "EDIT PASSWORD"};
#endif
    header(titles[editField]);
    drawEditLine();

    char count[32];
    snprintf(count, sizeof(count), "LEN %d  POS %d", editLen, editPos);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(count, 6, 64);
    if (editField == 2) lcd.drawString("1-65535", 6, 78);
    if (note[0]) {
        lcd.setTextColor(kWarn, kBg);
        char line[40];
        snprintf(line, sizeof(line), "%.38s", note);
        lcd.drawString(line, 6, 94);
    }

    static const Hint h[] = {{"ENT", "save"}, {"OPT , /", "cursor"}, {"FN BKSP", "cancel"}};
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

    static const uint16_t palette[16] = {
        0x0000, 0xF800, 0x07E0, 0xFFE0, 0x001F, 0xF81F, 0x07FF, 0xFFFF,
        0x8410, 0xF800, 0x07E0, 0xFFE0, 0x001F, 0xF81F, 0x07FF, 0xFFFF,
    };

    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, termScroll);
    for (int r = 0; r < sshterm::kRows; ++r) {
        const int y = 14 + r * 8;
        lcd.fillRect(0, y, lcd.width(), 8, kBg);
        for (int c = 0; c < sshterm::kCols; ++c) {
            const sshterm::Cell& cell = snap[r][c];
            const uint16_t fg = palette[cell.fg & 15];
            const uint16_t bg = palette[cell.bg & 15];
            const int x = c * 6;
            if (bg != kBg) lcd.fillRect(x, y, 6, 8, bg);
            // drawString() is used instead of drawChar(): the latter rendered
            // high/control cell bytes as extended font glyphs on this panel.
            char one[2];
            one[0] = (cell.ch >= 32 && cell.ch <= 126) ? cell.ch : ' ';
            one[1] = 0;
            lcd.setTextColor(fg, bg);
            lcd.drawString(one, x, y);
        }
    }

    static const Hint h[] = {{"FN BKSP", "back"}, {"FN Q", "close"}, {"OPT ; .", "scroll"}};
    hints(h, 3);
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
    lcd.drawString("ENT list   R retry   K clear keys", 6, 76);

    static const Hint h[] = {{"ENT", "list"}, {"R", "retry"}, {"K", "keys"}, {"esc", "launcher"}};
    hints(h, 4);
}

void drawHostKey()
{
    lcd.fillScreen(kBg);
    header("HOST KEY");
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString("NEW SSH HOST", 6, 22);

    lcd.setTextColor(kText, kBg);
    char host[30];
    snprintf(host, sizeof(host), "%.28s", hostKeyHost);
    lcd.drawString(host, 6, 38);

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("SHA256", 6, 54);
    char a[33];
    char b[33];
    memcpy(a, hostKeyFingerprint, 32);
    a[32] = 0;
    memcpy(b, hostKeyFingerprint + 32, 32);
    b[32] = 0;
    lcd.setTextColor(kText, kBg);
    lcd.drawString(a, 6, 66);
    lcd.drawString(b, 6, 78);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Trust this host key?", 6, 96);

    static const Hint h[] = {{"ENT", "trust"}, {"esc", "reject"}};
    hints(h, 2);
}

void drawCurrent()
{
    switch (scr) {
        case Scr::kList: drawList(); break;
        case Scr::kProfile: drawProfile(); break;
        case Scr::kEdit: drawEdit(); break;
        case Scr::kWifiWait: drawWifiWait(); break;
        case Scr::kConnecting: drawConnecting(); break;
        case Scr::kTerminal: drawTerminal(); break;
        case Scr::kError: drawError(); break;
        case Scr::kHostKey: drawHostKey(); break;
    }
}

// Host-key trust is kept in a firmware-owned file instead of relying on
// libssh's known_hosts path check: on this Arduino/SPIFFS VFS `access()` can
// report a present file as missing, which made every connection re-prompt.
constexpr char kHostKeyStore[] = "/ssh/hostkeys.txt";
enum HostKeyLookup : uint8_t { kHostKeyMissing = 0, kHostKeyMatch = 1, kHostKeyChanged = 2 };

int lookupHostKey(const char* host, uint16_t port, const char* fingerprint)
{
    if (host == nullptr || fingerprint == nullptr) return kHostKeyMissing;
    File f = SPIFFS.open(kHostKeyStore, FILE_READ);
    if (!f) return kHostKeyMissing;
    int result = kHostKeyMissing;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        char h[64];
        unsigned p = 0;
        char fp[65];
        if (sscanf(line.c_str(), "%63s %u %64s", h, &p, fp) == 3 && strcmp(h, host) == 0 &&
            p == port) {
            result = strcmp(fp, fingerprint) == 0 ? kHostKeyMatch : kHostKeyChanged;
            break;
        }
    }
    f.close();
    return result;
}

bool saveHostKey(const char* host, uint16_t port, const char* fingerprint)
{
    if (host == nullptr || fingerprint == nullptr) return false;
    File f;
    if (SPIFFS.exists(kHostKeyStore)) {
        f = SPIFFS.open(kHostKeyStore, FILE_APPEND);
    } else {
        f = SPIFFS.open(kHostKeyStore, FILE_WRITE);
    }
    if (!f) return false;
    f.printf("%s %u %s\n", host, static_cast<unsigned>(port), fingerprint);
    f.close();
    return true;
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
    uint32_t lastKeepalive = 0;
    uint32_t lastStats     = 0;
    char buf[512];
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
    long timeout = 15;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    if (!SPIFFS.begin(true) || !SPIFFS.exists("/ssh")) {
        if (!SPIFFS.mkdir("/ssh")) {
            setError("SPIFFS /ssh unavailable");
            goto done;
        }
    }
    sshPhase = 1;
    if (ssh_connect(session) != SSH_OK) {
        char msg[48];
        snprintf(msg, sizeof(msg), "TCP: %.30s", ssh_get_error(session));
        setError(msg);
        goto done;
    }
    if (sshStop) {
        sshPhase = 5;
        goto done;
    }

    {
        ssh_key serverKey   = nullptr;
        unsigned char* hash = nullptr;
        size_t hashLen      = 0;
        if (ssh_get_server_publickey(session, &serverKey) != SSH_OK ||
            ssh_get_publickey_hash(serverKey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen) != SSH_OK ||
            hash == nullptr || hashLen < 32) {
            if (hash != nullptr) ssh_clean_pubkey_hash(&hash);
            if (serverKey != nullptr) ssh_key_free(serverKey);
            setError("HOST KEY HASH FAILED");
            goto done;
        }
        for (int i = 0; i < 32; ++i) snprintf(&hostKeyFingerprint[i * 2], 3, "%02x", hash[i]);
        hostKeyFingerprint[64] = 0;
        copyCString(hostKeyHost, sizeof(hostKeyHost), connectHost);
        ssh_clean_pubkey_hash(&hash);
        ssh_key_free(serverKey);

        if (sshStop) {
            sshPhase = 5;
            goto done;
        }

        const int stored = lookupHostKey(connectHost, connectPort, hostKeyFingerprint);
        if (stored == kHostKeyChanged) {
            setError("HOST KEY CHANGED");
            goto done;
        }
        if (stored == kHostKeyMissing) {
            hostKeyDecision = 0;
            sshPhase        = 6;
            while (hostKeyDecision == 0 && !sshStop) vTaskDelay(pdMS_TO_TICKS(50));
            if (sshStop || hostKeyDecision != 1) {
                if (hostKeyDecision == 2) sshPhase = 5;
                else setError("HOST KEY REJECTED");
                goto done;
            }
            if (!saveHostKey(connectHost, connectPort, hostKeyFingerprint)) {
                setError("HOSTKEY SAVE FAILED");
                goto done;
            }
            Serial.printf("[ssh] host key trusted %s:%u\n", connectHost,
                          static_cast<unsigned>(connectPort));
        }
    }

    sshPhase = 2;
#if SSH_ENABLE_ED25519
    if (connectAuth == 1) {
        ssh_key key = nullptr;
        if (ssh_pki_import_privkey_file(connectKeyPath, nullptr, nullptr, nullptr, &key) != SSH_OK) {
            setError("KEY IMPORT FAILED");
            goto done;
        }
        const int rc = ssh_userauth_publickey(session, connectUser, key);
        ssh_key_free(key);
        if (rc != SSH_AUTH_SUCCESS) {
            char msg[48];
            snprintf(msg, sizeof(msg), "KEY AUTH: %.26s", ssh_get_error(session));
            setError(msg);
            goto done;
        }
    } else
#endif
    if (ssh_userauth_password(session, nullptr, connectPass) != SSH_AUTH_SUCCESS) {
        char msg[48];
        snprintf(msg, sizeof(msg), "AUTH: %.30s", ssh_get_error(session));
        setError(msg);
        goto done;
    }
    if (sshStop) {
        sshPhase = 5;
        goto done;
    }

    channel = ssh_channel_new(session);
    if (channel == nullptr || ssh_channel_open_session(channel) != SSH_OK) {
        setError("CHANNEL OPEN FAILED");
        goto done;
    }
    if (sshStop) {
        sshPhase = 5;
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
    if (sshStop) {
        sshPhase = 5;
        goto done;
    }

    sshterm::clear();
    sshPhase = 3;
    Serial.printf("[ssh] shell open %s@%s:%u heap=%u stack=%u\n", connectUser, connectHost,
                  static_cast<unsigned>(connectPort), static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    lastKeepalive = millis();
    lastStats     = millis();
    while (!sshStop) {
        uint8_t key;
        while (keyQ != nullptr && xQueueReceive(keyQ, &key, 0) == pdTRUE) {
            ssh_channel_write(channel, &key, 1);
        }
        const int n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0);
        if (n > 0) {
            if (!rxDumped) {
                rxDumped = true;
                const int dump = n < 128 ? n : 128;
                Serial.printf("[ssh] first RX %d bytes:", n);
                for (int i = 0; i < dump; ++i) {
                    Serial.printf(" %02X", static_cast<unsigned char>(buf[i]));
                }
                Serial.println();
            }
            sshterm::feed(buf, n);
        } else if (n < 0 && n != SSH_AGAIN) {
            Serial.printf("[ssh] read error %d: %s\n", n, ssh_get_error(session));
            break;
        }
        if (ssh_channel_is_eof(channel)) break;

        if (millis() - lastKeepalive >= 30000) {
            lastKeepalive = millis();
            if (ssh_send_keepalive(session) != SSH_OK) {
                Serial.println("[ssh] keepalive failed");
                break;
            }
        }
        if (millis() - lastStats >= 30000) {
            lastStats = millis();
            Serial.printf("[ssh] heap=%u stack=%u\n", static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
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
    Profile* p = currentProfile();
    if (p == nullptr || p->host[0] == 0) {
        copyCString(note, sizeof(note), "HOST IS EMPTY");
        scr   = Scr::kError;
        dirty = true;
        return false;
    }

    copyCString(connectHost, sizeof(connectHost), p->host);
    copyCString(connectUser, sizeof(connectUser), p->user);
    copyCString(connectPass, sizeof(connectPass), p->pass);
    connectPort = p->port;
#if SSH_ENABLE_ED25519
    copyCString(connectKeyPath, sizeof(connectKeyPath), p->keyPath);
    connectAuth = p->auth;
#endif
    sshError[0] = 0;
    sshStop     = false;
    sshPhase    = 1;
    hostKeyDecision = 0;
    rxDumped    = false;
    sshterm::clear();

    if (keyQ == nullptr) keyQ = xQueueCreate(128, sizeof(uint8_t));
    if (keyQ == nullptr) {
        copyCString(note, sizeof(note), "NO MEMORY (key queue)");
        scr   = Scr::kError;
        dirty = true;
        return false;
    }

    sshRunning = true;
    const BaseType_t ok =
        xTaskCreatePinnedToCore(sshTask, "ssh", 56 * 1024, nullptr, 5, nullptr, 1);
    if (ok != pdPASS) {
        sshRunning = false;
        copyCString(note, sizeof(note), "NO MEMORY (ssh task)");
        scr   = Scr::kError;
        dirty = true;
        return false;
    }

    scr        = Scr::kConnecting;
    dirty      = true;
    return true;
}

void stopSsh()
{
    sshStop  = true;
    sshPhase = 0;
    hostKeyDecision = 2;
    scr      = Scr::kList;
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
    WiFi.setSleep(false);
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
    kUp      = 1u << 0,
    kDown    = 1u << 1,
    kEnter   = 1u << 2,
    kBack    = 1u << 3,
    kDelete  = 1u << 4,
    kEdit    = 1u << 6,
    kDelProf = 1u << 7,
    kLeft    = 1u << 8,
    kRight   = 1u << 9,
    kClear   = 1u << 10,
    kRetry   = 1u << 11,
};

uint32_t readActions()
{
    uint32_t a = 0;
    const bool opt = M5Cardputer.Keyboard.keysState().opt;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            case ';': a |= kUp; break;
            case '.': a |= kDown; break;
            case KEY_ENTER: a |= kEnter; break;
            case '`': case '~': case 0x1B: a |= kBack; break;
            case 0x2A: a |= kDelete; break;
            case 'e': case 'E': a |= kEdit; break;
            case 'd': case 'D': a |= kDelProf; break;
            case ',': if (opt) a |= kLeft; break;
            case '/': if (opt) a |= kRight; break;
            case 'k': case 'K': a |= kClear; break;
            case 'r': case 'R': a |= kRetry; break;
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
    Profile* p = currentProfile();
    if (p == nullptr) return;
    editField = field;
    editLen   = 0;
    editBuf[0] = 0;
    if (field == 2) {
        snprintf(editBuf, sizeof(editBuf), "%u", static_cast<unsigned>(p->port));
    } else {
        const char* src = field == 0 ? p->name
                          : field == 1 ? p->host
                          : field == 3 ? p->user
#if SSH_ENABLE_ED25519
                          : field == 4 ? p->pass
                                       : p->keyPath;
#else
                          : p->pass;
#endif
        copyCString(editBuf, sizeof(editBuf), src);
    }
    editLen = strlen(editBuf);
    editPos = editLen;
    editCursorOn  = true;
    editCursorMs  = millis();
    note[0]       = 0;
    resetEditEdges();
    // The ENTER that opened this page is still held on the next loop; require
    // a release before it can be interpreted as "save".
    prevEnter = true;
    scr   = Scr::kEdit;
    dirty = true;
}

void finishEdit()
{
    Profile* p = currentProfile();
    if (p == nullptr) {
        scr = Scr::kList;
        return;
    }
    if (editField == 2) {
        const long v = atol(editBuf);
        if (v < 1 || v > 65535) {
            copyCString(note, sizeof(note), "PORT 1-65535");
            dirty = true;
            return;
        }
        p->port = static_cast<uint16_t>(v);
    } else if (editField == 0) {
        copyCString(p->name, sizeof(p->name), editBuf);
    } else if (editField == 1) {
        copyCString(p->host, sizeof(p->host), editBuf);
    } else if (editField == 3) {
        copyCString(p->user, sizeof(p->user), editBuf);
    } else if (editField == 4) {
        copyCString(p->pass, sizeof(p->pass), editBuf);
#if SSH_ENABLE_ED25519
    } else {
        copyCString(p->keyPath, sizeof(p->keyPath), editBuf);
#endif
    }
    saveProfiles();
    scr   = Scr::kProfile;
    dirty = true;
}

void handleEdit(uint32_t pressed)
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
        if (ks.opt && (c == ',' || c == '/')) continue;  // Opt cursor movement
        if (editField == 2 && (c < '0' || c > '9')) continue;
        bool already = false;
        for (char p : prevWord) {
            if (p == c) {
                already = true;
                break;
            }
        }
        if (!already && editLen < (int)sizeof(editBuf) - 1) {
            memmove(&editBuf[editPos + 1], &editBuf[editPos],
                    static_cast<size_t>(editLen - editPos + 1));
            editBuf[editPos++] = c;
            ++editLen;
            changed = true;
        }
    }
    prevWord.assign(ks.word.begin(), ks.word.end());

    if (ks.del && !prevDel && editPos > 0) {
        memmove(&editBuf[editPos - 1], &editBuf[editPos],
                static_cast<size_t>(editLen - editPos + 1));
        --editPos;
        --editLen;
        changed = true;
    }
    prevDel = ks.del;

    if (pressed & kLeft && editPos > 0) {
        --editPos;
        changed = true;
    }
    if (pressed & kRight && editPos < editLen) {
        ++editPos;
        changed = true;
    }

    if (changed) {
        editCursorOn = true;
        editCursorMs = millis();
        dirty        = true;
    }

    if (ks.enter && !prevEnter) {
        prevEnter = true;
        finishEdit();
        return;
    }
    prevEnter = ks.enter;
}

#if SSH_ENABLE_ED25519
void importKeyFromSd()
{
    if (!sdmount::ensure()) {
        copyCString(note, sizeof(note), "NO TF CARD");
        dirty = true;
        return;
    }
    File src = SD.open("/ssh/id_ed25519", FILE_READ);
    if (!src) src = SD.open("/id_ed25519", FILE_READ);
    if (!src) {
        copyCString(note, sizeof(note), "PUT KEY AT /ssh/id_ed25519");
        dirty = true;
        return;
    }

    SPIFFS.begin(true);
    if (!SPIFFS.exists("/ssh")) SPIFFS.mkdir("/ssh");
    File dst = SPIFFS.open("/ssh/id_ed25519", FILE_WRITE);
    if (!dst) {
        src.close();
        copyCString(note, sizeof(note), "SPIFFS WRITE FAILED");
        dirty = true;
        return;
    }
    uint8_t buf[256];
    while (src.available()) {
        const int n = src.read(buf, sizeof(buf));
        if (n <= 0) break;
        dst.write(buf, n);
    }
    dst.close();
    src.close();

    // The public half is optional; copy it when present.
    File pubSrc = SD.open("/ssh/id_ed25519.pub", FILE_READ);
    if (pubSrc) {
        File pubDst = SPIFFS.open("/ssh/id_ed25519.pub", FILE_WRITE);
        if (pubDst) {
            while (pubSrc.available()) {
                const int n = pubSrc.read(buf, sizeof(buf));
                if (n <= 0) break;
                pubDst.write(buf, n);
            }
            pubDst.close();
        }
        pubSrc.close();
    }

    if (currentProfile() == nullptr) addProfile();
    Profile* p = currentProfile();
    if (p != nullptr) {
        p->auth = 1;
        copyCString(p->keyPath, sizeof(p->keyPath), "/spiffs/ssh/id_ed25519");
        saveProfiles();
    }
    copyCString(note, sizeof(note), "KEY IMPORTED FROM TF");
    Serial.println("[ssh] imported /ssh/id_ed25519 from TF");
    dirty = true;
}
#endif

void handleList(uint32_t pressed)
{
    const int total = profileCount + (profileCount < kMaxProfiles ? 1 : 0);
    if (pressed & kBack) {
        launcher::rebootInto(launcher::kAppMenu);
        return;
    }
    if (pressed & kUp) {
        listSel = (listSel + total - 1) % total;
        dirty   = true;
    }
    if (pressed & kDown) {
        listSel = (listSel + 1) % total;
        dirty   = true;
    }
    if (pressed & kEdit) {
        if (listSel < profileCount) {
            current = listSel;
            sel     = 0;
            scr     = Scr::kProfile;
            note[0] = 0;
            dirty   = true;
        }
        return;
    }
    if (pressed & kDelProf) {
        if (listSel >= profileCount) return;
        if (!deleteArmed) {
            deleteArmed = true;
            deleteAt    = millis();
            copyCString(note, sizeof(note), "PRESS D AGAIN TO DELETE");
            dirty = true;
            return;
        }
        deleteProfile(listSel);
        listSel = profileCount > 0 ? (listSel < profileCount ? listSel : profileCount - 1) : 0;
        deleteArmed = false;
        note[0]     = 0;
        dirty       = true;
        return;
    }
#if SSH_ENABLE_ED25519
    if (pressed & kClear) {
        importKeyFromSd();
        return;
    }
#endif
    if (pressed & kRetry) {
        if (listSel < profileCount && !sshRunning) {
            current = listSel;
            startWifiForSsh();
        }
        return;
    }
    if (!(pressed & kEnter)) return;

    if (listSel >= profileCount) {
        if (addProfile()) {
            listSel = current;
            sel     = 0;
            scr     = Scr::kProfile;
        } else {
            copyCString(note, sizeof(note), "MAX 4 PROFILES");
        }
        dirty = true;
        return;
    }
    current = listSel;
    note[0] = 0;
    startWifiForSsh();
}

void handleProfile(uint32_t pressed)
{
    if (pressed & kBack) {
        scr   = Scr::kList;
        dirty = true;
        return;
    }
    if (pressed & kUp) {
#if SSH_ENABLE_ED25519
        sel   = (sel + 6) % 7;
#else
        sel   = (sel + 5) % 6;
#endif
        dirty = true;
    }
    if (pressed & kDown) {
#if SSH_ENABLE_ED25519
        sel   = (sel + 1) % 7;
#else
        sel   = (sel + 1) % 6;
#endif
        dirty = true;
    }
    if (!(pressed & kEnter)) return;
    Profile* p = currentProfile();
    if (p == nullptr) return;
#if SSH_ENABLE_ED25519
    if (sel == 4) {
        p->auth = p->auth == 0 ? 1 : 0;
        saveProfiles();
        dirty = true;
    } else if (sel == 5) {
        if (p->auth == 1) {
            copyCString(note, sizeof(note), "KEY: /spiffs/ssh/id_ed25519");
            dirty = true;
        } else {
            beginEdit(4);
        }
    } else if (sel == 6) {
        startWifiForSsh();
    } else {
        beginEdit(sel);
    }
#else
    if (sel == 4) {
        beginEdit(4);
    } else if (sel == 5) {
        startWifiForSsh();
    } else {
        beginEdit(sel);
    }
#endif
}

void handleTerminal()
{
    const auto& ks = M5Cardputer.Keyboard.keysState();

    if (ks.fn && ks.del) {
        stopSsh();
        return;
    }

    // Opt + ;/. scrolls the history; ordinary ;/. still go to the shell.
    uint8_t scroll = 0;
    if (ks.opt) {
        for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
            const uint8_t code =
                static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
            if (code == ';') scroll |= 1;
            if (code == '.') scroll |= 2;
        }
    }
    if (scroll != prevScroll) {
        if (scroll & 1) ++termScroll;
        if (scroll & 2) --termScroll;
        if (termScroll < 0) termScroll = 0;
        const int maxScroll = sshterm::historyCount();
        if (termScroll > maxScroll) termScroll = maxScroll;
        dirty = true;
    }
    prevScroll = scroll;

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
        if (ks.opt && (c == ';' || c == '.')) continue;
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
    if (pressed & kClear) {
        SPIFFS.begin(true);
        SPIFFS.remove("/ssh/known_hosts");
        SPIFFS.remove("/ssh/hostkeys.txt");
        copyCString(note, sizeof(note), "HOST KEYS CLEARED");
        scr   = Scr::kList;
        dirty = true;
        return;
    }
    if (pressed & kRetry) {
        if (!sshRunning) startWifiForSsh();
        return;
    }
    if (pressed & kEnter) {
        scr   = Scr::kList;
        dirty = true;
    }
}

void handleHostKey(uint32_t pressed)
{
    if (pressed & kEnter) {
        hostKeyDecision = 1;
        scr             = Scr::kConnecting;
        dirty           = true;
        return;
    }
    if (pressed & kBack) {
        hostKeyDecision = 2;
        sshStop         = true;
        sshPhase        = 0;
        copyCString(note, sizeof(note), "HOST KEY REJECTED");
        scr   = Scr::kList;
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
    loadProfiles();
    listSel = 0;
    sel     = 0;

    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
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

    scr   = Scr::kList;
    dirty = true;
}

void loop()
{
    M5Cardputer.update();
    pollWifi();

    if (deleteArmed && millis() - deleteAt > 2000) {
        deleteArmed = false;
        note[0]     = 0;
        dirty       = true;
    }

    // The task flips phases asynchronously; the UI only reacts to transitions.
    if (sshPhase == 6 && hostKeyDecision == 0 && scr != Scr::kHostKey) {
        scr   = Scr::kHostKey;
        dirty = true;
    } else if (sshPhase == 3 && scr != Scr::kTerminal) {
        scr   = Scr::kTerminal;
        dirty = true;
        termScroll = 0;
        prevScroll = 0;
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
            scr   = Scr::kList;
            dirty = true;
        }
    }

    const uint32_t a       = readActions();
    const uint32_t pressed = a & ~prevActions;
    prevActions            = a;

    switch (scr) {
        case Scr::kList:
            handleList(pressed);
            break;
        case Scr::kProfile:
            handleProfile(pressed);
            break;
        case Scr::kEdit:
            handleEdit(pressed);
            if (millis() - editCursorMs >= 500) {
                editCursorMs = millis();
                editCursorOn = !editCursorOn;
                drawEditLine();
            }
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
        case Scr::kHostKey:
            handleHostKey(pressed);
            break;
    }

    if (dirty) {
        dirty = false;
        drawCurrent();
    }
    delay(5);
}

}  // namespace sshapp
