#include "launcher.h"

#include <M5Cardputer.h>
#include <Preferences.h>
#include <esp_system.h>

#include "launcher.h"
#include "polling_keyboard.h"

namespace launcher {
namespace {

constexpr char kNs[] = "appboot";

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;

auto& lcd = M5Cardputer.Display;

int sel = 0;
bool dirty = true;

void draw()
{
    lcd.fillScreen(kBg);
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("CARDPUTER", 3, 2);

    const int level = M5.Power.getBatteryLevel();
    char bat[12];
    snprintf(bat, sizeof(bat), level < 0 ? "--" : "%d%%", level);
    lcd.setTextColor(kText, kHead);
    lcd.drawRightString(bat, lcd.width() - 3, 2);

    const char* items[] = {"RF TOOLS", "NFC TOOLS", "BACKUP"};
    lcd.setTextSize(2);
    for (int i = 0; i < 3; ++i) {
        const int y = 24 + i * 24;
        if (i == sel) lcd.fillRect(4, y - 5, lcd.width() - 8, 21, kBar);
        lcd.setTextColor(i == sel ? kAccent : kText, i == sel ? kBar : kBg);
        lcd.setCursor(16, y);
        lcd.printf("%s %s", i == sel ? ">" : " ", items[i]);
    }
    lcd.setTextSize(1);

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("RF capture  |  NFC read/emulate  |  TF backup", 4, 100);

    lcd.drawFastHLine(0, 118, lcd.width(), kDim);
    lcd.setTextColor(0x07E0, kBg);
    lcd.drawString("; .", 3, 122);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("move", 22, 122);
    lcd.setTextColor(0x07E0, kBg);
    lcd.drawString("ENT", 60, 122);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("open", 82, 122);
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
    return v <= kAppBackup ? static_cast<App>(v) : kAppMenu;
}

void rebootInto(App app)
{
    Preferences p;
    if (p.begin(kNs, false)) {
        p.putUChar("app", static_cast<uint8_t>(app));
        p.end();
    }
    Serial.printf("[launcher] restart into app %u\n", static_cast<unsigned>(app));
    Serial.flush();
    delay(50);
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
    dirty = true;
    draw();
}

void menuLoop()
{
    M5Cardputer.update();

    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            case ';': case 'k': case 'K':
            case '.': case 'j': case 'J': a |= 1u << 0; break;
            case KEY_ENTER: a |= 1u << 1; break;
            default: break;
        }
    }
    static uint32_t prev = 0;
    const uint32_t pressed = a & ~prev;
    prev                   = a;

    if (pressed & (1u << 0)) {
        sel   = (sel + 1) % 3;
        dirty = true;
    }
    if (pressed & (1u << 1)) {
        // does not return
        rebootInto(sel == 0 ? kAppRf : (sel == 1 ? kAppNfc : kAppBackup));
    }

    if (dirty) {
        dirty = false;
        draw();
    }
    delay(5);
}

}  // namespace launcher
