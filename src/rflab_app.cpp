#include "rflab_app.h"

#include <M5Cardputer.h>
#include <SD.h>

#include <cstring>
#include <memory>

#include "cc1101.h"
#include "launcher.h"
#include "ook_decoder.h"
#include "ook_rx.h"
#include "ook_tx.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "rf_replay.h"
#include "rf_sub.h"
#include "saved_store.h"
#include "sd_mount.h"
#include "ui_hint.h"

namespace rflabapp {
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
Cc1101& rf_ = g_cc1101;
OokReceiver rx_;
OokTransmitter tx_;
SavedStore saved_;

enum class Screen : uint8_t { kHome, kSpectrum, kSub, kGen, kAnalyze, kEdit };
enum class EditMode : uint8_t { kList, kEdit };
Screen screen = Screen::kHome;
EditMode editMode = EditMode::kList;
bool dirty = true;
RfBand band = RfBand::k433;
uint32_t prevActions = 0;
char note[48] = {0};

// Spectrum
constexpr int kSpecCols = 40;
uint8_t specLevel[kSpecCols];
uint8_t waterfall[10][kSpecCols];
bool specRunning = false;
uint32_t lastSweepMs = 0;
int specPeakDbm = -110;

// SUB
char subNames[12][48];
int subCount = 0;
int subSel = 0;
int subTop = 0;

// Generator
int genProto = 0;  // 0 EV1527, 1 PT2262, 2 CAME, 3 Nice
uint32_t genCode = 0x123456;
int genBits = 24;
int genBase = 350;
int genField = 0;
int genSel = 0;

// Analyzer
int anaDbm = -110;
int anaHighPct = 0;
int anaEdges = 0;
const char* anaVerdict = "PRESS S";
char anaProto[24] = "NO FRAME";

// Editor
int editSel = 0;
int editTop = 0;
int editField = 0;
int editPulse = 0;
int editRepeat = 5;
int editOffset = 0;
int editBase = 350;
int editPulseWidth = 350;
int editTarget = -1;

enum : uint32_t {
    kUp = 1u << 0,
    kDown = 1u << 1,
    kLeft = 1u << 2,
    kRight = 1u << 3,
    kEnter = 1u << 4,
    kBack = 1u << 5,
    kSend = 1u << 6,
    kActionKey = 1u << 7,
    kScanKey = 1u << 8,
    kExport = 1u << 9,
};

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

void header(const char* title)
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(title, 3, 2);
    char b[20];
    snprintf(b, sizeof(b), "%s", rfcfg::bandName(band));
    lcd.drawRightString(b, lcd.width() - 3, 2);
}

uint8_t dbmLevel(int dbm)
{
    if (dbm < -100) dbm = -100;
    if (dbm > -30) dbm = -30;
    return static_cast<uint8_t>((dbm + 100) * 63 / 70);
}

uint16_t levelColor(uint8_t v)
{
    if (v < 16) return 0x0010;
    if (v < 32) return 0x07E0;
    if (v < 48) return 0xFFE0;
    return 0xF800;
}

uint32_t actionsNow()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t c = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (c) {
            case ';': a |= kUp; break;
            case '.': a |= kDown; break;
            case ',': a |= kLeft; break;
            case '/': a |= kRight; break;
            case KEY_ENTER: a |= kEnter; break;
            case '`': case '~': case 0x1B: a |= kBack; break;
            case 's': case 'S': a |= kSend; break;
            case 'b': case 'B': a |= kActionKey; break;
            case 'f': case 'F': a |= kScanKey; break;
            case 'x': case 'X': a |= kExport; break;
            default: break;
        }
    }
    return a;
}

bool savedToSignal(int index, rfsub::Signal& out)
{
    if (index < 0 || index >= saved_.size()) return false;
    const SavedSignal& s = saved_.at(index);
    out = rfsub::Signal{};
    out.band = s.band;
    out.proto = static_cast<RfProtocol>(s.proto);
    out.code = s.code;
    out.baseUs = s.baseUs;
    out.bits = s.bits;
    out.count = s.count > rfcfg::storedPulses ? rfcfg::storedPulses : s.count;
    for (int i = 0; i < out.count; ++i) out.pulses[i] = s.pulses[i];
    return out.count > 0;
}

