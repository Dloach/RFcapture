/*
 * IR TOOLS -- send, learn and manage infrared remotes.
 *
 * Hardware in one line: the board transmits on G44 (single IR LED, 22 R, no
 * carrier hardware) and *cannot* receive unless a 38 kHz demodulating module is
 * wired to the Grove port.  Everything here is built around that asymmetry:
 * sending works out of the box, learning says so when it has no receiver.
 *
 * Layout, one reboot into this app (see launcher.h):
 *
 *   MY CODES     parsed codes in NVS (ir_store.h), export to /ir/MYCODES.ir
 *   SD REMOTES   browse .ir files on the TF card, per-button replay
 *   LEARN        capture with an external receiver, decode, save
 *   UNIVERSAL    a handful of built-in TV codes, so the feature works with no
 *                card and nothing learned yet
 *   SETUP        receiver pin, repeat count, LED test
 */

#include "ir_app.h"

#include <M5Cardputer.h>
#include <Preferences.h>
#include <SD.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "ir_config.h"
#include "ir_file.h"
#include "ir_protocol.h"
#include "ir_rx.h"
#include "ir_store.h"
#include "ir_tx.h"
#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "sd_mount.h"
#include "ui_hint.h"
#include "usb_log.h"

namespace irapp {
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

// Nine 10 px rows fit between the header and the footer, like FILES does.
constexpr int kRows = 9;
constexpr int kRowH = 10;

enum class Page : uint8_t {
    kHome,
    kMine,
    kAc,
    kFiles,
    kFileKeys,
    kLearn,
    kUniv,
    kUnivKeys,
    kSetup,
};

// One key press, edge triggered by comparing against the previous frame.
enum : uint32_t {
    kUp     = 1u << 0,
    kDown   = 1u << 1,
    kEnter  = 1u << 2,
    kBack   = 1u << 3,
    kSave   = 1u << 4,
    kDelete = 1u << 5,
    kLeft   = 1u << 6,
    kRight  = 1u << 7,
};

uint32_t readActions();

Page page      = Page::kHome;
int homeSel    = 0;
bool dirty     = true;
uint32_t prevActions = 0;
char note[52]  = {0};

Preferences prefs;
bool prefsReady = false;
uint8_t rxPin   = irrx::kPinG1;
uint8_t repeats = 3;

// ---------------------------------------------------------------------------
// Footer hints
// ---------------------------------------------------------------------------
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
    std::snprintf(bat, sizeof(bat), level < 0 ? "--" : "%d%%", level);
    lcd.setTextColor(kText, kHead);
    lcd.drawRightString(bat, lcd.width() - 3, 2);
}

// ---------------------------------------------------------------------------
// Built-in universal codes
//
// These are the classic, widely published mains codes for the four most common
// TV brands.  They are a demonstration that the transmitter works and a First
// Aid for a lost remote -- if one of them does nothing, learn the real button.
// ---------------------------------------------------------------------------
struct UKey {
    const char* name;
    ir::Protocol proto;
    uint32_t address;
    uint32_t command;
};

const UKey kLgKeys[] = {
    {"POWER", ir::Protocol::kNec, 0x04, 0x08}, {"VOL+", ir::Protocol::kNec, 0x04, 0x02},
    {"VOL-", ir::Protocol::kNec, 0x04, 0x03},  {"MUTE", ir::Protocol::kNec, 0x04, 0x09},
    {"CH+", ir::Protocol::kNec, 0x04, 0x00},   {"CH-", ir::Protocol::kNec, 0x04, 0x01},
    {"INPUT", ir::Protocol::kNec, 0x04, 0x0B},
};

const UKey kSamsungKeys[] = {
    {"POWER", ir::Protocol::kSamsung32, 0x07, 0x02},
    {"SOURCE", ir::Protocol::kSamsung32, 0x07, 0x01},
    {"VOL+", ir::Protocol::kSamsung32, 0x07, 0x07},
    {"VOL-", ir::Protocol::kSamsung32, 0x07, 0x0B},
    {"MUTE", ir::Protocol::kSamsung32, 0x07, 0x0F},
    {"CH+", ir::Protocol::kSamsung32, 0x07, 0x12},
    {"CH-", ir::Protocol::kSamsung32, 0x07, 0x10},
};

const UKey kSonyKeys[] = {
    {"POWER", ir::Protocol::kSirc, 0x01, 0x15}, {"VOL+", ir::Protocol::kSirc, 0x01, 0x12},
    {"VOL-", ir::Protocol::kSirc, 0x01, 0x13},  {"MUTE", ir::Protocol::kSirc, 0x01, 0x14},
    {"CH+", ir::Protocol::kSirc, 0x01, 0x10},   {"CH-", ir::Protocol::kSirc, 0x01, 0x11},
};

const UKey kPhilipsKeys[] = {
    {"POWER", ir::Protocol::kRc5, 0x00, 0x0C}, {"VOL+", ir::Protocol::kRc5, 0x00, 0x10},
    {"VOL-", ir::Protocol::kRc5, 0x00, 0x11},  {"MUTE", ir::Protocol::kRc5, 0x00, 0x0D},
    {"CH+", ir::Protocol::kRc5, 0x00, 0x20},   {"CH-", ir::Protocol::kRc5, 0x00, 0x21},
};

struct UBrand {
    const char* name;
    const UKey* keys;
    int count;
};

const UBrand kBrands[] = {
    {"LG / NEC 0x04", kLgKeys, static_cast<int>(sizeof(kLgKeys) / sizeof(kLgKeys[0]))},
    {"SAMSUNG 0x07", kSamsungKeys, static_cast<int>(sizeof(kSamsungKeys) / sizeof(kSamsungKeys[0]))},
    {"SONY SIRC 0x01", kSonyKeys, static_cast<int>(sizeof(kSonyKeys) / sizeof(kSonyKeys[0]))},
    {"PHILIPS RC5 0x00", kPhilipsKeys,
     static_cast<int>(sizeof(kPhilipsKeys) / sizeof(kPhilipsKeys[0]))},
};
constexpr int kBrandCount = static_cast<int>(sizeof(kBrands) / sizeof(kBrands[0]));
int univSel    = 0;
int univKeySel = 0;

// ---------------------------------------------------------------------------
// Shared static buffers
//
// A Timings is 1 kB and an irfile::Entry is over 1 kB; neither may live on the
// loop task's 8 kB stack while a frame is being transmitted.
// ---------------------------------------------------------------------------
ir::Timings g_timings;
irfile::Entry g_entry;
uint16_t g_learnUs[ir::kMaxTimings];
int g_learnCount = 0;
ir::Signal g_learnSig;
bool g_learnDecoded = false;
bool g_learnHasFrame = false;

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

