#include "badusb_app.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

#include <memory>
#include <cstring>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "sd_mount.h"
#include "ui_hint.h"

namespace badusbapp {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;

auto& lcd = M5Cardputer.Display;
USBHIDKeyboard usbKeyboard;

enum class Screen : uint8_t { kWarning, kList, kRunning, kDone };
Screen screen = Screen::kWarning;
bool dirty = true;
bool sdOk = false;
char scripts[24][64];
int scriptCount = 0;
int sel = 0;
int top = 0;
char status[48] = {0};
uint32_t prevActions = 0;

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
        x += lcd.textWidth(h[i].key) + 5;
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(h[i].text, x, 122);
        x += lcd.textWidth(h[i].text) + 10;
    }
}

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

void scanScripts()
{
    scriptCount = 0;
    SD.mkdir("/badusb");
    File dir = SD.open("/badusb");
    if (!dir) {
        sdOk = false;
        return;
    }
    while (true) {
        File f = dir.openNextFile();
        if (!f) break;
        const char* raw = f.name();
        const char* base = strrchr(raw, '/');
        base = base ? base + 1 : raw;
        String lower = base;
        lower.toLowerCase();
        if (!f.isDirectory() && lower.endsWith(".txt")) {
            if (scriptCount < 24) {
                strncpy(scripts[scriptCount], base, sizeof(scripts[0]) - 1);
                scripts[scriptCount][sizeof(scripts[0]) - 1] = 0;
                ++scriptCount;
            }
        }
        f.close();
    }
    dir.close();
}

void drawWarning()
{
    lcd.fillScreen(kBg);
    header("BADUSB");
    lcd.setTextColor(kWarn, kBg);
    lcd.drawString("AUTHORIZED USE ONLY", 8, 24);
    lcd.setTextColor(kText, kBg);
    lcd.drawString("Runs DuckyScript from", 8, 44);
    lcd.drawString("/badusb/*.txt as a USB keyboard.", 8, 56);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Plug into the target PC after this screen.", 8, 76);
    lcd.drawString("Never run untrusted scripts.", 8, 88);
    static const Hint h[] = {{"ENT", "accept"}, {"esc", "back"}};
    hints(h, 2);
}

void drawList()
{
    lcd.fillScreen(kBg);
    header("BADUSB SCRIPTS");
    if (!sdOk) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString("NO TF CARD", 8, 30);
    } else if (scriptCount == 0) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("No .txt scripts in /badusb.", 8, 30);
    }
    for (int r = 0; r < 7; ++r) {
        const int idx = top + r;
        if (idx >= scriptCount) break;
        const int y = 20 + r * 12;
        if (idx == sel) lcd.fillRect(0, y - 2, lcd.width(), 12, kBar);
        lcd.setTextColor(idx == sel ? kAccent : kText, idx == sel ? kBar : kBg);
        lcd.drawString(scripts[idx], 6, y);
    }
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(status[0] ? status : "ENT run   esc back", 4, 108);
    static const Hint h[] = {{"ENT", "run"}, {"esc", "back"}};
    hints(h, 2);
}

void drawRunning()
{
    lcd.fillScreen(kBg);
    header("BADUSB RUNNING");
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString(status, 6, 30);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("FN BKSP aborts", 6, 98);
}