bool signalToCapture(const rfsub::Signal& sig, Capture& c)
{
    if (sig.count < 8) return false;
    c = Capture{};
    c.band = sig.band;
    c.code = sig.code;
    c.baseUs = sig.baseUs;
    c.count = sig.count > rfcfg::storedPulses ? rfcfg::storedPulses : sig.count;
    c.bits = sig.bits;
    c.proto = static_cast<uint8_t>(sig.proto);
    c.valid = sig.proto != RfProtocol::kUnknown;
    for (int i = 0; i < c.count; ++i) c.pulses[i] = sig.pulses[i];
    return true;
}

// ---------------------------------------------------------------------------
// Spectrum
// ---------------------------------------------------------------------------

void sweepSpectrum()
{
    const uint32_t center = rfsub::bandHz(band);
    const uint32_t half = band == RfBand::k315 ? 600000u : 1800000u;
    const uint32_t start = center - half;
    const uint32_t step = (half * 2) / (kSpecCols - 1);
    int peak = -110;
    for (int i = 0; i < kSpecCols; ++i) {
        rf_.tuneTo(band, start + step * i);
        delay(1);
        const int dbm = rf_.rssiDbm();
        specLevel[i] = dbmLevel(dbm);
        if (dbm > peak) peak = dbm;
    }
    specPeakDbm = peak;
    for (int r = 9; r > 0; --r) memcpy(waterfall[r], waterfall[r - 1], kSpecCols);
    memcpy(waterfall[0], specLevel, kSpecCols);
    dirty = true;
}

void enterSpectrum()
{
    screen = Screen::kSpectrum;
    specRunning = true;
    memset(waterfall, 0, sizeof(waterfall));
    rx_.suspend();
    lastSweepMs = 0;
    sweepSpectrum();
}

void drawSpectrum()
{
    lcd.fillScreen(kBg);
    header("SPECTRUM");
    char info[48];
    snprintf(info, sizeof(info), "PEAK %d dBm  -100..-30", specPeakDbm);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(info, 3, 15);

    for (int i = 0; i < kSpecCols; ++i) {
        const int x = i * 6;
        const int h = 4 + (specLevel[i] * 34) / 63;
        lcd.fillRect(x, 49 - h, 5, h, levelColor(specLevel[i]));
    }
    for (int r = 0; r < 10; ++r) {
        for (int i = 0; i < kSpecCols; ++i) {
            lcd.fillRect(i * 6, 52 + r * 6, 5, 5, levelColor(waterfall[r][i]));
        }
    }
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(specRunning ? "RUNNING" : "HOLD", 3, 112);
    static const Hint h[] = {{"S", "run"}, {"F", "band"}, {"esc", "back"}};
    hints(h, 3);
}

// ---------------------------------------------------------------------------
// SUB
// ---------------------------------------------------------------------------

void refreshSub()
{
    subCount = 0;
    subSel = 0;
    subTop = 0;
    if (!sdmount::ensure(false)) {
        snprintf(note, sizeof(note), "NO TF CARD");
        return;
    }
    SD.mkdir("/subghz");
    File dir = SD.open("/subghz");
    if (!dir) {
        snprintf(note, sizeof(note), "OPEN /subghz FAILED");
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
        if (!f.isDirectory() && lower.endsWith(".sub") && subCount < 12) {
            strncpy(subNames[subCount], base, sizeof(subNames[0]) - 1);
            subNames[subCount][sizeof(subNames[0]) - 1] = 0;
            ++subCount;
        }
        f.close();
    }
    dir.close();
    note[0] = 0;
}