// `Serial` is UART0 in this firmware and is not routed to USB, so app-level
// logs also go through the shared USBCDC instance (src/usb_log.h).  Both are
// written: the UART is what a wired probe sees, the CDC is what a phone or a
// laptop sees.  Writing to the CDC before USB.begin() is a no-op, not a crash.
void irLog(const char* format, ...)
{
    char buffer[160];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.println(buffer);
    usbLogf("%s", buffer);
}

void drainKeyboard()
{
    // A transmission blocks for up to a few hundred milliseconds; the polling
    // keyboard reader still needs its update() or held keys look like presses.
    for (int i = 0; i < 12; ++i) {
        M5Cardputer.update();
        delay(2);
    }
    // Resynchronise to whatever is still held: zeroing would make the next loop
    // treat the send key as a fresh press and repeat the transmission.
    prevActions = readActions();
}

uint32_t gapFor(ir::Protocol p)
{
    // Inter-frame silence.  NEC wants ~110 ms between frames, Sony 45 ms, and
    // the Manchester protocols are happy anywhere above their own frame time.
    switch (p) {
        case ir::Protocol::kSirc:
        case ir::Protocol::kSirc15:
        case ir::Protocol::kSirc20: return 25000;
        case ir::Protocol::kNec:
        case ir::Protocol::kNecExt: return 40000;
        default: return 40000;
    }
}

bool sendSignal(const ir::Signal& s)
{
    if (!ir::encode(s, &g_timings)) {
        std::snprintf(note, sizeof(note), "CANNOT ENCODE %s", ir::protocolName(s.proto));
        dirty = true;
        return false;
    }
    // An air conditioner frame already carries the complete state, so one copy
    // is one press -- repeating it does not help and some units dislike it
    // (IRremoteESP8266 also sends Gree exactly once).
    const int copies = (s.proto == ir::Protocol::kGree) ? 1 : repeats;
    const bool ok = irtx::send(g_timings.us, g_timings.count, g_timings.frequency,
                               g_timings.dutyPercent, copies, gapFor(s.proto));
    char desc[40];
    ir::describe(s, desc, sizeof(desc));
    irLog("[ir] send %s x%d -> %s", desc, copies, ok ? "ok" : "failed");
    std::snprintf(note, sizeof(note), ok ? "SENT %s" : "TX FAILED", ir::protocolName(s.proto));
    drainKeyboard();
    dirty = true;
    return ok;
}

bool sendRaw(const uint16_t* us, int count, uint32_t frequency, uint8_t duty)
{
    const bool ok = irtx::send(us, count, frequency, duty, repeats, 25000);
    irLog("[ir] send raw %d timings @%lu -> %s", count,
                  static_cast<unsigned long>(frequency), ok ? "ok" : "failed");
    std::snprintf(note, sizeof(note), ok ? "SENT RAW %d" : "TX FAILED", count);
    drainKeyboard();
    dirty = true;
    return ok;
}

// ---------------------------------------------------------------------------
// MY CODES
// ---------------------------------------------------------------------------
int mineSel = 0;
int mineTop = 0;
bool deleteArmed = false;
uint32_t deleteArmedAt = 0;

void enterMine()
{
    page    = Page::kMine;
    mineSel = 0;
    mineTop = 0;
    note[0] = 0;
    dirty   = true;
}

void drawMine()
{
    lcd.fillScreen(kBg);
    header("IR / MY CODES");

    const int n = irstore::count();
    if (n == 0) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString("EMPTY", 6, 24);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("learn or copy from a", 6, 40);
        lcd.drawString(".ir file", 6, 50);
    }
    for (int r = 0; r < kRows; ++r) {
        const int idx = mineTop + r;
        if (idx >= n) break;
        const int y = 17 + r * kRowH;
        if (idx == mineSel) lcd.fillRect(0, y - 1, lcd.width(), kRowH, kBar);
        const irstore::Code& c = irstore::at(idx);
        lcd.setTextColor(idx == mineSel ? kAccent : kText, idx == mineSel ? kBar : kBg);
        lcd.drawString(c.name, 4, y);
        char right[24];
        std::snprintf(right, sizeof(right), "%lX %lX", static_cast<unsigned long>(c.address),
                      static_cast<unsigned long>(c.command));
        lcd.setTextColor(idx == mineSel ? kAccent : kDim, idx == mineSel ? kBar : kBg);
        lcd.drawRightString(right, lcd.width() - 4, y);
    }

    lcd.setTextColor(note[0] ? kAccent : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT send  D delete  S export", 4, 108);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

bool exportLibrary()
{
    if (!sdmount::ensure(false)) {
        std::snprintf(note, sizeof(note), "NO TF CARD");
        return false;
    }
    SD.mkdir("/ir");
    File f = SD.open("/ir/MYCODES.ir", FILE_WRITE);
    if (!f) {
        std::snprintf(note, sizeof(note), "OPEN FAILED");
        return false;
    }
    f.print("Filetype: IR signals file\nVersion: 1\n");
    char buf[320];
    for (int i = 0; i < irstore::count(); ++i) {
        const irstore::Code& c = irstore::at(i);
        const int len = irfile::formatEntry(irstore::signalOf(c), c.name, buf, sizeof(buf));
        if (len > 0) f.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(len));
    }
    f.close();
    std::snprintf(note, sizeof(note), "EXPORTED %d", irstore::count());
    return true;
}

void handleMine(uint32_t pressed)
{
    const int n = irstore::count();
    if ((pressed & kUp) && mineSel > 0) {
        --mineSel;
        dirty = true;
    }
    if ((pressed & kDown) && mineSel + 1 < n) {
        ++mineSel;
        dirty = true;
    }
    if (mineSel < mineTop) mineTop = mineSel;
    if (mineSel >= mineTop + kRows) mineTop = mineSel - kRows + 1;

    if ((pressed & kEnter) != 0 && n > 0) {
        sendSignal(irstore::signalOf(irstore::at(mineSel)));
    }
    if ((pressed & kDelete) != 0 && n > 0) {
        // Two presses within 2 s, the same "are you sure" the FILES app uses;
        // deleting a learned remote you cannot relearn without the receiver is
        // not something to do on a stray keypress.
        if (!deleteArmed) {
            deleteArmed   = true;
            deleteArmedAt = millis();
            std::snprintf(note, sizeof(note), "PRESS D AGAIN");
        } else {
            deleteArmed = false;
            if (irstore::remove(mineSel)) {
                if (mineSel >= irstore::count()) mineSel = irstore::count() - 1;
                if (mineSel < 0) mineSel = 0;
                std::snprintf(note, sizeof(note), "DELETED");
            } else {
                std::snprintf(note, sizeof(note), "DELETE FAILED");
            }
        }
        dirty = true;
    }
    if (pressed & kSave) {
        exportLibrary();
        dirty = true;
    }
    if (pressed & kBack) {
        page    = Page::kHome;
        note[0] = 0;
        dirty   = true;
    }
}

