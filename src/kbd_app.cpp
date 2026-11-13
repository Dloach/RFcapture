// Wired (USB HID) and Bluetooth (BLE HID) keyboard.
//
// The two modes share everything except the transport: the Cardputer keyboard
// already produces standard HID usage codes and a modifier bitmask, so both
// paths are just "build an 8-byte boot-keyboard report and send it".
//
// Entered from the launcher's KEYBOARD item.  USB HID needs the whole firmware
// to be built with ARDUINO_USB_MODE=0 (TinyUSB); see platformio.ini.

#include "kbd_app.h"

#include <M5Cardputer.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

#include <BLE2902.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>

#include <cstring>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"

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

enum class Screen : uint8_t { kMenu, kBtList, kUsb, kBle };
Screen screen  = Screen::kMenu;
int menuSel    = 0;
bool dirty     = true;
bool statusDirty = true;
uint32_t prevActions = 0;

uint8_t prevReport[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool havePrev          = false;
char bleStatus[24]     = "IDLE";

USBHIDKeyboard usbKeyboard;

BLEServer* bleServer         = nullptr;
BLEHIDDevice* bleHid         = nullptr;
BLECharacteristic* bleInput  = nullptr;
bool bleInit                 = false;
bool bleConnected            = false;
bool autoAdvertise           = false;
uint32_t lastBleKick         = 0;

constexpr int kMaxBonds = 10;
int bondCount           = 0;
esp_bd_addr_t bondAddrs[kMaxBonds];
int btSel               = 0;
int btTop               = 0;

// Standard 8-byte boot keyboard report map with an LED output report.
const uint8_t kReportMap[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x91, 0x02,        //   Output (Data, Var, Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const)
    0xC0               // End Collection
};

class ServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer*) override
    {
        bleConnected = true;
        havePrev     = false;  // force the first report to the new host
        // Windows does not always re-subscribe to the input report CCCD when it
        // reconnects to a bonded device after a power cycle.  Without this the
        // link looks connected but every notify() is dropped as "notifications
        // disabled".  Set the local CCCD and let the periodic kick below make
        // sure the host receives a report after it has (re)subscribed.
        if (bleInput != nullptr) {
            auto* cccd = (BLE2902*)bleInput->getDescriptorByUUID((uint16_t)0x2902);
            if (cccd != nullptr) cccd->setNotifications(true);
        }
        lastBleKick = millis();
        snprintf(bleStatus, sizeof(bleStatus), "CONNECTED");
        statusDirty = true;
        Serial.println("[kbd] BLE connected");
    }

    void onDisconnect(BLEServer* server) override
    {
        bleConnected = false;
        havePrev     = false;
        if (autoAdvertise) {
            snprintf(bleStatus, sizeof(bleStatus), "ADVERTISING");
            statusDirty = true;
            // Bluedroid needs advertising restarted here for a quick re-connect.
            server->getAdvertising()->start();
            Serial.println("[kbd] BLE disconnected, advertising");
        } else {
            snprintf(bleStatus, sizeof(bleStatus), "DISCONNECTED");
            statusDirty = true;
            Serial.println("[kbd] BLE disconnected");
        }
    }
};

void header(const char* title)
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(title, 3, 2);
}

struct Hint {
    const char* key;
    const char* text;
};

void drawHints(const Hint* hints, int count)
{
    lcd.drawFastHLine(0, 118, lcd.width(), kDim);
    const int y = 122;
    int x       = 3;
    for (int i = 0; i < count; ++i) {
        const int need = lcd.textWidth(hints[i].key) + 2 + lcd.textWidth(hints[i].text);
        if (x + need > lcd.width()) break;
        lcd.setTextColor(kOk, kBg);
        lcd.drawString(hints[i].key, x, y);
        x += lcd.textWidth(hints[i].key) + 2;
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(hints[i].text, x, y);
        x += lcd.textWidth(hints[i].text) + 6;
    }
}

uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            case ';': case 'k': case 'K': a |= 1u << 0; break;
            case '.': case 'j': case 'J': a |= 1u << 1; break;
            case KEY_ENTER: a |= 1u << 2; break;
            case '`': case '~': case 0x1B: a |= 1u << 3; break;  // esc / backtick
            // USBHIDKeyboard.h redefines KEY_BACKSPACE to 0xB2; the M5 matrix
            // reports 0x2A.  Use the actual M5 keycode, not the macro.
            case 0x2A: a |= 1u << 4; break;                      // backspace
            default: break;
        }
    }
    return a;
}