void drawSub()
{
    lcd.fillScreen(kBg);
    header("SUB /subghz");
    if (subCount == 0) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(note[0] ? note : "NO .sub FILES", 6, 30);
    }
    for (int r = 0; r < 7; ++r) {
        const int i = subTop + r;
        if (i >= subCount) break;
        const int y = 18 + r * 12;
        if (i == subSel) lcd.fillRect(0, y - 2, lcd.width(), 12, kBar);
        lcd.setTextColor(i == subSel ? kAccent : kText, i == subSel ? kBar : kBg);
        lcd.drawString(subNames[i], 5, y);
    }
    lcd.setTextColor(note[0] ? kWarn : kDim, kBg);
    lcd.drawString(note[0] ? note : "ENT import  E export saved  S rescan", 3, 108);
    static const Hint h[] = {{"ENT", "import"}, {"E", "export"}, {"esc", "back"}};
    hints(h, 3);
}

void importSub()
{
    if (subCount == 0) return;
    char path[80];
    snprintf(path, sizeof(path), "/subghz/%s", subNames[subSel]);
    rfsub::Signal sig;
    if (!rfsub::parseFile(path, sig)) {
        snprintf(note, sizeof(note), "PARSE FAILED");
        dirty = true;
        return;
    }
    Capture c;
    if (!signalToCapture(sig, c)) {
        snprintf(note, sizeof(note), "TOO SHORT");
        dirty = true;
        return;
    }
    int idx = -1;
    const auto res = saved_.add(c, &idx);
    if (res == SavedStore::AddResult::kFull) snprintf(note, sizeof(note), "SAVED LIST FULL");
    else snprintf(note, sizeof(note), "IMPORTED %s", protocolName(sig.proto));
    dirty = true;
}

