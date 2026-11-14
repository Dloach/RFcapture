#include "launcher.h"

#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>

#include "cc1101.h"
#include "nfc_app.h"
#include "ook_tx.h"
#include "polling_keyboard.h"
#include "quick_bind.h"
#include "rf_replay.h"
#include "saved_store.h"
#include "ui_hint.h"
#include "wifi_store.h"

namespace launcher {
namespace {

constexpr char kNs[] = "appboot";

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;

auto& lcd = M5Cardputer.Display;

int sel = 0;
int menuTop = 0;
int gridPage = 0;  // 0 = main, 1 = Fn, 2 = Aa/Shift, 3 = Opt
bool dirty = true;
bool lastWifiConnected = false;
bool wifiStarted       = false;

// The launcher list scrolls four rows at a time.  Keep the order here separate
// from the App numeric values: BACKUP must stay 3 and KEYBOARD 4 because those
// bytes are already in NVS appboot.
constexpr int kMenuVisible = 4;
const char* const kMenuItems[] = {"RF TOOLS", "NFC TOOLS", "KEYBOARD", "SSH", "SETTINGS"};
constexpr int kMenuItemCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
const App kMenuApps[] = {kAppRf, kAppNfc, kAppKbd, kAppSsh, kAppSettings};

char status[32] = {0};  // one-line result of the last quick action

// Which grid page the modifier keys currently select.  Holding Fn/Aa/Opt shows
// that page; releasing returns to MAIN.  Priority is Fn > Aa > Opt so a stuck
// modifier cannot hide the others.
int pageFromModifiers()
{
    const auto& ks = M5Cardputer.Keyboard.keysState();
    if (ks.fn) return 1;
    if (ks.shift) return 2;
    if (ks.opt) return 3;
    return 0;
}

// ---------------------------------------------------------------------------
// RF quick send, straight from the home page -- no reboot.
//
// The CC1101 is a plain SPI-mode-0 device that radiates only while a burst is
// on the air, so unlike the ST25R3916 (whose carrier stays on across reboots)
// it can live on the launcher page: initialised once at boot when any slot is
// bound to an RF signal, parked with idle() between bursts.  Only the TX side
// is set up; the OOK receiver is never started here.
// ---------------------------------------------------------------------------
SavedStore saved;
OokTransmitter tx;
bool rfReady = false;
bool txReady = false;

void rfSetupIfBound()
{
    bool need = false;
    for (int page = 0; page < quickbind::kPages; ++page) {
        for (int i = 0; i < quickbind::kSlots; ++i) {
            if (quickbind::slots[page][i].kind == quickbind::kRf) need = true;
        }
    }
    if (!need) return;

    saved.begin();
    rfReady = g_cc1101.begin();
    txReady = tx.begin(rfcfg::gdo0Pin);
    g_cc1101.idle();
    Serial.printf("[launcher] quick-rf rf=%d tx=%d saved=%d\n", static_cast<int>(rfReady),
                  static_cast<int>(txReady), saved.size());
}

uint32_t savedKey(const SavedSignal& s)
{
    return quickbind::keyForRf(static_cast<uint8_t>(s.band), s.proto != 0, s.code, s.bits,
                               s.pulses, s.count);
}

void quickSendRf(const quickbind::Binding& b)
{
    if (!rfReady || !txReady) {
        snprintf(status, sizeof(status), "RF not ready");
        dirty = true;
        return;
    }

    // Resolve the bound signal by its stable key, not by its old array index:
    // deleting an earlier signal shifts every later index, and the previous
    // range check would then send whichever signal inherited that slot.
    int idx = -1;
    if (b.key != 0) {
        for (int i = 0; i < saved.size(); ++i) {
            if (savedKey(saved.at(i)) == b.key) {
                idx = i;
                break;
            }
        }
    } else if (b.index < saved.size()) {
        // Legacy binding written before keys existed: use the index once, and
        // let the next re-bind store a key.
        idx = b.index;
    }
    if (idx < 0) {
        snprintf(status, sizeof(status), "BINDING STALE");
        dirty = true;
        return;
    }

    const SavedSignal& s = saved.at(idx);

    g_cc1101.setBand(s.band);
    g_cc1101.enterTx();
    delay(3);

    int ts = 0, te = s.count;
    rfreplay::wholeFrameRange(s.pulses, s.count, s.baseUs, &ts, &te);
    const uint16_t* train  = s.pulses + ts;
    const int trainCount   = te - ts;
    const int reps         = rfreplay::planRepeats(train, trainCount, nullptr);
    const bool ok          = tx.send(train, trainCount, reps, 0);

    // The PA stage can leave the chip in an odd state; re-apply the band and
    // park it, the same recovery src/app.cpp's transmit() does.
    g_cc1101.setBand(s.band);
    g_cc1101.idle();

    snprintf(status, sizeof(status), "%s", ok ? "TX ok" : "TX FAILED");
    Serial.printf("[quick] rf '%s' %s %06lX x%d -> %s\n", s.name, rfcfg::bandName(s.band),
                  static_cast<unsigned long>(s.code & 0xFFFFFFu), reps, ok ? "ok" : "failed");
    dirty = true;

    // The burst blocks for ~700 ms; drain the keys piled up meanwhile.
    for (int i = 0; i < 10; ++i) {
        M5Cardputer.update();
        delay(1);
    }
}

void quickEmulateNfc(const quickbind::Binding& b)
{
    nfcapp::queueEmulation(b.index, b.key);  // writes the nfcretry boot flag
    rebootInto(kAppNfc);                     // does not return
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------
void drawWifiIcon(int x, int y)
{
    const bool on = WiFi.status() == WL_CONNECTED;
    const uint16_t c = on ? kOk : kDim;
    // Three ascending bars, small enough for the 13 px header.
    lcd.fillRect(x, y + 5, 2, 4, c);
    lcd.fillRect(x + 4, y + 3, 2, 6, c);
    lcd.fillRect(x + 8, y + 1, 2, 8, c);
    if (!on) lcd.drawLine(x - 1, y + 9, x + 11, y, kWarn);
}

void draw()
{
    lcd.fillScreen(kBg);
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("CARDPUTER", 3, 2);
    lcd.setTextColor(kAccent, kHead);
    lcd.drawString(quickbind::pageName(gridPage), 80, 2);

    drawWifiIcon(190, 2);

    const int level = M5.Power.getBatteryLevel();
    char bat[12];
    snprintf(bat, sizeof(bat), level < 0 ? "--" : "%d%%", level);
    lcd.setTextColor(kText, kHead);
    lcd.drawRightString(bat, lcd.width() - 3, 2);

    // Left: a four-row scrolling window over the feature list.  Highlight is
    // colour + bar only, no ">" prefix: at text size 2 the marker pushed
    // "NFC TOOLS" to 120 px wide, spilling over the divider onto the grid.
    lcd.setTextSize(2);
    for (int row = 0; row < kMenuVisible; ++row) {
        const int idx = menuTop + row;
        if (idx >= kMenuItemCount) break;
        const int y = 22 + row * 22;
        if (idx == sel) lcd.fillRect(2, y - 4, 112, 20, kBar);
        lcd.setTextColor(idx == sel ? kAccent : kText, idx == sel ? kBar : kBg);
        lcd.drawString(kMenuItems[idx], 8, y);
    }
    lcd.setTextSize(1);

    // Scroll affordances at the far left edge, clear of the text.
    if (menuTop > 0) {
        lcd.setTextColor(kAccent, kBg);
        lcd.drawString("^", 108, 14);
    }
    if (menuTop + kMenuVisible < kMenuItemCount) {
        lcd.setTextColor(kAccent, kBg);
        lcd.drawString("v", 108, 94);
    }

    if (status[0]) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(status, 4, 108);
    }

    // Right: the 3x3 quick-bind grid for the current modifier page.
    lcd.drawFastVLine(118, 16, 98, kDim);
    for (int s = 0; s < quickbind::kSlots; ++s) {
        const int gx = 124 + (s % 3) * 38;
        const int gy = 16 + (s / 3) * 33;
        quickbind::drawSlotCell(lcd, gx, gy, 36, 31, gridPage, s, false);
    }

    lcd.drawFastHLine(0, 118, lcd.width(), kDim);
    drawMoveTriangles(lcd, 4, 122, kOk);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("move", 24, 122);
    lcd.setTextColor(kOk, kBg);
    lcd.drawString("ENT", 62, 122);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("open", 84, 122);
}

// ---------------------------------------------------------------------------
// Reboot splash
//
// The LCD keeps its framebuffer across an MCU reset, so this screen stays
// visible until the next feature draws its first frame.  It used to be a line
// of text ("ENTERING ... / restarting...") that vanished before anyone could
// read it; a filling bar communicates the same thing in one glance and gives
// the ~2 s feature bring-up a visual anchor.
// ---------------------------------------------------------------------------
constexpr int kBootBarX = 24;
constexpr int kBootBarY = 86;
constexpr int kBootBarW = 192;
constexpr int kBootBarH = 14;

const char* const kBootName[] = {"MENU", "RF TOOLS", "NFC TOOLS", "BACKUP", "KEYBOARD", "SETTINGS",
                                 "SSH"};

void drawBootBase(App app)
{
    lcd.fillScreen(kBg);
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("CARDPUTER", 3, 2);

    const char* name = kBootName[app <= kAppSsh ? app : 0];
    lcd.setTextSize(2);
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString(name, (lcd.width() - lcd.textWidth(name)) / 2, 52);
    lcd.setTextSize(1);

    // Frame; the interior is repainted by drawBootProgress().
    lcd.drawRect(kBootBarX, kBootBarY, kBootBarW, kBootBarH, kDim);
    lcd.fillRect(kBootBarX + 1, kBootBarY + 1, kBootBarW - 2, kBootBarH - 2, kBg);
}

void drawBootProgress(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const int innerW = kBootBarW - 2;
    const int fill   = innerW * percent / 100;
    lcd.fillRect(kBootBarX + 1, kBootBarY + 1, innerW, kBootBarH - 2, kBg);
    if (fill > 0) {
        lcd.fillRect(kBootBarX + 1, kBootBarY + 1, fill, kBootBarH - 2, kOk);
    }
}

}  // namespace

App currentApp()
{
    // A cold start always lands on the chooser.  The stored choice exists only so
    // that a *software* restart -- the NFC emulator re-arming itself, or a feature
    // handing control back -- lands where it intended.  Without this the device
    // booted straight into whichever feature was used last, which is not what
    // "the first screen is a choice" means.
    const esp_reset_reason_t why = esp_reset_reason();
    if (why == ESP_RST_POWERON || why == ESP_RST_BROWNOUT || why == ESP_RST_EXT) {
        return kAppMenu;
    }

    Preferences p;
    if (!p.begin(kNs, true)) return kAppMenu;
    const uint8_t v = p.getUChar("app", kAppMenu);
    p.end();
    return v <= kAppSsh ? static_cast<App>(v) : kAppMenu;
}

void rebootInto(App app)
{
    Preferences p;
    if (p.begin(kNs, false)) {
        p.putUChar("app", static_cast<uint8_t>(app));
        p.end();
    }

    // The LCD keeps its framebuffer across an MCU reset, so this splash stays
    // on screen until the next feature draws its first frame.  Fill the bar
    // before restarting: the visible motion is what tells the operator the
    // press was accepted, and the filled frame remains during feature bring-up.
    Serial.printf("[launcher] restart into app %u\n", static_cast<unsigned>(app));
    Serial.flush();

    drawBootBase(app);
    for (int percent = 0; percent <= 100; percent += 4) {
        drawBootProgress(percent);
        delay(8);
    }

    delay(30);
    ESP.restart();
}

void menuSetup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    // Same polling reader the features use: the stock edge-driven reader latches
    // the TCA8418 low and the keyboard dies after a handful of presses.
    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));

    lcd.setRotation(rfcfg::screenRotation);
    lcd.setBrightness(90);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);

    Serial.printf("[launcher] menu\n");

    quickbind::load();
    rfSetupIfBound();

    // Attach the saved WiFi network once per boot, in the background.  The
    // header icon updates when it lands; SETTINGS is still where credentials
    // are changed.  Do not re-run this when SETTINGS returns without a reboot.
    if (!wifiStarted) {
        wifiStarted = true;
        wifistore::begin();
        // A soft return from SETTINGS already has a live station link; do not
        // drop it just because this is the first menu bring-up in this boot.
        if (WiFi.status() != WL_CONNECTED && wifistore::count() > 0) {
            const auto* n = wifistore::last();
            if (n != nullptr) {
                WiFi.mode(WIFI_STA);
                WiFi.persistent(false);
                WiFi.setAutoReconnect(true);
                WiFi.begin(n->ssid, n->pass);
                Serial.printf("[launcher] wifi auto-connect '%s'\n", n->ssid);
            }
        }
    }

    lastWifiConnected = WiFi.status() == WL_CONNECTED;
    dirty = true;
    draw();
}