void drawDone()
{
    lcd.fillScreen(kBg);
    header("BADUSB DONE");
    lcd.setTextColor(kOk, kBg);
    lcd.drawString("SCRIPT FINISHED", 8, 34);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("esc back to script list", 8, 60);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void draw()
{
    switch (screen) {
        case Screen::kWarning: drawWarning(); break;
        case Screen::kList: drawList(); break;
        case Screen::kRunning: drawRunning(); break;
        case Screen::kDone: drawDone(); break;
    }
    dirty = false;
}

// HID key codes, avoiding the KEY_* macro collision between M5's keyboard
// headers and USBHIDKeyboard.
uint8_t keyCode(const String& token)
{
    if (token == "ENTER" || token == "RETURN") return 0x28;
    if (token == "ESC" || token == "ESCAPE") return 0x29;
    if (token == "BACKSPACE") return 0x2A;
    if (token == "TAB") return 0x2B;
    if (token == "SPACE") return 0x2C;
    if (token == "DELETE" || token == "DEL") return 0x4C;
    if (token == "HOME") return 0x4A;
    if (token == "END") return 0x4D;
    if (token == "PAGEUP") return 0x4B;
    if (token == "PAGEDOWN") return 0x4E;
    if (token == "RIGHT") return 0x4F;
    if (token == "LEFT") return 0x50;
    if (token == "DOWN") return 0x51;
    if (token == "UP") return 0x52;
    if (token == "CAPSLOCK") return 0x39;
    if (token.length() == 1) {
        const char c = token[0];
        if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
        if (c >= 'A' && c <= 'Z') return 0x04 + (c - 'A');
        if (c >= '1' && c <= '9') return 0x1E + (c - '1');
        if (c == '0') return 0x27;
    }
    if (token.length() >= 2 && token[0] == 'F') {
        const int n = token.substring(1).toInt();
        if (n >= 1 && n <= 12) return 0x3A + (n - 1);
    }
    return 0;
}

bool modifierCode(const String& token, uint8_t& code)
{
    if (token == "CTRL" || token == "CONTROL") code = 0xE0;
    else if (token == "SHIFT") code = 0xE1;
    else if (token == "ALT" || token == "OPTION") code = 0xE2;
    else if (token == "GUI" || token == "WIN" || token == "COMMAND") code = 0xE3;
    else return false;
    return true;
}

bool runScript(const char* name)
{
    char path[96];
    snprintf(path, sizeof(path), "/badusb/%s", name);
    File f = SD.open(path, FILE_READ);
    if (!f) {
        snprintf(status, sizeof(status), "OPEN FAILED");
        return false;
    }

    int defaultDelay = 10;
    snprintf(status, sizeof(status), "RUNNING");
    screen = Screen::kRunning;
    dirty  = true;
    draw();

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("REM") || line.startsWith("//")) continue;

        String upper = line;
        upper.toUpperCase();
        if (upper.startsWith("DELAY ")) {
            int ms = upper.substring(6).toInt();
            if (ms > 10000) ms = 10000;
            if (ms > 0) delay(ms);
            continue;
        }
        if (upper.startsWith("DEFAULTDELAY ")) {
            defaultDelay = upper.substring(13).toInt();
            if (defaultDelay < 0) defaultDelay = 0;
            if (defaultDelay > 1000) defaultDelay = 1000;
            continue;
        }
        if (upper.startsWith("STRING ")) {
            String text = line.substring(7);
            for (size_t i = 0; i < text.length(); ++i) {
                char c = text[i];
                if (c == '\r') continue;
                usbKeyboard.print(c);
                delay(defaultDelay);
            }
            continue;
        }

        // Combination line: CTRL ALT DELETE, GUI r, etc.
        uint8_t mods[4];
        int modCount = 0;
        uint8_t key = 0;
        int start = 0;
        while (start < (int)upper.length() && modCount < 4) {
            int sp = upper.indexOf(' ', start);
            String token = sp < 0 ? upper.substring(start) : upper.substring(start, sp);
            uint8_t mod = 0;
            if (modifierCode(token, mod)) {
                mods[modCount++] = mod;
            } else {
                key = keyCode(token);
                break;
            }
            if (sp < 0) break;
            start = sp + 1;
        }
        if (key != 0) {
            for (int i = 0; i < modCount; ++i) usbKeyboard.pressRaw(mods[i]);
            usbKeyboard.pressRaw(key);
            delay(defaultDelay + 10);
            usbKeyboard.releaseRaw(key);
            for (int i = 0; i < modCount; ++i) usbKeyboard.releaseRaw(mods[i]);
        } else if (modCount > 0) {
            for (int i = 0; i < modCount; ++i) usbKeyboard.pressRaw(mods[i]);
            delay(defaultDelay);
            for (int i = 0; i < modCount; ++i) usbKeyboard.releaseRaw(mods[i]);
        }

        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed('~') ||
            M5Cardputer.Keyboard.isKeyPressed(0x2A)) {
            snprintf(status, sizeof(status), "ABORTED");
            f.close();
            return false;
        }
    }
    f.close();
    snprintf(status, sizeof(status), "SCRIPT FINISHED");
    return true;
}

enum : uint32_t { kUp = 1, kDown = 2, kEnter = 4, kBack = 8 };

uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t c = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (c) {
            case ';': a |= kUp; break;
            case '.': a |= kDown; break;
            case KEY_ENTER: a |= kEnter; break;
            case '`': case '~': case 0x1B: a |= kBack; break;
            default: break;
        }
    }
    return a;
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

    USB.begin();
    usbKeyboard.begin();

    sdOk = sdmount::ensure(false);
    if (sdOk) scanScripts();
    dirty = true;
}

void loop()
{
    M5Cardputer.update();
    const uint32_t actions = readActions();
    const uint32_t pressed = actions & ~prevActions;
    prevActions            = actions;

    if (screen == Screen::kWarning) {
        if (pressed & kBack) {
            launcher::rebootInto(launcher::kAppMenu);
            return;
        }
        if (pressed & kEnter) {
            screen = Screen::kList;
            dirty  = true;
        }
    } else if (screen == Screen::kList) {
        if (pressed & kBack) {
            launcher::rebootInto(launcher::kAppMenu);
            return;
        }
        if (pressed & kUp && sel > 0) {
            --sel;
            if (sel < top) top = sel;
            dirty = true;
        }
        if (pressed & kDown && sel + 1 < scriptCount) {
            ++sel;
            if (sel >= top + 7) top = sel - 6;
            dirty = true;
        }
        if (pressed & kEnter && scriptCount > 0) {
            const bool ok = runScript(scripts[sel]);
            screen = ok ? Screen::kDone : Screen::kList;
            dirty  = true;
        }
    } else if (screen == Screen::kDone) {
        if (pressed & kBack) {
            screen = Screen::kList;
            dirty  = true;
        }
    }

    if (dirty) draw();
}

}  // namespace badusbapp