void exportSaved()
{
    if (!sdmount::ensure(false)) {
        snprintf(note, sizeof(note), "NO TF CARD");
        dirty = true;
        return;
    }
    int ok = 0;
    for (int i = 0; i < saved_.size(); ++i) {
        char name[40];
        char path[80];
        rfsub::safeName(saved_.at(i).name, name, sizeof(name));
        snprintf(path, sizeof(path), "/subghz/%s.sub", name);
        if (rfsub::exportFile(path, saved_.at(i))) ++ok;
    }
    snprintf(note, sizeof(note), "EXPORTED %d", ok);
    dirty = true;
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

bool makeGenerated(rfsub::Signal& sig)
{
    bool ok = false;
    if (genProto == 0) ok = rfsub::buildEv1527(genCode, genBits, genBase, sig);
    else if (genProto == 1) ok = rfsub::buildPt2262(genCode, genBits, genBase, sig);
    else if (genProto == 2) ok = rfsub::buildCame(genCode, genBits, genBase, sig);
    else ok = rfsub::buildNice(genCode, genBits, genBase, sig);
    if (ok) sig.band = band;
    return ok;
}

void drawGen()
{
    lcd.fillScreen(kBg);
    header("GENERATE");
    const char* protos[] = {"EV1527", "PT2262", "CAME", "NICE"};
    char line[64];
    snprintf(line, sizeof(line), "PROTO  %s", protos[genProto]);
    lcd.setTextColor(genField == 0 ? kAccent : kText, kBg);
    lcd.drawString(line, 6, 22);
    snprintf(line, sizeof(line), "CODE   %06lX", static_cast<unsigned long>(genCode));
    lcd.setTextColor(genField == 1 ? kAccent : kText, kBg);
    lcd.drawString(line, 6, 36);
    snprintf(line, sizeof(line), "BITS   %d", genBits);
    lcd.setTextColor(genField == 2 ? kAccent : kText, kBg);
    lcd.drawString(line, 6, 50);
    snprintf(line, sizeof(line), "BASE   %d us", genBase);
    lcd.setTextColor(genField == 3 ? kAccent : kText, kBg);
    lcd.drawString(line, 6, 64);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("ENT save  S send  B +256 sweep", 4, 94);
    lcd.drawString(note[0] ? note : ", / adjust  ; . field", 4, 106);
    static const Hint h[] = {{"ENT", "save"}, {"S", "send"}, {"esc", "back"}};
    hints(h, 3);
}

void sendSignal(const rfsub::Signal& sig, int repeats)
{
    if (sig.count < 8) return;
    rf_.setBand(sig.band);
    rx_.suspend();
    tx_.send(sig.pulses, sig.count, repeats, 0);
    rx_.resume();
    rf_.setBand(band);
}

// ---------------------------------------------------------------------------
// Analyzer (envelope heuristic, not a demodulator)
// ---------------------------------------------------------------------------

void sampleAnalyzer()
{
    rx_.suspend();
    rf_.tuneTo(band, rfsub::bandHz(band));
    int high = 0;
    int edges = 0;
    int prev = 0;
    int peak = -110;
    for (int i = 0; i < 240; ++i) {
        const int dbm = rf_.rssiDbm();
        if (dbm > peak) peak = dbm;
        const int now = dbm > -80 ? 1 : 0;
        if (now != prev && i) ++edges;
        prev = now;
        if (now) ++high;
        delay(1);
    }
    anaDbm = peak;
    anaHighPct = high * 100 / 240;
    anaEdges = edges;
    if (anaHighPct < 35 && edges > 2) anaVerdict = "OOK / BURST";
    else if (anaHighPct > 70 && edges < 8) anaVerdict = "2-FSK/MSK?";
    else if (anaHighPct < 15) anaVerdict = "WEAK / NOISE";
    else anaVerdict = "UNKNOWN";
    rx_.resume();
    delay(20);
    RfFrame frame;
    snprintf(anaProto, sizeof(anaProto), "NO FRAME");
    if (rx_.poll(frame)) {
        const Decoded d = decodeFrame(frame);
        if (d.valid) {
            snprintf(anaProto, sizeof(anaProto), "%s %06lX", protocolName(d.proto),
                     static_cast<unsigned long>(d.code & 0xFFFFFFu));
        } else {
            uint32_t code;
            uint8_t bits;
            uint16_t base;
            const RfProtocol p = rfsub::decodeBrand(frame, code, bits, base);
            if (p != RfProtocol::kUnknown) {
                snprintf(anaProto, sizeof(anaProto), "%s %03lX", protocolName(p),
                         static_cast<unsigned long>(code & 0xFFFu));
            }
        }
    }
    dirty = true;
}

void drawAnalyze()
{
    lcd.fillScreen(kBg);
    header("MOD ANALYZE");
    char line[64];
    snprintf(line, sizeof(line), "RSSI   %d dBm", anaDbm);
    lcd.setTextColor(kText, kBg);
    lcd.drawString(line, 6, 24);
    snprintf(line, sizeof(line), "HIGH   %d%%   EDGES %d", anaHighPct, anaEdges);
    lcd.drawString(line, 6, 38);
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString(anaVerdict, 6, 58);
    lcd.setTextColor(kText, kBg);
    lcd.drawString(anaProto, 6, 70);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Envelope heuristic only;", 6, 88);
    lcd.drawString("not an FSK/MSK demodulator.", 6, 100);
    static const Hint h[] = {{"S", "sample"}, {"F", "band"}, {"esc", "back"}};
    hints(h, 3);
}

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

void drawEditList()
{
    lcd.fillScreen(kBg);
    header("EDIT / SEND");
    if (saved_.empty()) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("NO SAVED SIGNAL", 6, 30);
    }
    for (int r = 0; r < 7; ++r) {
        const int i = editTop + r;
        if (i >= saved_.size()) break;
        const int y = 18 + r * 12;
        if (i == editSel) lcd.fillRect(0, y - 2, lcd.width(), 12, kBar);
        lcd.setTextColor(i == editSel ? kAccent : kText, i == editSel ? kBar : kBg);
        lcd.drawString(saved_.at(i).name, 5, y);
        lcd.setTextColor(i == editSel ? kAccent : kDim, i == editSel ? kBar : kBg);
        lcd.drawRightString(protocolName(static_cast<RfProtocol>(saved_.at(i).proto)),
                            lcd.width() - 4, y);
    }
    static const Hint h[] = {{"ENT", "edit"}, {"esc", "back"}};
    hints(h, 2);
}

