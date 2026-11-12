/*
 * BACKUP: one place that puts everything on the TF card.
 *
 * A top-level feature rather than a page inside RF or NFC, because both write to
 * the same card and only one of them should ever own the SPI bus:
 *
 *   RF  uses the CC1101 in async mode, SPI mode 0.
 *   NFC uses the ST25R3916, SPI mode 1.
 *
 * An SD card is mode 0.  Mounting the card while the NFC chip is configured left
 * it latched -- the card stayed unreadable for the rest of the power cycle, and
 * even the RF feature could no longer mount it.  This feature therefore never
 * initialises either radio: it is a clean SPI bus with one device on it.
 *
 * The backup is two files on the card, both plain text:
 *
 *   /rf/signals.txt   the saved RF signals (tf_backup.cpp's format)
 *   /nfc/cards.bak    the saved NFC cards  (nfc_cards.h's format)
 *
 * Restore is destructive on both, so it asks first and reports how many records
 * each side holds.
 */

#include "backup_app.h"

#include <M5Cardputer.h>
#include <SPIFFS.h>

#include <memory>

#include "launcher.h"
#include "nfc_cards.h"
#include "polling_keyboard.h"
#include "quick_bind.h"
#include "rf_config.h"
#include "saved_store.h"
#include "sd_mount.h"
#include "tf_backup.h"
#include "ui_hint.h"

namespace backupapp {
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

SavedStore saved_;  // RF signals, from NVS
int sel = 0;
bool dirty = true;
bool confirmRestore = false;
char statusMsg[48] = {0};
bool sdOk = false;

struct Hint {
    const char* key;
    const char* text;
};

void header()
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("BACKUP", 3, 2);

    const int level = M5.Power.getBatteryLevel();
    char bat[12];
    snprintf(bat, sizeof(bat), level < 0 ? "--" : "%d%%", level);
    lcd.setTextColor(kText, kHead);
    lcd.drawRightString(bat, lcd.width() - 3, 2);
}

void hints()
{
    lcd.drawFastHLine(0, 118, lcd.width(), kDim);
    const char* keys[]  = {"ENT", "; .", "esc"};
    const char* texts[] = {confirmRestore ? "restore" : "run", "move", "menu"};
    int x               = 3;
    for (int i = 0; i < 3; ++i) {
        lcd.setTextColor(kOk, kBg);
        if (keys[i][0] == ';') {
            // "; ." means "the orange arrow keys": draw them as triangles.
            drawMoveTriangles(lcd, x, 122, kOk);
            x += kMoveTrianglesW;
        } else {
            lcd.drawString(keys[i], x, 122);
            x += lcd.textWidth(keys[i]);
        }
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(texts[i], x + 2, 122);
        x += lcd.textWidth(texts[i]) + 10;
    }
}

void draw()
{
    lcd.fillScreen(kBg);
    header();

    if (confirmRestore) {
        lcd.setTextColor(kText, kBg);
        lcd.setTextSize(2);
        lcd.drawString("Restore?", 10, 34);
        lcd.setTextSize(1);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("Replaces the RF signals and the", 10, 62);
        lcd.drawString("NFC card list with what is on", 10, 74);
        lcd.drawString("the TF card.", 10, 86);
        hints();
        return;
    }

    const char* items[] = {"BACKUP TO TF", "RESTORE FROM TF"};
    lcd.setTextSize(2);
    for (int i = 0; i < 2; ++i) {
        const int y = 26 + i * 26;
        if (i == sel) lcd.fillRect(4, y - 5, lcd.width() - 8, 21, kBar);
        lcd.setTextColor(i == sel ? kAccent : kText, i == sel ? kBar : kBg);
        lcd.setCursor(14, y);
        lcd.printf("%s %s", i == sel ? ">" : " ", items[i]);
    }
    lcd.setTextSize(1);

    char line[48];
    lcd.setTextColor(kDim, kBg);
    snprintf(line, sizeof(line), "RF signals: %u", static_cast<unsigned>(saved_.size()));
    lcd.drawString(line, 6, 82);
    snprintf(line, sizeof(line), "NFC cards : %u", static_cast<unsigned>(nfccards::count));
    lcd.drawString(line, 6, 94);

    lcd.setTextColor(sdOk ? kOk : kWarn, kBg);
    lcd.drawString(sdOk ? "TF card ready" : "no TF card", 6, 106);

    if (statusMsg[0]) {
        lcd.setTextColor(kAccent, kBg);
        lcd.drawString(statusMsg, 6, 106);
        lcd.setTextColor(sdOk ? kOk : kWarn, kBg);
    }

    hints();
}

