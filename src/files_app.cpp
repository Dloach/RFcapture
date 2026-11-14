#include "files_app.h"

#include <M5Cardputer.h>
#include <SPIFFS.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "launcher.h"
#include "polling_keyboard.h"
#include "sd_mount.h"
#include "ui_hint.h"

namespace filesapp {
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

enum class Storage : uint8_t { kSd, kSpiffs };
enum class Screen : uint8_t { kList, kViewer };

struct Entry {
    char name[64];
    bool dir;
    uint32_t size;
};

std::vector<Entry> entries;
char cwd[128] = "/";
int sel = 0;
int top = 0;
Storage storage = Storage::kSd;
Screen screen = Screen::kList;
bool dirty = true;
bool deleteArmed = false;
uint32_t deleteAt = 0;
char note[48] = {0};
bool sdOk = false;
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

// Viewer
char viewer[80][41];
int viewerLines = 0;
int viewerTop = 0;
char viewerName[64] = {0};

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

void refresh();

void drawList()
{
    lcd.fillScreen(kBg);
    header(storage == Storage::kSd ? "FILES / TF" : "FILES / SPIFFS");

    lcd.setTextColor(kDim, kBg);
    lcd.drawString(cwd, 4, 15);

    constexpr int kRows = 8;
    if (entries.empty()) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString(storage == Storage::kSd && !sdOk ? "NO TF CARD" : "EMPTY", 6, 32);
    }
    for (int r = 0; r < kRows; ++r) {
        const int idx = top + r;
        if (idx >= static_cast<int>(entries.size())) break;
        const int y = 27 + r * 10;
        if (idx == sel) lcd.fillRect(0, y - 1, lcd.width(), 10, kBar);
        const Entry& e = entries[idx];
        lcd.setTextColor(idx == sel ? kAccent : (e.dir ? kOk : kText),
                         idx == sel ? kBar : kBg);
        lcd.drawString(e.name, 4, y);
        char size[16];
        if (!e.dir) {
            snprintf(size, sizeof(size), "%lu", static_cast<unsigned long>(e.size));
            lcd.setTextColor(idx == sel ? kAccent : kDim, idx == sel ? kBar : kBg);
            lcd.drawRightString(size, lcd.width() - 4, y);
        }
    }

    lcd.setTextColor(note[0] ? kWarn : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT open  D delete  S storage", 4, 108);

    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void drawViewer()
{
    lcd.fillScreen(kBg);
    header(viewerName);
    lcd.setTextColor(kDim, kBg);
    for (int r = 0; r < 10; ++r) {
        const int idx = viewerTop + r;
        if (idx >= viewerLines) break;
        lcd.drawString(viewer[idx], 4, 16 + r * 9);
    }
    if (viewerLines == 0) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString("EMPTY OR BINARY", 6, 40);
    }
    lcd.setTextColor(kDim, kBg);
    char pos[24];
    snprintf(pos, sizeof(pos), "%d/%d", viewerTop + 1, viewerLines);
    lcd.drawRightString(pos, lcd.width() - 4, 108);
    static const Hint h[] = {{"; .", "scroll"}, {"esc", "back"}};
    hints(h, 2);
}

void draw()
{
    if (screen == Screen::kViewer) drawViewer();
    else drawList();
    dirty = false;
}

void refresh()
{
    entries.clear();
    sel = 0;
    top = 0;
    note[0] = 0;
    deleteArmed = false;

    if (storage == Storage::kSd) {
        sdOk = sdmount::ensure(false);
        if (!sdOk) {
            snprintf(note, sizeof(note), "NO TF CARD");
            dirty = true;
            return;
        }
    } else {
        SPIFFS.begin(true);
    }

    File dir;
    if (storage == Storage::kSd) dir = SD.open(cwd);
    else dir = SPIFFS.open(cwd);
    if (!dir) {
        snprintf(note, sizeof(note), "OPEN FAILED");
        dirty = true;
        return;
    }

    while (true) {
        File f = dir.openNextFile();
        if (!f) break;
        const char* raw = f.name();
        const char* base = strrchr(raw, '/');
        base = base ? base + 1 : raw;
        if (base[0] == 0 || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
            f.close();
            continue;
        }
        if (entries.size() < 64) {
            Entry e{};
            strncpy(e.name, base, sizeof(e.name) - 1);
            e.dir  = f.isDirectory();
            e.size = e.dir ? 0 : f.size();
            entries.push_back(e);
        }
        f.close();
    }
    dir.close();

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir > b.dir;
        return strcmp(a.name, b.name) < 0;
    });
    dirty = true;
}