void drawEditor()
{
    if (editTarget < 0 || editTarget >= saved_.size()) return;
    const SavedSignal& s = saved_.at(editTarget);
    lcd.fillScreen(kBg);
    header("EDIT SIGNAL");
    char line[64];
    lcd.setTextColor(kText, kBg);
    lcd.drawString(s.name, 4, 14);
    snprintf(line, sizeof(line), "PULSE %d/%u", editPulse + 1, s.count);
    lcd.setTextColor(editField == 0 ? kAccent : kText, kBg);
    lcd.drawString(line, 4, 28);
    snprintf(line, sizeof(line), "WIDTH %d us", editPulseWidth);
    lcd.setTextColor(editField == 1 ? kAccent : kText, kBg);
    lcd.drawString(line, 4, 42);
    snprintf(line, sizeof(line), "BASE  %d us", editBase);
    lcd.setTextColor(editField == 2 ? kAccent : kText, kBg);
    lcd.drawString(line, 4, 56);
    snprintf(line, sizeof(line), "REPEAT %d", editRepeat);
    lcd.setTextColor(editField == 3 ? kAccent : kText, kBg);
    lcd.drawString(line, 4, 70);
    snprintf(line, sizeof(line), "OFFSET %+d kHz", editOffset / 1000);
    lcd.setTextColor(editField == 4 ? kAccent : kText, kBg);
    lcd.drawString(line, 4, 84);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("ENT save  S send  X export", 4, 98);
    if (note[0]) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString(note, 4, 108);
    }
    static const Hint h[] = {{"; .", "field"}, {", /", "adjust"}, {"esc", "back"}};
    hints(h, 3);
}

