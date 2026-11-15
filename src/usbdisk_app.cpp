/*
 * USB DISK: expose the TF card to a PC as a USB mass-storage drive.
 *
 * The dangerous part is leaving.  The card's file system is cached by the host,
 * so a reboot (or a card pull) while the host still thinks it owns the drive can
 * leave a half-updated FAT behind -- a corrupted file system, not just a lost
 * file.  The MSC start/stop callback is the only signal this firmware gets that
 * the host has finished with the medium, so this app tracks it and refuses the
 * normal exit until it has been seen:
 *
 *   start unit  (start=1)  host took the medium
 *   stop unit   (start=0)  host released it ("Safely Remove", or a shutdown)
 *
 * `mediaPresent(false)` is asserted before the reboot so a listening host is
 * told the medium went away rather than being left to discover it later.
 *
 * The guard is a UX guard, not a substitute for the host's own flush: only the
 * PC can guarantee that its cache reached the card, which is why the screen
 * keeps telling the operator to eject first.  A forced exit exists (three
 * presses) for the case where the host has hung or its cable was pulled, and it
 * says so in the log.
 */

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
char note[48] = {0};
uint32_t prevActions = 0;

// Host-side medium ownership, written from the TinyUSB task.
volatile bool hostActive   = false;  // the host has taken the medium
volatile bool hostReleased = false;  // the host stopped / unloaded it
volatile bool hostWrote    = false;  // the host changed sectors

// Forced exit: three presses while the host still owns the drive.
uint32_t forceAt    = 0;
uint8_t forceCount  = 0;

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

/// True when the host no longer owns the medium: it ejected, never mounted it,
/// or never wrote to it (nothing can be pending in that case).
bool canExitSafely()
{
    if (!hostActive) return true;
    if (hostReleased) return true;
    if (!hostWrote) return true;
    return false;
}

const char* hostStateText()
{
    if (!hostActive) return "PC NOT MOUNTED";
    if (hostReleased) return "EJECTED - SAFE";
    if (!hostWrote) return "MOUNTED (read only)";
    return "DO NOT UNPLUG";
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

        lcd.setTextColor(canExitSafely() ? kOk : kWarn, kBg);
        lcd.drawString(hostStateText(), 8, 54);

        lcd.setTextColor(kDim, kBg);
        lcd.drawString(note[0] ? note : "EJECT the drive on the PC first.", 8, 70);
        lcd.drawString("Then esc. Do not pull the cable while writing.", 8, 82);
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
    // From here on the host may hold a cached FAT that only it can flush.
    hostWrote = true;
    return static_cast<int32_t>(bufsize);
}

// TinyUSB calls this on "start unit", "stop unit", and "load/eject medium".
// Returning true acknowledges; the value that matters is the state we record.
bool startStopCallback(uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    hostActive   = start;
    hostReleased = !start;
    Serial.printf("[msc] start_stop start=%d eject=%d -> %s\n", static_cast<int>(start),
                  static_cast<int>(load_eject), hostReleased ? "released" : "active");
    return true;
}

/// Detach the medium and let the card go before the reset.
void shutdownMedium()
{
    if (msc != nullptr) {
        msc->mediaPresent(false);
        delay(120);  // give the host a chance to see the removal
        msc->end();
    }
    SD.end();
}

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
    Serial.println("[msc] media present, waiting for a host");
    draw();
}

void loop()
{
    M5Cardputer.update();
    const uint32_t actions = readActions();
    const uint32_t pressed = actions & ~prevActions;
    prevActions            = actions;

    if (pressed & kBack) {
        if (canExitSafely()) {
            Serial.println("[msc] safe exit");
            shutdownMedium();
            launcher::rebootInto(launcher::kAppMenu);
        } else {
            // Refuse, and say exactly why.  The host's cache is the problem; only
            // the host can flush it.
            const uint32_t now = millis();
            if (forceCount == 0 || now - forceAt > 4000) forceCount = 0;
            ++forceCount;
            forceAt = now;
            if (forceCount >= 3) {
                Serial.println("[msc] FORCED exit with the host still mounted");
                snprintf(note, sizeof(note), "FORCED EXIT - FS MAY BE DAMAGED");
                draw();
                delay(400);
                shutdownMedium();
                launcher::rebootInto(launcher::kAppMenu);
            } else {
                snprintf(note, sizeof(note), "EJECT ON PC FIRST - esc x%u TO FORCE",
                         static_cast<unsigned>(3 - forceCount));
                draw();
            }
            prevActions = readActions();
        }
    }
    delay(5);
}

}  // namespace usbdiskapp