void menuLoop()
{
    M5Cardputer.update();

    // The WiFi stack may outlive SETTINGS when the user returns without a
    // reboot; keep the header icon honest while sitting on the launcher.
    static uint32_t lastWifiPoll = 0;
    if (millis() - lastWifiPoll >= 1000) {
        lastWifiPoll = millis();
        const bool now = WiFi.status() == WL_CONNECTED;
        if (now != lastWifiConnected) {
            lastWifiConnected = now;
            dirty            = true;
        }
    }

    // Fn / Aa / Opt select grid pages 1 / 2 / 3 while held.  MAIN is the
    // default page when no modifier is down.
    const int newPage = pageFromModifiers();
    if (newPage != gridPage) {
        gridPage = newPage;
        dirty    = true;
    }

    uint32_t a = 0;
    int quickSlot = -1;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        // ; = up, . = down.  The j/k aliases are gone here on purpose: J and K
        // are grid keys on this page.
        if (code == ';') {
            a |= 1u << 0;
        } else if (code == '.') {
            a |= 1u << 1;
        } else if (code == KEY_ENTER) {
            a |= 1u << 2;
        } else {
            const int qs = quickbind::slotFor(code);
            if (qs >= 0) quickSlot = qs;
        }
    }
    static uint32_t prev = 0;
    static int prevSlot  = -1;
    const uint32_t pressed = a & ~prev;
    const int slotPressed  = (quickSlot >= 0 && quickSlot != prevSlot) ? quickSlot : -1;
    prev                   = a;
    prevSlot               = quickSlot;

    if (pressed & (1u << 0)) {
        sel     = (sel + kMenuItemCount - 1) % kMenuItemCount;  // up, wrapping
        dirty   = true;
    }
    if (pressed & (1u << 1)) {
        sel     = (sel + 1) % kMenuItemCount;  // down, wrapping
        dirty   = true;
    }
    if (sel < menuTop) menuTop = sel;
    if (sel >= menuTop + kMenuVisible) menuTop = sel - kMenuVisible + 1;

    if (pressed & (1u << 2)) {
        // does not return.  Keep the App numeric values stable; the menu order
        // is independent of the NVS appboot values.
        rebootInto(kMenuApps[sel]);
    }

    if (slotPressed >= 0) {
        const quickbind::Binding& b = quickbind::slots[gridPage][slotPressed];
        if (b.kind == quickbind::kRf) {
            quickSendRf(b);
        } else if (b.kind == quickbind::kNfc) {
            quickEmulateNfc(b);  // does not return
        } else {
            snprintf(status, sizeof(status), "%s/%s: empty", quickbind::pageName(gridPage),
                     quickbind::keyLegend(slotPressed));
            dirty = true;
        }
    }

    if (dirty) {
        dirty = false;
        draw();
    }
    delay(5);
}

}  // namespace launcher