void enterEditor(int index)
{
    if (index < 0 || index >= saved_.size()) return;
    editTarget = index;
    editMode = EditMode::kEdit;
    editField = 0;
    editPulse = 0;
    editBase = saved_.at(index).baseUs ? saved_.at(index).baseUs : 350;
    editPulseWidth = saved_.at(index).count ? saved_.at(index).pulses[0] : editBase;
    editRepeat = 5;
    editOffset = 0;
    dirty = true;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

void render()
{
    switch (screen) {
        case Screen::kHome: {
            lcd.fillScreen(kBg);
            header("RF LAB");
            const char* items[] = {"SPECTRUM", "SUB FILES", "GENERATE", "MOD ANALYZE", "EDIT / SEND"};
            lcd.setTextSize(2);
            for (int i = 0; i < 5; ++i) {
                const int y = 18 + i * 18;
                if (i == genSel) lcd.fillRect(4, y - 3, lcd.width() - 8, 18, kBar);
                lcd.setTextColor(i == genSel ? kAccent : kText, i == genSel ? kBar : kBg);
                lcd.drawString(items[i], 10, y);
            }
            lcd.setTextSize(1);
            lcd.setTextColor(kDim, kBg);
            lcd.drawString("RF Lab: experimentation only.", 5, 108);
            static const Hint h[] = {{"; .", "select"}, {"ENT", "open"}, {"esc", "back"}};
            hints(h, 3);
            break;
        }
        case Screen::kSpectrum: drawSpectrum(); break;
        case Screen::kSub: drawSub(); break;
        case Screen::kGen: drawGen(); break;
        case Screen::kAnalyze: drawAnalyze(); break;
        case Screen::kEdit:
            if (editMode == EditMode::kList) drawEditList();
            else drawEditor();
            break;
    }
    dirty = false;
}

void handleHome(uint32_t p)
{
    if (p & kBack) {
        launcher::rebootInto(launcher::kAppMenu);
        return;
    }
    if (p & kUp) {
        genSel = (genSel + 4) % 5;
        dirty = true;
    }
    if (p & kDown) {
        genSel = (genSel + 1) % 5;
        dirty = true;
    }
    if (p & kEnter) {
        if (genSel == 0) enterSpectrum();
        else if (genSel == 1) {
            screen = Screen::kSub;
            refreshSub();
            dirty = true;
        } else if (genSel == 2) {
            screen = Screen::kGen;
            dirty = true;
        } else if (genSel == 3) {
            screen = Screen::kAnalyze;
            anaVerdict = "PRESS S";
            dirty = true;
        } else {
            screen = Screen::kEdit;
            editMode = EditMode::kList;
            dirty = true;
        }
    }
}

void handleSpectrum(uint32_t p)
{
    if (p & kBack) {
        rx_.resume();
        screen = Screen::kHome;
        dirty = true;
        return;
    }
    if (p & kSend) {
        specRunning = !specRunning;
        dirty = true;
    }
    if (p & kScanKey) {
        band = band == RfBand::k315 ? RfBand::k433 : (band == RfBand::k433 ? RfBand::k868 : RfBand::k315);
        sweepSpectrum();
    }
}

void handleSub(uint32_t p)
{
    if (p & kBack) {
        screen = Screen::kHome;
        dirty = true;
        return;
    }
    if (p & kUp && subSel > 0) {
        --subSel;
        if (subSel < subTop) subTop = subSel;
        dirty = true;
    }
    if (p & kDown && subSel + 1 < subCount) {
        ++subSel;
        if (subSel >= subTop + 7) subTop = subSel - 6;
        dirty = true;
    }
    if (p & kEnter) importSub();
    if (p & kActionKey) exportSaved();
    if (p & kSend) {
        refreshSub();
        dirty = true;
    }
}

void handleGen(uint32_t p)
{
    if (p & kBack) {
        screen = Screen::kHome;
        dirty = true;
        return;
    }
    if (p & kUp) {
        genField = (genField + 3) % 4;
        dirty = true;
    }
    if (p & kDown) {
        genField = (genField + 1) % 4;
        dirty = true;
    }
    const int delta = (p & kRight) ? 1 : ((p & kLeft) ? -1 : 0);
    if (delta) {
        if (genField == 0) {
            genProto = (genProto + delta + 4) % 4;
            genBits = genProto == 0 ? 24 : 12;
            genBase = genProto == 3 ? 700 : (genProto == 2 ? 320 : 350);
        } else if (genField == 1) {
            const uint32_t mask = genBits >= 32 ? 0xFFFFFFFFu : ((1u << genBits) - 1u);
            genCode = (genCode + delta) & mask;
        } else if (genField == 2) {
            genBits = genBits == 12 ? 24 : 12;
            if (genProto != 0) genBits = 12;
        } else {
            genBase += delta * 10;
            if (genBase < 100) genBase = 100;
            if (genBase > 1000) genBase = 1000;
        }
        dirty = true;
    }
    if (p & kEnter) {
        rfsub::Signal sig;
        if (!makeGenerated(sig)) {
            snprintf(note, sizeof(note), "BUILD FAILED");
        } else {
            Capture c;
            if (signalToCapture(sig, c)) {
                saved_.add(c);
                snprintf(note, sizeof(note), "SAVED");
            }
        }
        dirty = true;
    }
    if (p & kSend) {
        rfsub::Signal sig;
        if (makeGenerated(sig)) {
            sendSignal(sig, 5);
            snprintf(note, sizeof(note), "SENT");
        }
        dirty = true;
    }
    if ((p & kActionKey) && genProto <= 1) {
        rfsub::Signal sig;
        if (makeGenerated(sig)) {
            for (int i = 0; i < 256; ++i) {
                genCode = (genCode + 1) & ((1u << genBits) - 1u);
                if (!makeGenerated(sig)) continue;
                sendSignal(sig, 3);
                M5Cardputer.update();
                if (M5Cardputer.Keyboard.isKeyPressed('`')) break;
            }
            snprintf(note, sizeof(note), "SWEEP DONE");
            dirty = true;
        }
    }
}

void handleAnalyze(uint32_t p)
{
    if (p & kBack) {
        screen = Screen::kHome;
        dirty = true;
        return;
    }
    if (p & kScanKey) {
        band = band == RfBand::k315 ? RfBand::k433 : (band == RfBand::k433 ? RfBand::k868 : RfBand::k315);
        sampleAnalyzer();
    }
    if (p & kSend) sampleAnalyzer();
}

void handleEdit(uint32_t p)
{
    if (editMode == EditMode::kList) {
        if (p & kBack) {
            screen = Screen::kHome;
            dirty = true;
            return;
        }
        if (p & kUp && editSel > 0) {
            --editSel;
            if (editSel < editTop) editTop = editSel;
            dirty = true;
        }
        if (p & kDown && editSel + 1 < saved_.size()) {
            ++editSel;
            if (editSel >= editTop + 7) editTop = editSel - 6;
            dirty = true;
        }
        if (p & kEnter) enterEditor(editSel);
        return;
    }

    if (p & kBack) {
        editMode = EditMode::kList;
        dirty = true;
        return;
    }
    if (p & kUp) {
        editField = (editField + 4) % 5;
        dirty = true;
    }
    if (p & kDown) {
        editField = (editField + 1) % 5;
        dirty = true;
    }
    const int delta = (p & kRight) ? 1 : ((p & kLeft) ? -1 : 0);
    if (delta && editTarget >= 0 && editTarget < saved_.size()) {
        SavedSignal& s = saved_.at(editTarget);
        if (editField == 0) {
            if (s.count > 1) {
                editPulse = (editPulse + delta + s.count) % s.count;
                editPulseWidth = s.pulses[editPulse];
            }
        } else if (editField == 1) {
            editPulseWidth += delta * 10;
            if (editPulseWidth < 50) editPulseWidth = 50;
            if (editPulseWidth > 20000) editPulseWidth = 20000;
            if (editPulse < s.count) s.pulses[editPulse] = static_cast<uint16_t>(editPulseWidth);
        } else if (editField == 2) {
            editBase += delta * 10;
            if (editBase < 50) editBase = 50;
            if (editBase > 20000) editBase = 20000;
        } else if (editField == 3) {
            editRepeat += delta;
            if (editRepeat < 1) editRepeat = 1;
            if (editRepeat > 40) editRepeat = 40;
        } else {
            editOffset += delta * 10000;
            if (editOffset < -500000) editOffset = -500000;
            if (editOffset > 500000) editOffset = 500000;
        }
        dirty = true;
    }
    if (p & kEnter) {
        if (editTarget >= 0 && editTarget < saved_.size()) {
            SavedSignal& s = saved_.at(editTarget);
            s.baseUs = static_cast<uint16_t>(editBase);
            saved_.save();
            snprintf(note, sizeof(note), "SAVED");
        }
        dirty = true;
    }
    if (p & kExport) {
        if (editTarget >= 0 && editTarget < saved_.size() && sdmount::ensure(false)) {
            char name[40], path[80];
            rfsub::safeName(saved_.at(editTarget).name, name, sizeof(name));
            snprintf(path, sizeof(path), "/subghz/%s.sub", name);
            snprintf(note, sizeof(note), rfsub::exportFile(path, saved_.at(editTarget))
                                            ? "EXPORTED"
                                            : "EXPORT FAILED");
        }
        dirty = true;
    }
    if (p & kSend) {
        if (editTarget >= 0 && editTarget < saved_.size()) {
            SavedSignal& s = saved_.at(editTarget);
            rf_.setBand(s.band);
            rf_.tuneTo(s.band, rfsub::bandHz(s.band) + editOffset);
            rx_.suspend();
            tx_.send(s.pulses, s.count, editRepeat, 0);
            rx_.resume();
            rf_.setBand(band);
            snprintf(note, sizeof(note), "SENT x%d", editRepeat);
        }
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
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    lcd.fillScreen(kBg);

    rf_.begin();
    rx_.begin(rfcfg::gdo0Pin);
    tx_.begin(rfcfg::gdo0Pin);
    saved_.begin();
    band = RfBand::k433;
    rf_.setBand(band);
    dirty = true;
}

void loop()
{
    M5Cardputer.update();
    const uint32_t actions = actionsNow();
    const uint32_t p = actions & ~prevActions;
    prevActions = actions;

    if (screen == Screen::kSpectrum && specRunning && millis() - lastSweepMs > 180) {
        lastSweepMs = millis();
        sweepSpectrum();
    }

    switch (screen) {
        case Screen::kHome: handleHome(p); break;
        case Screen::kSpectrum: handleSpectrum(p); break;
        case Screen::kSub: handleSub(p); break;
        case Screen::kGen: handleGen(p); break;
        case Screen::kAnalyze: handleAnalyze(p); break;
        case Screen::kEdit: handleEdit(p); break;
    }
    if (dirty) render();
}

}  // namespace rflabapp