void drawMenu()
{
    lcd.fillScreen(kBg);
    header("KEYBOARD");

    const char* items[] = {"USB / WIRED", "BLUETOOTH"};
    const char* desc[]  = {"Plug into a PC; ENTER starts.", "Pair Cardputer-KB; ENTER starts."};
    lcd.setTextSize(2);
    for (int i = 0; i < 2; ++i) {
        const int y = 24 + i * 42;
        if (i == menuSel) lcd.fillRect(4, y - 4, lcd.width() - 8, 22, kBar);
        lcd.setTextColor(i == menuSel ? kAccent : kText, i == menuSel ? kBar : kBg);
        lcd.drawString(items[i], 12, y);
        lcd.setTextSize(1);
        lcd.setTextColor(kDim, kBg);
        lcd.drawString(desc[i], 12, y + 18);
        lcd.setTextSize(2);
    }
    lcd.setTextSize(1);

    static const Hint kMenuHints[] = {{"ENT", "start"}, {"esc", "launcher"}};
    drawHints(kMenuHints, 2);
}

constexpr int kKeyX = 1;
// 4 rows x 23 px = 92 px in the 13..118 body; y=19 leaves 6 px above and
// 7 px below instead of hugging the header.
constexpr int kKeyY = 19;
constexpr int kKeyW = 17;
constexpr int kKeyH = 23;
constexpr int kKeyRows = 4;
constexpr int kKeyCols = 14;
constexpr uint16_t kKeyBg   = 0x2104;
constexpr uint16_t kKeyEdge = 0x4208;

// Printed Cardputer layout: 4 x 14.  The two-character abbreviations keep the
// labels inside a 17 px cell at font 1.
const char* const kBaseLabels[kKeyRows][kKeyCols] = {
    {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Bk"},
    {"Tb", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "\\"},
    {"Fn", "Aa", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "En"},
    {"Ct", "Op", "Al", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/", "Sp"},
};

bool isModifierKey(int row, int col)
{
    if (row == 2 && (col == 0 || col == 1)) return true;  // Fn, Aa
    if (row == 3 && col <= 2) return true;                // Ctrl, Opt, Alt
    return false;
}

bool isFnMapped(int row, int col)
{
    if (row == 0 && (col == 0 || col == 13)) return true;  // ESC, exit
    if (row == 0 && col >= 1 && col <= 12) return true;    // F1..F12
    if (row == 2 && col == 11) return true;                // Up
    if (row == 3 && col >= 10 && col <= 12) return true;   // Left/Down/Right
    return false;
}

const char* keyLabel(int row, int col, bool fn)
{
    static char fbuf[4];
    if (fn) {
        if (row == 0 && col == 0) return "Es";
        if (row == 0 && col == 13) return "Ex";
        if (row == 0 && col >= 1 && col <= 9) {
            fbuf[0] = 'F';
            fbuf[1] = static_cast<char>('1' + (col - 1));
            fbuf[2] = 0;
            return fbuf;
        }
        if (row == 0 && col == 10) return "10";
        if (row == 0 && col == 11) return "11";
        if (row == 0 && col == 12) return "12";
        if (row == 2 && col == 11) return "Up";
        if (row == 3 && col == 10) return "Lt";
        if (row == 3 && col == 11) return "Dn";
        if (row == 3 && col == 12) return "Rt";
    }
    return kBaseLabels[row][col];
}

bool prevDown[kKeyRows][kKeyCols] = {};
bool prevFn                         = false;

void drawActiveHeader()
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(screen == Screen::kUsb ? "USB KEYBOARD" : "BLE KEYBOARD", 3, 2);

    const char* status = screen == Screen::kUsb ? "USB HID" : bleStatus;
    const uint16_t color = (screen == Screen::kUsb || bleConnected) ? kOk : kWarn;
    lcd.setTextColor(color, kHead);
    lcd.drawRightString(status, lcd.width() - 3, 2);
    statusDirty = false;
}

void drawActiveFooter()
{
    static const Hint kHints[] = {{"Fn+`", "Esc"}, {"Fn+BKSP", "menu"}, {"Opt", "Win"}};
    drawHints(kHints, 3);
}

void readDownKeys(bool down[kKeyRows][kKeyCols])
{
    memset(down, 0, sizeof(bool) * kKeyRows * kKeyCols);
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        if (pos.x < kKeyCols && pos.y < kKeyRows) down[pos.y][pos.x] = true;
    }
}