// The quick-bind table rides along as a plain file copy: SPIFFS /quick.txt
// <-> SD /quick.txt, same "one obvious backup slot" rule as the NFC cards.
bool copyFile(fs::FS& from, const char* src, fs::FS& dst, const char* dstPath)
{
    if (!from.exists(src)) return true;  // nothing to copy is not an error
    File in = from.open(src, FILE_READ);
    if (!in) return false;
    File out = dst.open(dstPath, FILE_WRITE);
    if (!out) {
        in.close();
        return false;
    }
    uint8_t buf[128];
    int n;
    while ((n = in.read(buf, sizeof(buf))) > 0) out.write(buf, static_cast<size_t>(n));
    in.close();
    out.close();
    return true;
}

void doBackup()
{
    if (!sdOk) {
        snprintf(statusMsg, sizeof(statusMsg), "no TF card");
        dirty = true;
        return;
    }
    int rfCount = 0;
    const TfResult rf = tfbackup::saveSignals(saved_, &rfCount);
    const bool nfc    = nfccards::backupToSd();
    const bool quick  = copyFile(SPIFFS, "/quick.txt", SD, "/quick.txt");

    snprintf(statusMsg, sizeof(statusMsg), "RF %d %s|NFC %u %s|Q %s", rfCount,
             rf == TfResult::kOk ? "ok" : "fail", static_cast<unsigned>(nfccards::count),
             nfc ? "ok" : "fail", quick ? "ok" : "fail");
    Serial.printf("[backup] rf=%d n=%d nfc=%d quick=%d msg=%s\n", static_cast<int>(rf), rfCount,
                  static_cast<int>(nfc), static_cast<int>(quick), statusMsg);
    dirty = true;
}

void doRestore()
{
    if (!sdOk) {
        snprintf(statusMsg, sizeof(statusMsg), "no TF card");
        dirty = true;
        return;
    }
    int rfCount         = 0;
    const TfResult rf   = tfbackup::loadSignals(saved_, &rfCount);
    const bool nfc      = nfccards::restoreFromSd();
    const bool quick    = copyFile(SD, "/quick.txt", SPIFFS, "/quick.txt");
    if (quick) quickbind::load();  // make the restored bindings live now

    snprintf(statusMsg, sizeof(statusMsg), "RF %d %s|NFC %u %s|Q %s", rfCount,
             rf == TfResult::kOk ? "ok" : "fail", static_cast<unsigned>(nfccards::count),
             nfc ? "ok" : "fail", quick ? "ok" : "fail");
    Serial.printf("[backup] restore rf=%d n=%d nfc=%d quick=%d msg=%s\n", static_cast<int>(rf),
                  rfCount, static_cast<int>(nfc), static_cast<int>(quick), statusMsg);
    confirmRestore = false;
    dirty          = true;
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

    saved_.begin();
    nfccards::load();

    // First paint BEFORE the SD mount: the launcher's splash is still on the
    // panel from before the reboot, and replacing it with this page (rather
    // than a blank) is what makes the transition read as a load, not a blackout.
    lcd.fillScreen(kBg);
    header();
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("Mounting TF card...", 6, 60);

    // The bus is untouched at this point -- neither radio has been initialised in
    // this boot -- so this is the one place the SD card can be mounted safely.
    sdOk = sdmount::ensure();
    Serial.printf("[backup] sd=%d rf=%d nfc=%u\n", static_cast<int>(sdOk),
                  static_cast<int>(saved_.size()), static_cast<unsigned>(nfccards::count));

    dirty = true;
    draw();
}

void loop()
{
    M5Cardputer.update();

    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            case ';': case 'k': case 'K':
            case '.': case 'j': case 'J': a |= 1u << 0; break;
            case KEY_ENTER: a |= 1u << 1; break;
            case '`': case '~': case 0x1B: case KEY_BACKSPACE: a |= 1u << 2; break;
            default: break;
        }
    }
    static uint32_t prev = 0;
    const uint32_t pressed = a & ~prev;
    prev                   = a;

    if (pressed & (1u << 2)) {  // esc
        if (confirmRestore) {
            confirmRestore = false;
            dirty          = true;
        } else {
            launcher::rebootInto(launcher::kAppMenu);
        }
    }
    if (pressed & (1u << 0)) {
        sel   = (sel + 1) % 2;
        dirty = true;
    }
    if (pressed & (1u << 1)) {
        if (confirmRestore) {
            doRestore();
        } else if (sel == 0) {
            doBackup();
        } else {
            confirmRestore = true;
            dirty          = true;
        }
    }

    if (dirty) {
        dirty = false;
        draw();
    }
    delay(5);
}

}  // namespace backupapp