// ---------------------------------------------------------------------------
// SD REMOTES: a directory browser over the TF card, then the buttons inside one
// `.ir` file.
// ---------------------------------------------------------------------------
struct FsEntry {
    char name[44];
    bool dir;
};

FsEntry fsEntries[64];
int fsCount = 0;
char cwd[96] = "/ir";
int fsSel     = 0;
int fsTop     = 0;

char* g_fileText = nullptr;
int g_fileLen    = 0;
char g_fileName[44] = {0};
irfile::Header g_headers[40];
int g_headerCount = 0;
int keySel        = 0;
int keyTop        = 0;

void freeFileText()
{
    if (g_fileText != nullptr) {
        free(g_fileText);
        g_fileText = nullptr;
    }
    g_fileLen = 0;
}

void refreshFiles()
{
    fsCount = 0;
    fsSel   = 0;
    fsTop   = 0;
    note[0] = 0;

    if (!sdmount::ensure(false)) {
        std::snprintf(note, sizeof(note), "NO TF CARD");
        return;
    }
    SD.mkdir("/ir");

    File dir = SD.open(cwd);
    if (!dir) {
        std::snprintf(note, sizeof(note), "OPEN FAILED");
        return;
    }
    while (fsCount < static_cast<int>(sizeof(fsEntries) / sizeof(fsEntries[0]))) {
        File f = dir.openNextFile();
        if (!f) break;
        const char* raw  = f.name();
        const char* base = strrchr(raw, '/');
        base             = (base != nullptr) ? base + 1 : raw;
        if (base[0] != 0 && strcmp(base, ".") != 0 && strcmp(base, "..") != 0) {
            FsEntry& e = fsEntries[fsCount];
            std::snprintf(e.name, sizeof(e.name), "%s", base);
            e.dir = f.isDirectory();
            ++fsCount;
        }
        f.close();
    }
    dir.close();

    std::sort(fsEntries, fsEntries + fsCount, [](const FsEntry& a, const FsEntry& b) {
        if (a.dir != b.dir) return a.dir > b.dir;
        return strcmp(a.name, b.name) < 0;
    });
    dirty = true;
}

bool loadFileText(const char* path)
{
    freeFileText();
    File f = SD.open(path, FILE_READ);
    if (!f) {
        std::snprintf(note, sizeof(note), "OPEN FAILED");
        return false;
    }
    const size_t size = f.size();
    // 48 kB is a whole Flipper-IRDB remote with room to spare; bigger files are
    // the A/C libraries, which this screen would not render well anyway.
    if (size == 0 || size > 48 * 1024) {
        f.close();
        std::snprintf(note, sizeof(note), size == 0 ? "EMPTY FILE" : "FILE TOO BIG");
        return false;
    }
    char* buf = static_cast<char*>(malloc(size + 1));
    if (buf == nullptr) {
        f.close();
        std::snprintf(note, sizeof(note), "OUT OF MEMORY");
        return false;
    }
    const size_t got = f.read(reinterpret_cast<uint8_t*>(buf), size);
    f.close();
    buf[got] = 0;
    g_fileText = buf;
    g_fileLen  = static_cast<int>(got);
    return true;
}

bool isIrFile(const char* name)
{
    const size_t n = strlen(name);
    if (n < 4) return false;
    if (name[n - 3] != '.') return false;
    const char a = name[n - 2];
    const char b = name[n - 1];
    return (a == 'i' || a == 'I') && (b == 'r' || b == 'R');
}

void openFileEntry()
{
    if (fsSel < 0 || fsSel >= fsCount) return;
    const FsEntry e = fsEntries[fsSel];

    char path[160];
    if (strcmp(cwd, "/") == 0) std::snprintf(path, sizeof(path), "/%s", e.name);
    else std::snprintf(path, sizeof(path), "%s/%s", cwd, e.name);

    if (e.dir) {
        std::snprintf(cwd, sizeof(cwd), "%s", path);
        refreshFiles();
        return;
    }

    // Only Flipper IR files make sense here.
    if (!isIrFile(e.name)) {
        std::snprintf(note, sizeof(note), "NOT A .ir FILE");
        dirty = true;
        return;
    }

    if (!loadFileText(path)) {
        dirty = true;
        return;
    }
    g_headerCount = irfile::listButtons(g_fileText, g_fileLen, g_headers,
                                        static_cast<int>(sizeof(g_headers) / sizeof(g_headers[0])));
    if (g_headerCount <= 0) {
        std::snprintf(note, sizeof(note), "NO BUTTONS");
        freeFileText();
        dirty = true;
        return;
    }
    std::snprintf(g_fileName, sizeof(g_fileName), "%s", e.name);
    keySel = 0;
    keyTop = 0;
    page   = Page::kFileKeys;
    note[0] = 0;
    dirty   = true;
}