void openEntry()
{
    if (sel < 0 || sel >= static_cast<int>(entries.size())) return;
    Entry e = entries[sel];
    char path[160];
    if (strcmp(cwd, "/") == 0) snprintf(path, sizeof(path), "/%s", e.name);
    else snprintf(path, sizeof(path), "%s/%s", cwd, e.name);

    if (e.dir) {
        strncpy(cwd, path, sizeof(cwd) - 1);
        refresh();
        return;
    }

    File f;
    if (storage == Storage::kSd) f = SD.open(path, FILE_READ);
    else f = SPIFFS.open(path, FILE_READ);
    if (!f) {
        snprintf(note, sizeof(note), "OPEN FAILED");
        dirty = true;
        return;
    }
    viewerLines = 0;
    viewerTop   = 0;
    strncpy(viewerName, e.name, sizeof(viewerName) - 1);
    while (f.available() && viewerLines < 80) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 40) line = line.substring(0, 40);
        strncpy(viewer[viewerLines], line.c_str(), 40);
        viewer[viewerLines][40] = 0;
        ++viewerLines;
    }
    f.close();
    screen = Screen::kViewer;
    dirty  = true;
}

void deleteSelected()
{
    if (sel < 0 || sel >= static_cast<int>(entries.size())) return;
    const Entry& e = entries[sel];
    if (e.dir) {
        snprintf(note, sizeof(note), "DIR NOT DELETED");
        dirty = true;
        return;
    }
    char path[160];
    if (strcmp(cwd, "/") == 0) snprintf(path, sizeof(path), "/%s", e.name);
    else snprintf(path, sizeof(path), "%s/%s", cwd, e.name);
    bool ok = storage == Storage::kSd ? SD.remove(path) : SPIFFS.remove(path);
    snprintf(note, sizeof(note), ok ? "DELETED" : "DELETE FAILED");
    if (ok) refresh();
    dirty = true;
}

enum : uint32_t { kUp = 1, kDown = 2, kEnter = 4, kBack = 8, kDelete = 16, kStorage = 32 };

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
            case 'd': case 'D': a |= kDelete; break;
            case 's': case 'S': a |= kStorage; break;
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
    refresh();
}

void loop()
{
    M5Cardputer.update();
    const uint32_t actions = readActions();
    const uint32_t pressed = actions & ~prevActions;
    prevActions            = actions;

    if (deleteArmed && millis() - deleteAt > 2000) {
        deleteArmed = false;
        if (strcmp(note, "PRESS D AGAIN") == 0) note[0] = 0;
        dirty = true;
    }

    if (screen == Screen::kViewer) {
        if (pressed & kBack) {
            screen = Screen::kList;
            dirty  = true;
        } else if (pressed & kUp) {
            if (viewerTop > 0) --viewerTop;
            dirty = true;
        } else if (pressed & kDown) {
            if (viewerTop + 1 < viewerLines) ++viewerTop;
            dirty = true;
        }
        if (dirty) draw();
        return;
    }

    if (pressed & kBack) {
        if (strcmp(cwd, "/") != 0) {
            char* slash = strrchr(cwd, '/');
            if (slash != cwd && slash != nullptr) *slash = 0;
            else strcpy(cwd, "/");
            refresh();
        } else {
            launcher::rebootInto(launcher::kAppMenu);
        }
        return;
    }
    if (pressed & kStorage) {
        storage = storage == Storage::kSd ? Storage::kSpiffs : Storage::kSd;
        strcpy(cwd, "/");
        refresh();
    }
    if (pressed & kUp) {
        if (sel > 0) --sel;
        if (sel < top) top = sel;
        dirty = true;
    }
    if (pressed & kDown) {
        if (sel + 1 < static_cast<int>(entries.size())) ++sel;
        if (sel >= top + 8) top = sel - 7;
        dirty = true;
    }
    if (pressed & kEnter) {
        openEntry();
    }
    if (pressed & kDelete) {
        if (!deleteArmed) {
            deleteArmed = true;
            deleteAt    = millis();
            snprintf(note, sizeof(note), "PRESS D AGAIN");
            dirty = true;
        } else {
            deleteArmed = false;
            deleteSelected();
        }
    }

    if (dirty) draw();
}

}  // namespace filesapp
