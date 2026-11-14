#include "usbdisk_app.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <USB.h>
#include <USBMSC.h>

#include <memory>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "sd_mount.h"
#include "ui_hint.h"

namespace usbdiskapp {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kHead   = 0x18E3;

auto& lcd = M5Cardputer.Display;
USBMSC* msc = nullptr;
bool ready = false;
bool sdOk  = false;
char note[40] = {0};
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

void header()
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("USB DISK", 3, 2);
    const int level = M5.Power.getBatteryLevel();
    char bat[12];
    snprintf(bat, sizeof(bat), level < 0 ? "--" : "%d%%", level);
    lcd.drawRightString(bat, lcd.width() - 3, 2);
}

void draw()
{
    lcd.fillScreen(kBg);
    header();
    if (!sdOk) {
        lcd.setTextColor(kWarn, kBg);
        lcd.drawString("NO TF CARD", 8, 34);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("Insert a card, then rerun USB DISK.", 8, 54);
    } else {
        lcd.setTextColor(ready ? kOk : kAccent, kBg);
        lcd.drawString(ready ? "USB DRIVE READY" : "USB DRIVE FAILED", 8, 24);
        lcd.setTextColor(kText, kBg);
        char info[48];
        snprintf(info, sizeof(info), "%lu MB / %lu sectors",
                 static_cast<unsigned long>(SD.cardSize() / (1024ULL * 1024ULL)),
                 static_cast<unsigned long>(SD.numSectors()));
        lcd.drawString(info, 8, 40);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(note[0] ? note : "Open the new drive on the PC.", 8, 58);
        lcd.drawString("EJECT / SAFELY REMOVE before exit.", 8, 70);
        lcd.drawString("esc returns to launcher.", 8, 82);
    }
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
}

int32_t readCallback(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize)
{
    if (!sdOk) return -1;
    const uint32_t sec = SD.sectorSize();
    if (sec == 0 || bufsize == 0 || (bufsize % sec) != 0) return -1;
    uint8_t* p = static_cast<uint8_t*>(buffer);
    const uint32_t count = bufsize / sec;
    for (uint32_t i = 0; i < count; ++i) {
        if (!SD.readRAW(p + i * sec, lba + i)) return -1;
    }
    return static_cast<int32_t>(bufsize);
}

int32_t writeCallback(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize)
{
    if (!sdOk) return -1;
    const uint32_t sec = SD.sectorSize();
    if (sec == 0 || bufsize == 0 || (bufsize % sec) != 0) return -1;
    const uint32_t count = bufsize / sec;
    for (uint32_t i = 0; i < count; ++i) {
        if (!SD.writeRAW(buffer + i * sec, lba + i)) return -1;
    }
    return static_cast<int32_t>(bufsize);
}

bool startStopCallback(uint8_t, bool, bool) { return true; }

enum : uint32_t { kBack = 1 };

uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t c = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        if (c == '`' || c == '~' || c == 0x1B) a |= kBack;
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

    sdOk = sdmount::ensure(false);
    if (!sdOk) {
        draw();
        return;
    }

    msc = new USBMSC();
    if (msc == nullptr) {
        snprintf(note, sizeof(note), "MSC ALLOC FAILED");
        draw();
        return;
    }
    msc->vendorID("M5Stack");
    msc->productID("Cardputer TF");
    msc->productRevision("1.0");
    msc->onRead(readCallback);
    msc->onWrite(writeCallback);
    msc->onStartStop(startStopCallback);
    msc->mediaPresent(true);
    ready = msc->begin(SD.numSectors(), SD.sectorSize());
    if (!ready) snprintf(note, sizeof(note), "MSC INIT FAILED");
    USB.begin();
    draw();
}

void loop()
{
    M5Cardputer.update();
    const uint32_t actions = readActions();
    const uint32_t pressed = actions & ~prevActions;
    prevActions            = actions;

    if (pressed & kBack) {
        // The host must eject first; this is only a UX guard, not a safe
        // unmount protocol.
        launcher::rebootInto(launcher::kAppMenu);
    }
    delay(5);
}

}  // namespace usbdiskapp