void drawKeyCell(int row, int col, const bool down[kKeyRows][kKeyCols], bool fn)
{
    const int x = kKeyX + col * kKeyW;
    const int y = kKeyY + row * kKeyH;
    const bool pressed = down[row][col];
    const bool mod     = isModifierKey(row, col);
    const bool mapped  = fn && isFnMapped(row, col);

    uint16_t bg   = kKeyBg;
    uint16_t fg   = mapped ? kAccent : kText;
    uint16_t edge = kKeyEdge;
    if (pressed) {
        bg   = mod ? kAccent : kOk;
        fg   = kBg;
        edge = bg;
    }

    lcd.fillRect(x + 1, y + 1, kKeyW - 2, kKeyH - 2, bg);
    lcd.drawRect(x, y, kKeyW - 1, kKeyH - 1, edge);
    const char* label = keyLabel(row, col, fn);
    const int tw      = lcd.textWidth(label);
    lcd.setTextColor(fg, bg);
    lcd.drawString(label, x + (kKeyW - tw) / 2, y + (kKeyH - 8) / 2);
}

void renderKeys(const bool down[kKeyRows][kKeyCols], bool fn)
{
    for (int r = 0; r < kKeyRows; ++r) {
        for (int c = 0; c < kKeyCols; ++c) drawKeyCell(r, c, down, fn);
    }
}

void drawActive()
{
    lcd.fillScreen(kBg);
    lcd.setTextSize(1);
    drawActiveHeader();

    bool down[kKeyRows][kKeyCols];
    readDownKeys(down);
    const bool fn = M5Cardputer.Keyboard.keysState().fn;
    renderKeys(down, fn);
    memcpy(prevDown, down, sizeof(prevDown));
    prevFn = fn;

    drawActiveFooter();
}

void sendReport(const uint8_t* report)
{
    if (screen == Screen::kUsb) {
        KeyReport kr = {};
        kr.modifiers = report[0];
        kr.reserved  = report[1];
        memcpy(kr.keys, report + 2, sizeof(kr.keys));
        usbKeyboard.sendReport(&kr);
    } else if (screen == Screen::kBle && bleConnected && bleInput) {
        // setValue() takes a mutable pointer in this core version.
        uint8_t copy[8];
        memcpy(copy, report, sizeof(copy));
        bleInput->setValue(copy, sizeof(copy));
        bleInput->notify();
    }
}

void updateReport()
{
    const auto& ks = M5Cardputer.Keyboard.keysState();
    uint8_t modifiers = ks.modifiers;
    // M5's Opt key is a separate flag, not a HID modifier.  On Windows the
    // useful mapping is Left GUI (Win); on macOS the same bit is Command.
    if (ks.opt) modifiers |= 0x08;
    uint8_t report[8] = {modifiers, 0, 0, 0, 0, 0, 0, 0};
    int n = 0;
    for (uint8_t key : ks.hid_keys) {
        if (key == 0) continue;
        // Keep the orange punctuation keys as arrows under Fn, and make Fn+`
        // the Escape key.  Fn+Backspace is the way out (see loop()).
        if (ks.fn) {
            switch (key) {
                case 0x33: key = 0x52; break;  // ; -> Up
                case 0x37: key = 0x51; break;  // . -> Down
                case 0x36: key = 0x50; break;  // , -> Left
                case 0x38: key = 0x4F; break;  // / -> Right
                case 0x35: key = 0x29; break;  // ` -> ESC
                // Fn + number row -> F1..F12 (0 is F10, - is F11, = is F12).
                case 0x1E: key = 0x3A; break;  // 1 -> F1
                case 0x1F: key = 0x3B; break;  // 2 -> F2
                case 0x20: key = 0x3C; break;  // 3 -> F3
                case 0x21: key = 0x3D; break;  // 4 -> F4
                case 0x22: key = 0x3E; break;  // 5 -> F5
                case 0x23: key = 0x3F; break;  // 6 -> F6
                case 0x24: key = 0x40; break;  // 7 -> F7
                case 0x25: key = 0x41; break;  // 8 -> F8
                case 0x26: key = 0x42; break;  // 9 -> F9
                case 0x27: key = 0x43; break;  // 0 -> F10
                case 0x2D: key = 0x44; break;  // - -> F11
                case 0x2E: key = 0x45; break;  // = -> F12
                default: break;
            }
        }
        if (n < 6) report[2 + n] = key;
        ++n;
    }

    if (!havePrev || memcmp(report, prevReport, sizeof(report)) != 0) {
        memcpy(prevReport, report, sizeof(report));
        havePrev = true;
        sendReport(report);
    }
}