void drawFiles()
{
    lcd.fillScreen(kBg);
    header("IR / TF REMOTES");
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(cwd, 4, 15);

    if (fsCount == 0) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString(note[0] ? note : "EMPTY", 6, 30);
    }
    for (int r = 0; r < kRows - 1; ++r) {
        const int idx = fsTop + r;
        if (idx >= fsCount) break;
        const int y = 26 + r * kRowH;
        if (idx == fsSel) lcd.fillRect(0, y - 1, lcd.width(), kRowH, kBar);
        lcd.setTextColor(idx == fsSel ? kAccent : (fsEntries[idx].dir ? kOk : kText),
                         idx == fsSel ? kBar : kBg);
        lcd.drawString(fsEntries[idx].name, 4, y);
    }

    lcd.setTextColor(note[0] ? kWarn : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT open", 4, 108);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void handleFiles(uint32_t pressed)
{
    if ((pressed & kUp) != 0 && fsSel > 0) {
        --fsSel;
        dirty = true;
    }
    if ((pressed & kDown) != 0 && fsSel + 1 < fsCount) {
        ++fsSel;
        dirty = true;
    }
    if (fsSel < fsTop) fsTop = fsSel;
    if (fsSel >= fsTop + kRows - 1) fsTop = fsSel - (kRows - 1) + 1;

    if (pressed & kEnter) openFileEntry();

    if (pressed & kBack) {
        if (strcmp(cwd, "/ir") != 0 && strcmp(cwd, "/") != 0) {
            char* slash = strrchr(cwd, '/');
            if (slash != nullptr && slash != cwd) *slash = 0;
            else std::snprintf(cwd, sizeof(cwd), "/ir");
            refreshFiles();
        } else {
            page    = Page::kHome;
            note[0] = 0;
            dirty   = true;
        }
    }
}

void drawFileKeys()
{
    lcd.fillScreen(kBg);
    header(g_fileName);

    for (int r = 0; r < kRows - 1; ++r) {
        const int idx = keyTop + r;
        if (idx >= g_headerCount) break;
        const int y = 17 + r * kRowH;
        if (idx == keySel) lcd.fillRect(0, y - 1, lcd.width(), kRowH, kBar);
        lcd.setTextColor(idx == keySel ? kAccent : kText, idx == keySel ? kBar : kBg);
        lcd.drawString(g_headers[idx].name, 4, y);
        lcd.setTextColor(idx == keySel ? kAccent : kDim, idx == keySel ? kBar : kBg);
        lcd.drawRightString(g_headers[idx].raw ? "raw" : "parsed", lcd.width() - 4, y);
    }

    lcd.setTextColor(note[0] ? kAccent : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT send  S copy to MY CODES", 4, 108);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void sendFileButton()
{
    if (g_fileText == nullptr) return;
    if (!irfile::loadButton(g_fileText, g_fileLen, keySel, &g_entry)) {
        std::snprintf(note, sizeof(note), "UNSUPPORTED PROTOCOL");
        dirty = true;
        return;
    }
    if (g_entry.raw) {
        sendRaw(g_entry.timings, g_entry.count, g_entry.frequency, g_entry.dutyPercent);
    } else {
        sendSignal(g_entry.signal);
    }
}

void copyFileButton()
{
    if (g_fileText == nullptr) return;
    if (!irfile::loadButton(g_fileText, g_fileLen, keySel, &g_entry) || g_entry.raw) {
        std::snprintf(note, sizeof(note), "RAW: USE LEARN/EXPORT");
        dirty = true;
        return;
    }
    if (irstore::find(g_entry.signal) >= 0) {
        std::snprintf(note, sizeof(note), "ALREADY SAVED");
        dirty = true;
        return;
    }
    if (irstore::add(g_entry.name, g_entry.signal) < 0) {
        std::snprintf(note, sizeof(note), "LIBRARY FULL");
        dirty = true;
        return;
    }
    std::snprintf(note, sizeof(note), "SAVED %s", g_entry.name);
    dirty = true;
}

void handleFileKeys(uint32_t pressed)
{
    if ((pressed & kUp) && keySel > 0) {
        --keySel;
        dirty = true;
    }
    if ((pressed & kDown) && keySel + 1 < g_headerCount) {
        ++keySel;
        dirty = true;
    }
    if (keySel < keyTop) keyTop = keySel;
    if (keySel >= keyTop + kRows - 1) keyTop = keySel - (kRows - 1) + 1;

    if (pressed & kEnter) sendFileButton();
    if (pressed & kSave) copyFileButton();
    if (pressed & kDelete) sendFileButton();  // second send key, easy to reach

    if (pressed & kBack) {
        freeFileText();
        page    = Page::kFiles;
        note[0] = 0;
        dirty   = true;
    }
}

// ---------------------------------------------------------------------------
// LEARN
// ---------------------------------------------------------------------------
bool rxStarted = false;

void drawLearn()
{
    lcd.fillScreen(kBg);
    header("IR / LEARN");

    if (!rxStarted) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString("NO RECEIVER", 4, 18);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("The board has no IR", 4, 32);
        lcd.drawString("receiver. Wire a 38 kHz", 4, 42);
        lcd.drawString("module to the Grove port:", 4, 52);
        lcd.setTextColor(kText, kBg);
        lcd.drawString("VCC->5V  GND->GND  OUT->G1", 4, 66);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("(SETUP changes G1 to G2)", 4, 76);
        static const Hint h[] = {{"esc", "back"}};
        hints(h, 1);
        return;
    }

    if (!g_learnHasFrame) {
        // A blinking dot is the entire "I am listening" affordance.
        const bool on = (millis() / 400) % 2 == 0;
        lcd.fillCircle(10, 24, 3, on ? kWarn : kDim);
        lcd.setTextColor(kText, kBg);
        lcd.drawString("POINT REMOTE", 20, 20);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("press one key", 20, 32);
        static const Hint h[] = {{"esc", "cancel"}};
        hints(h, 1);
        return;
    }

    char line[40];
    lcd.setTextColor(kOk, kBg);
    lcd.drawString(g_learnDecoded ? "DECODED" : "RAW CAPTURE", 4, 17);

    lcd.setTextColor(kText, kBg);
    if (g_learnDecoded) {
        std::snprintf(line, sizeof(line), "%s", ir::protocolName(g_learnSig.proto));
        lcd.drawString(line, 4, 29);
        std::snprintf(line, sizeof(line), "ADDR  %04lX", static_cast<unsigned long>(g_learnSig.address));
        lcd.drawString(line, 4, 40);
        std::snprintf(line, sizeof(line), "CMD   %04lX", static_cast<unsigned long>(g_learnSig.command));
        lcd.drawString(line, 4, 51);
    } else {
        lcd.drawString("unknown protocol", 4, 29);
        std::snprintf(line, sizeof(line), "%d timings", g_learnCount);
        lcd.drawString(line, 4, 40);
    }
    std::snprintf(line, sizeof(line), "%d edges", g_learnCount);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(line, 4, 62);

    lcd.setTextColor(note[0] ? kAccent : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT resend  S save", 4, 108);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void saveLearned()
{
    if (!g_learnHasFrame) return;

    if (g_learnDecoded) {
        char name[irstore::kNameLen];
        std::snprintf(name, sizeof(name), "%s %lX:%lX", ir::protocolName(g_learnSig.proto),
                      static_cast<unsigned long>(g_learnSig.address),
                      static_cast<unsigned long>(g_learnSig.command));
        if (irstore::find(g_learnSig) >= 0) {
            std::snprintf(note, sizeof(note), "ALREADY SAVED");
        } else if (irstore::add(name, g_learnSig) < 0) {
            std::snprintf(note, sizeof(note), "LIBRARY FULL");
        } else {
            std::snprintf(note, sizeof(note), "SAVED %s", ir::protocolName(g_learnSig.proto));
        }
        dirty = true;
        return;
    }

    // Unknown protocol: the only place it fits is a raw `.ir` file.
    if (!sdmount::ensure(false)) {
        std::snprintf(note, sizeof(note), "NO TF CARD");
        dirty = true;
        return;
    }
    SD.mkdir("/ir");
    char path[48];
    // Pick the first free CAPTUREn.ir rather than a per-boot counter that would
    // silently overwrite yesterday's capture.
    for (int seq = 1; seq <= 99; ++seq) {
        std::snprintf(path, sizeof(path), "/ir/CAPTURE%d.ir", seq);
        if (!SD.exists(path)) break;
    }
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        std::snprintf(note, sizeof(note), "OPEN FAILED");
        dirty = true;
        return;
    }
    std::memset(&g_entry, 0, sizeof(g_entry));
    std::snprintf(g_entry.name, sizeof(g_entry.name), "CAPTURE %d", g_learnCount);
    g_entry.raw         = true;
    g_entry.frequency   = 38000;
    g_entry.dutyPercent = 33;
    g_entry.count       = g_learnCount < ir::kMaxTimings ? g_learnCount : ir::kMaxTimings;
    for (int i = 0; i < g_entry.count; ++i) g_entry.timings[i] = g_learnUs[i];

    // 512 timings print as ~2.6 kB of decimal, so the staging buffer has to be
    // bigger than the 512 bytes first used here -- formatFile() otherwise
    // refuses and the file is left empty.
    static char buf[4096];
    const int len = irfile::formatFile(&g_entry, 1, buf, sizeof(buf));
    if (len <= 0) {
        f.close();
        SD.remove(path);
        std::snprintf(note, sizeof(note), "TOO BIG TO SAVE");
        dirty = true;
        return;
    }
    f.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(len));
    f.close();
    std::snprintf(note, sizeof(note), "SAVED %s", path);
    dirty = true;
}

void enterLearn()
{
    page            = Page::kLearn;
    note[0]         = 0;
    g_learnHasFrame = false;
    g_learnCount    = 0;
    g_learnDecoded  = false;

    rxStarted = irrx::begin(rxPin, 20);
    if (!rxStarted) std::snprintf(note, sizeof(note), "RX INIT FAILED");
    irrx::flush();
    dirty = true;
}

void handleLearn(uint32_t pressed)
{
    if (pressed & kBack) {
        irrx::end();
        rxStarted       = false;
        g_learnHasFrame = false;
        page            = Page::kHome;
        note[0]         = 0;
        dirty           = true;
        return;
    }
    if (!g_learnHasFrame) return;

    if (pressed & kEnter) {
        if (g_learnDecoded) {
            sendSignal(g_learnSig);
        } else {
            sendRaw(g_learnUs, g_learnCount, 38000, 33);
        }
    }
    if (pressed & kSave) saveLearned();
    if (pressed & kDelete) {  // D: discard and listen again
        g_learnHasFrame = false;
        g_learnDecoded  = false;
        note[0]         = 0;
        irrx::flush();
        dirty = true;
    }
}

// ---------------------------------------------------------------------------
// UNIVERSAL
// ---------------------------------------------------------------------------
void drawUniv()
{
    lcd.fillScreen(kBg);
    header("IR / UNIVERSAL");
    for (int r = 0; r < kRows - 1; ++r) {
        if (r >= kBrandCount) break;
        const int y = 17 + r * kRowH;
        if (r == univSel) lcd.fillRect(0, y - 1, lcd.width(), kRowH, kBar);
        lcd.setTextColor(r == univSel ? kAccent : kText, r == univSel ? kBar : kBg);
        lcd.drawString(kBrands[r].name, 4, y);
    }
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("common TVs, one frame each", 4, 108);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void drawUnivKeys()
{
    lcd.fillScreen(kBg);
    header(kBrands[univSel].name);
    const UBrand& b = kBrands[univSel];
    for (int r = 0; r < b.count && r < kRows - 1; ++r) {
        const int y = 17 + r * kRowH;
        if (r == univKeySel) lcd.fillRect(0, y - 1, lcd.width(), kRowH, kBar);
        lcd.setTextColor(r == univKeySel ? kAccent : kText, r == univKeySel ? kBar : kBg);
        lcd.drawString(b.keys[r].name, 4, y);
    }
    lcd.setTextColor(note[0] ? kAccent : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT send  S copy", 4, 108);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

ir::Signal ukeySignal(const UKey& k)
{
    ir::Signal s;
    s.proto   = k.proto;
    s.address = k.address;
    s.command = k.command;
    s.toggle  = false;
    return s;
}

void handleUniv(uint32_t pressed)
{
    if ((pressed & kUp) != 0 && univSel > 0) {
        --univSel;
        dirty = true;
    }
    if ((pressed & kDown) != 0 && univSel + 1 < kBrandCount) {
        ++univSel;
        dirty = true;
    }
    if (pressed & kEnter) {
        univKeySel = 0;
        page       = Page::kUnivKeys;
        note[0]    = 0;
        dirty      = true;
    }
    if (pressed & kBack) {
        page    = Page::kHome;
        note[0] = 0;
        dirty   = true;
    }
}

void handleUnivKeys(uint32_t pressed)
{
    const UBrand& b = kBrands[univSel];
    if ((pressed & kUp) != 0 && univKeySel > 0) {
        --univKeySel;
        dirty = true;
    }
    if ((pressed & kDown) != 0 && univKeySel + 1 < b.count) {
        ++univKeySel;
        dirty = true;
    }
    if (pressed & kEnter) sendSignal(ukeySignal(b.keys[univKeySel]));
    if (pressed & kSave) {
        const ir::Signal s = ukeySignal(b.keys[univKeySel]);
        char name[irstore::kNameLen];
        std::snprintf(name, sizeof(name), "%s %s", b.name, b.keys[univKeySel].name);
        if (irstore::find(s) >= 0) std::snprintf(note, sizeof(note), "ALREADY SAVED");
        else if (irstore::add(name, s) < 0) std::snprintf(note, sizeof(note), "LIBRARY FULL");
        else std::snprintf(note, sizeof(note), "SAVED");
        dirty = true;
    }
    if (pressed & kBack) {
        page    = Page::kUniv;
        note[0] = 0;
        dirty   = true;
    }
}

// ---------------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------------
int setupSel = 0;
constexpr int kSetupItems = 4;
bool clearArmed = false;
uint32_t clearArmedAt = 0;

void savePrefs()
{
    if (!prefsReady) return;
    prefs.putUChar("rxpin", rxPin);
    prefs.putUChar("reps", repeats);
}

void drawSetup()
{
    lcd.fillScreen(kBg);
    header("IR / SETUP");
    const char* labels[kSetupItems] = {"RX PIN", "REPEATS", "LED TEST", "CLEAR LIBRARY"};
    for (int i = 0; i < kSetupItems; ++i) {
        const int y = 17 + i * kRowH;
        if (i == setupSel) lcd.fillRect(0, y - 1, lcd.width(), kRowH, kBar);
        lcd.setTextColor(i == setupSel ? kAccent : kText, i == setupSel ? kBar : kBg);
        lcd.drawString(labels[i], 4, y);

        char value[20] = {0};
        if (i == 0) std::snprintf(value, sizeof(value), "G%u", static_cast<unsigned>(rxPin));
        else if (i == 1) std::snprintf(value, sizeof(value), "x%u", static_cast<unsigned>(repeats));
        else if (i == 2) std::snprintf(value, sizeof(value), "burst");
        else std::snprintf(value, sizeof(value), "%d codes", irstore::count());
        lcd.setTextColor(i == setupSel ? kAccent : kDim, i == setupSel ? kBar : kBg);
        lcd.drawRightString(value, lcd.width() - 4, y);
    }

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("TX on G44 (built in)", 4, 62);
    lcd.setTextColor(note[0] ? kAccent : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT change  esc back", 4, 108);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

void handleSetup(uint32_t pressed)
{
    if ((pressed & kUp) != 0 && setupSel > 0) {
        --setupSel;
        dirty = true;
    }
    if ((pressed & kDown) != 0 && setupSel + 1 < kSetupItems) {
        ++setupSel;
        dirty = true;
    }
    if (pressed & kEnter) {
        if (setupSel == 0) {
            rxPin = (rxPin == irrx::kPinG1) ? irrx::kPinG2 : irrx::kPinG1;
            std::snprintf(note, sizeof(note), "RX PIN G%u", static_cast<unsigned>(rxPin));
            savePrefs();
        } else if (setupSel == 1) {
            repeats = static_cast<uint8_t>(repeats >= 5 ? 1 : repeats + 1);
            std::snprintf(note, sizeof(note), "REPEATS x%u", static_cast<unsigned>(repeats));
            savePrefs();
        } else if (setupSel == 2) {
            // A phone camera sees this as a purple dot; the naked eye does not.
            // Blinking is easier to spot than one long burst, and a steady burst
            // the sensor might average away.
            std::snprintf(note, sizeof(note), "BLINKING - CHECK CAMERA");
            dirty = true;
            drawSetup();
            for (int i = 0; i < 8; ++i) {
                irtx::carrierBurst(180, 38000, 20);
                delay(220);
            }
            std::snprintf(note, sizeof(note), "LED TEST DONE");
        } else {
            if (!clearArmed) {
                clearArmed   = true;
                clearArmedAt = millis();
                std::snprintf(note, sizeof(note), "PRESS ENT AGAIN");
            } else {
                clearArmed = false;
                irstore::clear();
                std::snprintf(note, sizeof(note), "LIBRARY CLEARED");
            }
        }
        dirty = true;
    }
    if (pressed & kBack) {
        page    = Page::kHome;
        note[0] = 0;
        dirty   = true;
    }
}

// ---------------------------------------------------------------------------
// AIR CON
//
// An air conditioner is not a list of buttons: the temperature lives inside the
// frame, so every press has to describe the whole machine state.  The page
// therefore edits one state and sends it, and Gree's own 64-bit protocol does
// the rest (see ir_protocol.h).  The state is kept in NVS so the remote picks up
// where the last press left it.
// ---------------------------------------------------------------------------
ir::gree::State acState;
int acRow = 0;
constexpr int kAcRows = 6;
uint8_t acModelSel = 2;  // 0 = YAW1F, 1 = YAPOF, 2 = send both

uint8_t acModel() { return acModelSel == 1 ? ir::gree::kModelYapof : ir::gree::kModelYaw1F; }

bool acPower() { return (acState.raw[0] & 0x08) != 0; }
uint8_t acMode() { return static_cast<uint8_t>(acState.raw[0] & 0x07); }
uint8_t acFan() { return static_cast<uint8_t>((acState.raw[0] >> 4) & 0x03); }
uint8_t acTemp() { return static_cast<uint8_t>((acState.raw[1] & 0x0F) + ir::gree::kMinTempC); }
bool acSwing() { return (acState.raw[0] & 0x40) != 0; }

void acRebuild(bool power, uint8_t mode, uint8_t temp, uint8_t fan, bool swing)
{
    acState = ir::gree::makeState(power, mode, temp, fan, swing, true, acModel());
    if (prefsReady) prefs.putBytes("ac", acState.raw, ir::gree::kStateBytes);
    dirty = true;
}

void acLoadOrDefault()
{
    bool loaded = false;
    if (prefsReady && prefs.getBytesLength("ac") == ir::gree::kStateBytes) {
        ir::gree::State tmp;
        if (prefs.getBytes("ac", tmp.raw, ir::gree::kStateBytes) == ir::gree::kStateBytes &&
            ir::gree::valid(tmp)) {
            acState = tmp;
            loaded  = true;
        }
    }
    if (prefsReady) {
        const uint8_t sel = prefs.getUChar("acmodel", 2);
        acModelSel        = sel <= 2 ? sel : 2;
    }
    if (!loaded) {
        // Cold start: cooling, 26 C, fan automatic, swing off.
        acState = ir::gree::makeState(true, ir::gree::kModeCool, 26, ir::gree::kFanAuto, false,
                                      true, acModel());
    }
}

void enterAc()
{
    page    = Page::kAc;
    acRow   = 0;
    note[0] = 0;
    acLoadOrDefault();
    dirty = true;
}

void drawAc()
{
    lcd.fillScreen(kBg);
    header("IR / AIR CON");
    const char* labels[kAcRows] = {"POWER", "MODE", "TEMP", "FAN", "SWING", "MODEL"};
    char values[kAcRows][10];
    std::snprintf(values[0], sizeof(values[0]), "%s", acPower() ? "ON" : "OFF");
    std::snprintf(values[1], sizeof(values[1]), "%s", ir::gree::modeName(acMode()));
    std::snprintf(values[2], sizeof(values[2]), "%u C", static_cast<unsigned>(acTemp()));
    std::snprintf(values[3], sizeof(values[3]), "%s", ir::gree::fanName(acFan()));
    std::snprintf(values[4], sizeof(values[4]), "%s", acSwing() ? "ON" : "OFF");
    std::snprintf(values[5], sizeof(values[5]), "%s",
                  acModelSel == 0 ? "YAW1F" : (acModelSel == 1 ? "YAPOF" : "BOTH"));

    for (int i = 0; i < kAcRows; ++i) {
        const int y = 17 + i * kRowH;
        if (i == acRow) lcd.fillRect(0, y - 1, lcd.width(), kRowH, kBar);
        lcd.setTextColor(i == acRow ? kAccent : kText, i == acRow ? kBar : kBg);
        lcd.drawString(labels[i], 4, y);
        lcd.setTextColor(i == acRow ? kAccent : kOk, i == acRow ? kBar : kBg);
        lcd.drawRightString(values[i], lcd.width() - 4, y);
    }

    lcd.setTextColor(note[0] ? kAccent : kDim, kBg);
    lcd.drawString(note[0] ? note : "GREE 64-bit state", 4, 108);
    static const Hint h[] = {{"ENT", "send"}, {",/", "change"}, {"esc", "back"}};
    hints(h, 3);
}

void handleAc(uint32_t pressed)
{
    if ((pressed & kUp) != 0 && acRow > 0) {
        --acRow;
        dirty = true;
    }
    if ((pressed & kDown) != 0 && acRow + 1 < kAcRows) {
        ++acRow;
        dirty = true;
    }

    // `,` and `/` change the value of the selected row; a change never sends,
    // so bumping the temperature by six degrees is still one frame on air.
    const int delta = ((pressed & kRight) != 0) ? 1 : (((pressed & kLeft) != 0) ? -1 : 0);
    if (delta != 0) {
        switch (acRow) {
            case 0: acRebuild(!acPower(), acMode(), acTemp(), acFan(), acSwing()); break;
            case 1: {
                static const uint8_t kOrder[] = {ir::gree::kModeCool, ir::gree::kModeHeat,
                                                 ir::gree::kModeDry, ir::gree::kModeFan,
                                                 ir::gree::kModeAuto};
                int at = 0;
                for (int i = 0; i < 5; ++i) {
                    if (kOrder[i] == acMode()) at = i;
                }
                at = (at + delta + 5) % 5;
                acRebuild(acPower(), kOrder[at], acTemp(), acFan(), acSwing());
                break;
            }
            case 2:
                acRebuild(acPower(), acMode(), static_cast<uint8_t>(acTemp() + delta), acFan(),
                          acSwing());
                break;
            case 3: {
                const uint8_t fan = static_cast<uint8_t>((acFan() + delta + 4) % 4);
                acRebuild(acPower(), acMode(), acTemp(), fan, acSwing());
                break;
            }
            case 4: acRebuild(acPower(), acMode(), acTemp(), acFan(), !acSwing()); break;
            case 5:
                acModelSel = static_cast<uint8_t>((acModelSel + delta + 3) % 3);
                if (prefsReady) prefs.putUChar("acmodel", acModelSel);
                acState = ir::gree::makeState(acPower(), acMode(), acTemp(), acFan(), acSwing(),
                                             true, acModel());
                dirty = true;
                break;
            default: break;
        }
        std::snprintf(note, sizeof(note), "READY - ENT SENDS");
    }

    if (pressed & kEnter) {
        // BOTH sends each family once.  A unit ignores the frame it does not
        // understand and acts on the one it does, so the first press has a
        // chance of working without the operator guessing the model.
        const int frames = (acModelSel == 2) ? 2 : 1;
        bool ok = true;
        for (int i = 0; i < frames; ++i) {
            const uint8_t model = (acModelSel == 2)
                                      ? (i == 0 ? ir::gree::kModelYapof : ir::gree::kModelYaw1F)
                                      : acModel();
            const ir::gree::State st = ir::gree::makeState(acPower(), acMode(), acTemp(), acFan(),
                                                           acSwing(), true, model);
            ok = sendSignal(ir::gree::toSignal(st)) && ok;
            if (i + 1 < frames) delay(120);  // let the unit finish decoding
        }
        std::snprintf(note, sizeof(note), "SENT %s %uC %s x%d", ir::gree::modeName(acMode()),
                      static_cast<unsigned>(acTemp()), acPower() ? "ON" : "OFF", frames);
        if (!ok) std::snprintf(note, sizeof(note), "TX FAILED");
        dirty = true;
    }

    if (pressed & kSave) {
        const ir::Signal s = ir::gree::toSignal(acState);
        char name[irstore::kNameLen];
        std::snprintf(name, sizeof(name), "AC %s %uC", ir::gree::modeName(acMode()),
                      static_cast<unsigned>(acTemp()));
        if (irstore::find(s) >= 0) std::snprintf(note, sizeof(note), "ALREADY SAVED");
        else if (irstore::add(name, s) < 0) std::snprintf(note, sizeof(note), "LIBRARY FULL");
        else std::snprintf(note, sizeof(note), "SAVED");
        dirty = true;
    }

    if (pressed & kBack) {
        page    = Page::kHome;
        note[0] = 0;
        dirty   = true;
    }
}

// ---------------------------------------------------------------------------
// HOME
// ---------------------------------------------------------------------------
constexpr int kHomeItems = 6;
const char* const kHomeLabels[kHomeItems] = {"MY CODES", "AIR CON", "SD REMOTES", "LEARN",
                                             "UNIVERSAL", "SETUP"};

void drawHome()
{
    lcd.fillScreen(kBg);
    header("IR TOOLS");
    for (int i = 0; i < kHomeItems; ++i) {
        const int y = 16 + i * 13;
        if (i == homeSel) lcd.fillRect(2, y - 4, 200, 14, kBar);
        lcd.setTextColor(i == homeSel ? kAccent : kText, i == homeSel ? kBar : kBg);
        lcd.drawString(kHomeLabels[i], 8, y);
    }
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("TX G44   RX: ext module G1", 4, 100);
    lcd.setTextColor(note[0] ? kAccent : kDim, kBg);
    lcd.drawString(note[0] ? note : "", 4, 108);
    static const Hint h[] = {{"ENT", "open"}, {"esc", "back"}};
    hints(h, 2);
}

void handleHome(uint32_t pressed)
{
    if ((pressed & kUp) != 0 && homeSel > 0) {
        --homeSel;
        dirty = true;
    }
    if ((pressed & kDown) != 0 && homeSel + 1 < kHomeItems) {
        ++homeSel;
        dirty = true;
    }
    if (pressed & kEnter) {
        note[0] = 0;
        switch (homeSel) {
            case 0: enterMine(); break;
            case 1: enterAc(); break;
            case 2:
                page = Page::kFiles;
                std::snprintf(cwd, sizeof(cwd), "/ir");
                refreshFiles();
                break;
            case 3: enterLearn(); break;
            case 4:
                page = Page::kUniv;
                dirty = true;
                break;
            case 5:
                page = Page::kSetup;
                dirty = true;
                break;
            default: break;
        }
    }
    if (pressed & kBack) launcher::rebootInto(launcher::kAppMenu);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
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
            case 's': case 'S': a |= kSave; break;
            case 'd': case 'D': a |= kDelete; break;
            case ',': a |= kLeft; break;
            case '/': a |= kRight; break;
            default: break;
        }
    }
    return a;
}

void draw()
{
    switch (page) {
        case Page::kHome: drawHome(); break;
        case Page::kAc: drawAc(); break;
        case Page::kMine: drawMine(); break;
        case Page::kFiles: drawFiles(); break;
        case Page::kFileKeys: drawFileKeys(); break;
        case Page::kLearn: drawLearn(); break;
        case Page::kUniv: drawUniv(); break;
        case Page::kUnivKeys: drawUnivKeys(); break;
        case Page::kSetup: drawSetup(); break;
    }
    dirty = false;
}

// The learn page wants a live indicator even when nothing is pressed.
bool animated()
{
    if (page == Page::kLearn && rxStarted && !g_learnHasFrame) return true;
    if (clearArmed && millis() - clearArmedAt > 3000) {
        clearArmed = false;
        std::snprintf(note, sizeof(note), "CANCELLED");
        dirty = true;
    }
    if (deleteArmed && millis() - deleteArmedAt > 2000) {
        deleteArmed = false;
        if (strcmp(note, "PRESS D AGAIN") == 0) note[0] = 0;
        dirty = true;
    }
    return false;
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

    // Keep the two radio chips off the shared SPI bus (see sd_mount.h): the SD
    // card is used by the .ir browser.
    pinMode(RF_CC_CS, OUTPUT);
    digitalWrite(RF_CC_CS, HIGH);
    pinMode(RF_NFC_CS, OUTPUT);
    digitalWrite(RF_NFC_CS, HIGH);

    prefsReady = prefs.begin("ir", false);
    if (prefsReady) {
        rxPin   = prefs.getUChar("rxpin", irrx::kPinG1);
        repeats = prefs.getUChar("reps", 3);
    }
    if (rxPin != irrx::kPinG1 && rxPin != irrx::kPinG2) rxPin = irrx::kPinG1;
    if (repeats < 1 || repeats > 5) repeats = 3;

    irstore::begin();
    irtx::begin(irtx::kTxPin);

    page    = Page::kHome;
    homeSel = 0;
    dirty   = true;

    irLog("[ir] app ready: tx=%d codes=%d rx=G%u", static_cast<int>(irtx::ready()),
          irstore::count(), static_cast<unsigned>(rxPin));
}

void loop()
{
    M5Cardputer.update();

#if IR_BOOT_SELFTEST
    // Bring-up build only (env:ir-selftest).  USB CDC drops everything written
    // before the host opens the port, so the status line and a carrier burst are
    // repeated for the first few seconds: open the port afterwards and there is
    // still something to read, and the LED can be watched with a phone camera.
    {
        static int stage     = 0;
        static uint32_t next = 0;
        const uint32_t now   = millis();
        if (stage < 6 && now >= 1200 && now >= next) {
            // One carrier burst (what a phone camera can see) and one real NEC
            // frame (what the whole encode -> RMT write path needs), both with
            // their return value, so the log alone says whether the transmitter
            // actually accepted the data.
            const bool burst = irtx::carrierBurst(200, 38000, 20);

            ir::Signal probe;
            probe.proto   = ir::Protocol::kNec;
            probe.address = 0x04;  // LG mains code, the one in UNIVERSAL
            probe.command = 0x08;
            int timings   = 0;
            int sent      = 0;
            if (ir::encode(probe, &g_timings)) {
                timings = g_timings.count;
                sent    = irtx::send(g_timings.us, g_timings.count, g_timings.frequency,
                                     g_timings.dutyPercent, 1, 0)
                              ? 1
                              : 0;
            }

            // And the frame the air conditioner page would send: GREE, cooling,
            // 26 C, fan auto.  It has to survive the same encode -> RMT path.
            const ir::Signal ac = ir::gree::toSignal(ir::gree::makeState(
                true, ir::gree::kModeCool, 26, ir::gree::kFanAuto, false, true));
            int acTimings = 0;
            int acSent    = 0;
            if (ir::encode(ac, &g_timings)) {
                acTimings = g_timings.count;
                acSent    = irtx::send(g_timings.us, g_timings.count, g_timings.frequency,
                                       g_timings.dutyPercent, 1, 0)
                                ? 1
                                : 0;
            }

            irLog("[ir] selftest %d/6 tx=%d burst=%d nec_send=%d timings=%d gree_send=%d gree=%d",
                  stage + 1, static_cast<int>(irtx::ready()), static_cast<int>(burst), sent, timings,
                  acSent, acTimings);
            ++stage;
            next = millis() + 1500;
        }
    }
#endif

    // Capture first: the receiver runs entirely in the RMT interrupt, poll()
    // only drains what it produced.
    if (page == Page::kLearn && rxStarted && !g_learnHasFrame) {
        int count = 0;
        if (irrx::poll(g_learnUs, ir::kMaxTimings, &count) && count >= 4) {
            g_learnCount   = count;
            g_learnHasFrame = true;
            g_learnDecoded = ir::decode(g_learnUs, g_learnCount, &g_learnSig);
            std::snprintf(note, sizeof(note), "%s",
                          g_learnDecoded ? "DECODED" : "RAW - S saves to TF");
            irLog("[ir] learned %d edges, decoded=%d", g_learnCount,
                          static_cast<int>(g_learnDecoded));
            dirty = true;
        }
    }

    const uint32_t actions = readActions();
    const uint32_t pressed = actions & ~prevActions;
    prevActions            = actions;

    if (pressed != 0) {
        switch (page) {
            case Page::kHome: handleHome(pressed); break;
            case Page::kAc: handleAc(pressed); break;
            case Page::kMine: handleMine(pressed); break;
            case Page::kFiles: handleFiles(pressed); break;
            case Page::kFileKeys: handleFileKeys(pressed); break;
            case Page::kLearn: handleLearn(pressed); break;
            case Page::kUniv: handleUniv(pressed); break;
            case Page::kUnivKeys: handleUnivKeys(pressed); break;
            case Page::kSetup: handleSetup(pressed); break;
        }
    }

    if (animated()) dirty = true;
    if (dirty) draw();
    delay(5);
}

}  // namespace irapp