void initBleStack()
{
    if (bleInit) return;
    bleInit = true;
    snprintf(bleStatus, sizeof(bleStatus), "STARTING...");

    BLEDevice::init("Cardputer-KB");
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);

    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    bleHid = new BLEHIDDevice(bleServer);
    bleInput = bleHid->inputReport(1);
    // manufacturer(std::string) writes through m_manufacturerCharacteristic;
    // that pointer is only created by manufacturer().  Calling the string
    // overload first dereferenced an uninitialised pointer and crashed as soon
    // as a host started pairing.
    bleHid->manufacturer()->setValue("M5Stack");
    bleHid->pnp(0x02, 0x303A, 0x0002, 0x0110);
    bleHid->hidInfo(0x00, 0x01);
    bleHid->reportMap(const_cast<uint8_t*>(kReportMap), sizeof(kReportMap));
    bleHid->startServices();

    // Security parameters must be in place before advertising; otherwise a
    // reconnecting bonded host can reach the encrypted HID report without
    // re-engaging encryption.
    auto* sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_BOND);
    sec->setCapability(ESP_IO_CAP_NONE);

    bleServer->getAdvertising()->setAppearance(HID_KEYBOARD);
    bleServer->getAdvertising()->addServiceUUID(bleHid->hidService()->getUUID());

    snprintf(bleStatus, sizeof(bleStatus), "IDLE");
    statusDirty = true;
    Serial.println("[kbd] BLE stack ready");
}

void formatBondAddr(const esp_bd_addr_t addr, char* out, size_t n)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5]);
}

void refreshBonds()
{
    bondCount = 0;
    int n     = esp_ble_get_bond_device_num();
    if (n <= 0) return;
    if (n > kMaxBonds) n = kMaxBonds;

    esp_ble_bond_dev_t list[kMaxBonds];
    int count = n;
    if (esp_ble_get_bond_device_list(&count, list) != ESP_OK) return;
    if (count > kMaxBonds) count = kMaxBonds;
    for (int i = 0; i < count; ++i) {
        memcpy(bondAddrs[i], list[i].bd_addr, sizeof(esp_bd_addr_t));
    }
    bondCount = count;
}

void drawBtList()
{
    lcd.fillScreen(kBg);
    header("BLUETOOTH");

    constexpr int kListRows = 6;
    const int total         = 1 + bondCount;
    for (int r = 0; r < kListRows; ++r) {
        const int idx = btTop + r;
        if (idx >= total) break;
        const int y = 18 + r * 15;
        if (idx == btSel) lcd.fillRect(0, y - 2, lcd.width(), 14, kBar);
        const uint16_t fg = idx == btSel ? kAccent : kText;
        const uint16_t bg = idx == btSel ? kBar : kBg;

        char line[40];
        if (idx == 0) {
            snprintf(line, sizeof(line), "PAIR NEW DEVICE");
        } else {
            char addr[24];
            formatBondAddr(bondAddrs[idx - 1], addr, sizeof(addr));
            snprintf(line, sizeof(line), "DEVICE %d  %s", idx, addr);
        }
        lcd.setTextColor(fg, bg);
        lcd.drawString(line, 4, y);
    }

    if (bondCount == 0) {
        lcd.setTextColor(kDim, kBg);
        lcd.drawString("No bonded devices yet.", 6, 96);
    }

    static const Hint kHints[] = {{"ENT", "select"}, {"esc", "back"}};
    drawHints(kHints, 2);
}

void startBleAdvertising(bool pairNew, const esp_bd_addr_t* addr)
{
    initBleStack();
    auto* adv = bleServer->getAdvertising();
    adv->stop();
    esp_ble_gap_clear_whitelist();

    if (!pairNew && addr != nullptr) {
        // Arduino's whitelist helper uses public address type.  Windows BLE
        // hosts normally use public addresses; random-address hosts can still
        // use PAIR NEW DEVICE.
        esp_bd_addr_t target;
        memcpy(target, *addr, sizeof(target));
        BLEDevice::whiteListAdd(BLEAddress(target));
        adv->setScanFilter(false, true);
    } else {
        adv->setScanFilter(false, false);
    }

    autoAdvertise = true;
    adv->start();
    snprintf(bleStatus, sizeof(bleStatus), pairNew ? "PAIRING..." : "CONNECTING...");
    statusDirty = true;
    Serial.printf("[kbd] BLE %s\n", pairNew ? "pairing advertisement" : "targeted advertisement");
}

void enterBleList()
{
    screen = Screen::kBtList;
    initBleStack();
    refreshBonds();
    btSel = 0;
    btTop = 0;
    drawBtList();
}

void beginBle(int sel)
{
    screen   = Screen::kBle;
    havePrev = false;
    bleConnected = false;
    snprintf(bleStatus, sizeof(bleStatus), sel == 0 ? "PAIRING..." : "CONNECTING...");
    drawActive();
    if (sel == 0) {
        startBleAdvertising(true, nullptr);
    } else {
        startBleAdvertising(false, &bondAddrs[sel - 1]);
    }
}

void leaveBleToList()
{
    autoAdvertise = false;
    if (bleServer != nullptr) {
        bleServer->getAdvertising()->stop();
        auto peers = bleServer->getPeerDevices(false);
        for (auto& peer : peers) bleServer->disconnect(peer.first);
    }
    bleConnected = false;
    screen       = Screen::kBtList;
    refreshBonds();
    btSel = 0;
    btTop = 0;
    drawBtList();
}

void startUsb()
{
    screen   = Screen::kUsb;
    havePrev = false;
    dirty    = true;
    drawActive();

    usbKeyboard.begin();
    USB.begin();
    Serial.println("[kbd] USB HID started");
}

void handleMenu(uint32_t pressed)
{
    if (pressed & ((1u << 3) | (1u << 4))) {  // esc or backspace
        launcher::rebootInto(launcher::kAppMenu);
    }
    if (pressed & (1u << 0)) {
        menuSel = (menuSel + 1) % 2;
        dirty   = true;
    }
    if (pressed & (1u << 1)) {
        menuSel = (menuSel + 1) % 2;
        dirty   = true;
    }
    if (pressed & (1u << 2)) {
        if (menuSel == 0) {
            startUsb();
        } else {
            enterBleList();
        }
        return;
    }
}

}  // namespace

namespace kbdapp {

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    lcd.setRotation(rfcfg::screenRotation);
    lcd.setBrightness(90);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    lcd.fillScreen(kBg);

    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));

    Serial.printf("[kbd] keyboard app, usb_mode=%d\n", ARDUINO_USB_MODE);
    drawMenu();
}

void loop()
{
    M5Cardputer.update();

    const uint32_t a       = readActions();
    const uint32_t pressed = a & ~prevActions;
    prevActions            = a;

    if (screen == Screen::kMenu) {
        if (pressed) {
            handleMenu(pressed);
            // startUsb()/startBle() already drew the new page.  Without this
            // return, the rest of the old kMenu branch redraws the menu over
            // it, leaving the KEYBOARD title and footer on top of the mapping.
            if (screen != Screen::kMenu) {
                delay(2);
                return;
            }
        }
        if (dirty) {
            dirty = false;
            drawMenu();
        }
        delay(5);
        return;
    }

    if (screen == Screen::kBtList) {
        const int total = 1 + bondCount;
        if (pressed & ((1u << 3) | (1u << 4))) {  // esc / BKSP
            screen = Screen::kMenu;
            dirty  = true;
            drawMenu();
            return;
        }
        if (pressed & (1u << 0)) {
            btSel = (btSel + total - 1) % total;
            dirty = true;
        }
        if (pressed & (1u << 1)) {
            btSel = (btSel + 1) % total;
            dirty = true;
        }
        if (btSel < btTop) btTop = btSel;
        if (btSel >= btTop + 6) btTop = btSel - 5;

        if (pressed & (1u << 2)) {  // ENTER: pair new or selected bond
            beginBle(btSel);
            return;
        }
        if (dirty) {
            dirty = false;
            drawBtList();
        }
        delay(5);
        return;
    }

    // Exit is Fn+Backspace only.  A bare ESC/backtick is a normal keyboard key
    // here; Fn+` sends the real Escape usage code (see updateReport()).
    // BLE returns to the device list without a reboot; the stack is already up.
    // USB returns to the KEYBOARD mode menu by reboot (the HID stack is one-shot).
    const auto& ks = M5Cardputer.Keyboard.keysState();
    if (ks.fn && (a & (1u << 4))) {
        if (screen == Screen::kBle) {
            leaveBleToList();
        } else {
            launcher::rebootInto(launcher::kAppKbd);
        }
        return;
    }

    updateReport();

    bool down[kKeyRows][kKeyCols];
    readDownKeys(down);
    if (ks.fn != prevFn || memcmp(down, prevDown, sizeof(prevDown)) != 0) {
        renderKeys(down, ks.fn);
        memcpy(prevDown, down, sizeof(prevDown));
        prevFn = ks.fn;
    }

    // A bonded host can reconnect without re-writing the input report's CCCD.
    // Push the current report once a second so a late (re)subscription still
    // receives state instead of looking connected but deaf.
    if (screen == Screen::kBle && bleConnected && millis() - lastBleKick >= 1000) {
        lastBleKick = millis();
        sendReport(prevReport);
    }

    if (statusDirty) drawActiveHeader();
    delay(2);
}

}  // namespace kbdapp
