#include "rec_app.h"

#include <M5Cardputer.h>
#include <SD.h>

#include <cstring>
#include <memory>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "sd_mount.h"
#include "ui_hint.h"

namespace recapp {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kHead   = 0x18E3;
constexpr uint16_t kBar    = 0x2104;

constexpr uint32_t kRate = 16000;
constexpr size_t kBlock = 480;  // 30 ms, callback double-buffered
constexpr int kMaxFiles = 24;
constexpr int kVisibleFiles = 5;

auto& lcd = M5Cardputer.Display;
bool sdOk = false;
bool recording = false;
bool micOk = false;
bool dirty = true;
uint32_t prevActions = 0;
uint32_t samples = 0;
uint32_t savedBytes = 0;
int peak = 0;
int levelPct = 0;
char status[48] = "ENT record";
char lastFile[64] = {0};
File wav;
int16_t micBuf[2][kBlock];
volatile bool micReady[2] = {false, false};
bool deleteArmed = false;
uint32_t deleteAt = 0;

char fileNames[kMaxFiles][40];
uint32_t fileSizes[kMaxFiles];
int fileCount = 0;
int fileSel = 0;
int fileTop = 0;

struct __attribute__((packed)) WavHeader {
    char riff[4];
    uint32_t riffSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t format;
    uint16_t channels;
    uint32_t rate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bits;
    char data[4];
    uint32_t dataSize;
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

void header()
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("RECORDER", 3, 2);
    char b[20];
    snprintf(b, sizeof(b), "%d%%", M5.Power.getBatteryLevel());
    lcd.drawRightString(b, lcd.width() - 3, 2);
}

const char* baseName(const char* path)
{
    const char* slash = path ? strrchr(path, '/') : nullptr;
    return slash ? slash + 1 : (path ? path : "");
}

void micRelease(void*, void* data, size_t)
{
    if (data == micBuf[0]) micReady[0] = true;
    if (data == micBuf[1]) micReady[1] = true;
}

void processBlock(int16_t* buf)
{
    // Remove the ES8311 DC offset and map the block peak to a 0..100 meter.
    int32_t sum = 0;
    for (size_t i = 0; i < kBlock; ++i) sum += buf[i];
    const int32_t mean = sum / static_cast<int32_t>(kBlock);
    int p = 0;
    for (size_t i = 0; i < kBlock; ++i) {
        int32_t v = buf[i] - mean;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[i] = static_cast<int16_t>(v);
        const int a = abs(static_cast<int>(v));
        if (a > p) p = a;
    }
    peak = p;
    levelPct = p * 100 / 16000;
    if (levelPct > 100) levelPct = 100;
}

void scanFiles()
{
    fileCount = 0;
    File dir = SD.open("/recordings");
    if (dir) {
        while (fileCount < kMaxFiles) {
            File f = dir.openNextFile();
            if (!f) break;
            const char* raw = f.name();
            const char* name = baseName(raw);
            String lower = name;
            lower.toLowerCase();
            if (!f.isDirectory() && lower.endsWith(".wav")) {
                strncpy(fileNames[fileCount], name, sizeof(fileNames[0]) - 1);
                fileNames[fileCount][sizeof(fileNames[0]) - 1] = 0;
                fileSizes[fileCount] = f.size();
                ++fileCount;
            }
            f.close();
        }
        dir.close();
    }
    if (fileCount == 0) {
        fileSel = 0;
        fileTop = 0;
    } else {
        if (fileSel >= fileCount) fileSel = fileCount - 1;
        if (fileSel < 0) fileSel = 0;
        if (fileSel < fileTop) fileTop = fileSel;
        if (fileSel >= fileTop + kVisibleFiles) fileTop = fileSel - kVisibleFiles + 1;
    }
}

void drawTop()
{
    lcd.fillRect(0, 13, lcd.width(), 55, kBg);
    lcd.setTextColor(micOk ? kOk : kWarn, kBg);
    lcd.drawString(micOk ? "MIC OK" : "MIC FAIL", 4, 16);
    lcd.setTextColor(recording ? kWarn : kDim, kBg);
    lcd.drawRightString(status, lcd.width() - 4, 16);

    char line[64];
    snprintf(line, sizeof(line), "TIME %lu.%01lus", static_cast<unsigned long>(samples / kRate),
             static_cast<unsigned long>((samples % kRate) / (kRate / 10)));
    lcd.setTextColor(kText, kBg);
    lcd.drawString(line, 4, 28);

    lcd.setTextColor(kDim, kBg);
    lcd.drawString("LEVEL", 4, 41);
    lcd.drawRect(50, 40, 150, 10, kDim);
    lcd.fillRect(52, 42, (levelPct * 146) / 100, 6, levelPct > 85 ? kWarn : kOk);
    char lv[8];
    snprintf(lv, sizeof(lv), "%d%%", levelPct);
    lcd.setTextColor(kDim, kBg);
    lcd.drawRightString(lv, lcd.width() - 4, 41);

    const char* shown = lastFile[0] ? baseName(lastFile) : "-";
    snprintf(line, sizeof(line), "FILE %.28s", shown);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(line, 4, 53);
    snprintf(line, sizeof(line), "DATA %lu B", static_cast<unsigned long>(savedBytes));
    lcd.drawRightString(line, lcd.width() - 4, 53);
}

void drawList()
{
    lcd.fillRect(0, 68, lcd.width(), 48, kBg);
    lcd.drawFastHLine(0, 68, lcd.width(), kDim);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("SAVED", 4, 70);
    if (fileCount == 0) {
        lcd.drawString("no recordings", 4, 82);
        return;
    }
    for (int r = 0; r < kVisibleFiles; ++r) {
        const int idx = fileTop + r;
        if (idx >= fileCount) break;
        const int y = 78 + r * 8;
        if (idx == fileSel && !recording) lcd.fillRect(0, y - 1, lcd.width(), 9, kBar);
        lcd.setTextColor(idx == fileSel && !recording ? kAccent : kText,
                         idx == fileSel && !recording ? kBar : kBg);
        lcd.drawString(fileNames[idx], 4, y);
        char sz[20];
        snprintf(sz, sizeof(sz), "%luB", static_cast<unsigned long>(fileSizes[idx]));
        lcd.setTextColor(idx == fileSel && !recording ? kAccent : kDim,
                         idx == fileSel && !recording ? kBar : kBg);
        lcd.drawRightString(sz, lcd.width() - 4, y);
    }
}

void draw()
{
    lcd.fillScreen(kBg);
    header();
    drawTop();
    drawList();
    static const Hint h[] = {{"ENT", "rec/stop"}, {"P", "play"}, {"D", "del"}, {"esc", "back"}};
    hints(h, 4);
    dirty = false;
}

void drawLive()
{
    drawTop();
}

void writeHeader(uint32_t dataBytes)
{
    WavHeader h{};
    memcpy(h.riff, "RIFF", 4);
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    h.fmtSize = 16;
    h.format = 1;
    h.channels = 1;
    h.rate = kRate;
    h.byteRate = kRate * 2;
    h.blockAlign = 2;
    h.bits = 16;
    memcpy(h.data, "data", 4);
    h.dataSize = dataBytes;
    h.riffSize = 36 + dataBytes;
    wav.seek(0);
    wav.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
}

bool openRecording()
{
    SD.mkdir("/recordings");
    for (int n = 0; n < 1000; ++n) {
        char path[64];
        snprintf(path, sizeof(path), "/recordings/rec_%03d.wav", n);
        if (SD.exists(path)) continue;
        wav = SD.open(path, FILE_WRITE);
        if (!wav) return false;
        WavHeader zero{};
        wav.write(reinterpret_cast<const uint8_t*>(&zero), sizeof(zero));
        strncpy(lastFile, path, sizeof(lastFile) - 1);
        lastFile[sizeof(lastFile) - 1] = 0;
        samples = 0;
        peak = 0;
        return true;
    }
    return false;
}

void beginMic()
{
    M5.Mic.end();
    M5.Speaker.end();
    delay(100);
    auto mc = M5.Mic.config();
    mc.sample_rate = kRate;
    mc.magnification = 64;
    M5.Mic.config(mc);
    micOk = M5.Mic.begin();
    if (micOk) {
        M5.Mic.setSampleRate(kRate);
        delay(1200);
    }
    Serial.printf("[rec] mic=%d gain=%d\n", micOk ? 1 : 0, mc.magnification);
}

void stopRecording()
{
    if (!recording) return;
    recording = false;
    writeHeader(samples * 2);
    wav.close();
    savedBytes = samples * 2;
    scanFiles();
    fileSel = fileCount > 0 ? fileCount - 1 : 0;
    if (fileSel < fileTop) fileTop = fileSel;
    if (fileSel >= fileTop + kVisibleFiles) fileTop = fileSel - kVisibleFiles + 1;
    snprintf(status, sizeof(status), "SAVED %lu B", static_cast<unsigned long>(savedBytes));
    dirty = true;
}

void startRecording()
{
    if (!sdOk || recording) return;
    if (!openRecording()) {
        snprintf(status, sizeof(status), "OPEN FAILED");
        dirty = true;
        return;
    }
    if (!micOk) beginMic();
    if (!micOk) {
        snprintf(status, sizeof(status), "MIC FAILED");
        wav.close();
        dirty = true;
        return;
    }
    recording = true;
    samples = 0;
    savedBytes = 0;
    peak = 0;
    snprintf(status, sizeof(status), "RECORDING");
    dirty = true;
}

void recordLoop()
{
    bool enterReleased = false;
    int writeIndex = 0;
    int drawCounter = 0;
    micReady[0] = micReady[1] = false;
    M5.Mic.setBufferReleaseCallback(nullptr, micRelease);
    M5.Mic.record(micBuf[0], kBlock, kRate, false);
    M5.Mic.record(micBuf[1], kBlock, kRate, false);

    while (recording) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isKeyPressed('`')) break;
        const bool enterNow = M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER);
        if (!enterNow) enterReleased = true;
        if (enterReleased && enterNow) break;

        if (!micReady[writeIndex]) {
            delay(1);
            continue;
        }

        int16_t* block = micBuf[writeIndex];
        processBlock(block);
        if (samples == 0) writeHeader(0);
        const size_t wrote = wav.write(reinterpret_cast<const uint8_t*>(block), kBlock * 2);
        if (wrote != kBlock * 2) {
            snprintf(status, sizeof(status), "SD WRITE FAILED");
            break;
        }
        micReady[writeIndex] = false;
        if (!M5.Mic.record(block, kBlock, kRate, false)) {
            // The slot was just released; retry shortly rather than dropping it.
            for (int retry = 0; retry < 8; ++retry) {
                delay(2);
                if (M5.Mic.record(block, kBlock, kRate, false)) break;
            }
        }
        writeIndex ^= 1;
        samples += kBlock;
        if (++drawCounter >= 3) {
            drawCounter = 0;
            drawLive();
        }
        if (samples > kRate * 300) {
            snprintf(status, sizeof(status), "MAX 5 MIN");
            break;
        }
    }
    stopRecording();
    draw();
}

void playSelected()
{
    if (fileCount == 0) {
        snprintf(status, sizeof(status), "NO RECORDING");
        dirty = true;
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/recordings/%s", fileNames[fileSel]);
    File f = SD.open(path, FILE_READ);
    if (!f) {
        snprintf(status, sizeof(status), "OPEN FAILED");
        dirty = true;
        return;
    }
    const size_t bytes = f.size();
    if (bytes <= 44) {
        f.close();
        snprintf(status, sizeof(status), "EMPTY RECORDING");
        dirty = true;
        return;
    }
    f.seek(44);
    M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(240);
    static int16_t block[4096];  // 256 ms chunks: few SD-read gaps
    size_t remaining = bytes - 44;
    snprintf(status, sizeof(status), "PLAYING");
    dirty = true;
    draw();
    while (remaining > 0) {
        const size_t want = remaining > sizeof(block) ? sizeof(block) : remaining;
        const size_t got = f.read(reinterpret_cast<uint8_t*>(block), want);
        if (got < 2) break;
        M5.Speaker.playRaw(block, got / 2, kRate, false);
        while (M5.Speaker.isPlaying()) {
            M5Cardputer.update();
            if (M5Cardputer.Keyboard.isKeyPressed('`') ||
                M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                M5.Speaker.stop();
                remaining = 0;
                break;
            }
            delay(2);
        }
        if (remaining == 0) break;
        remaining -= got;
    }
    f.close();
    M5.Speaker.end();
    beginMic();
    snprintf(status, sizeof(status), "PLAYED");
    dirty = true;
}

enum : uint32_t { kBack = 1, kEnter = 2, kPlay = 4, kUp = 8, kDown = 16, kDelete = 32 };

uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t c = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        if (c == '`' || c == '~' || c == 0x1B) a |= kBack;
        if (c == KEY_ENTER) a |= kEnter;
        if (c == 'p' || c == 'P') a |= kPlay;
        if (c == ';') a |= kUp;
        if (c == '.') a |= kDown;
        if (c == 'd' || c == 'D') a |= kDelete;
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

    sdOk = sdmount::ensure(false);
    if (!sdOk) {
        snprintf(status, sizeof(status), "NO TF CARD");
    } else {
        scanFiles();
        beginMic();
    }
    draw();
}

void loop()
{
    M5Cardputer.update();
    const uint32_t actions = readActions();
    const uint32_t pressed = actions & ~prevActions;
    prevActions = actions;

    if (recording) {
        recordLoop();
        return;
    }
    if (pressed & kBack) {
        M5.Mic.end();
        launcher::rebootInto(launcher::kAppMenu);
        return;
    }
    if (pressed & kUp && fileCount > 0) {
        if (fileSel > 0) --fileSel;
        if (fileSel < fileTop) fileTop = fileSel;
        dirty = true;
    }
    if (pressed & kDown && fileCount > 0) {
        if (fileSel + 1 < fileCount) ++fileSel;
        if (fileSel >= fileTop + kVisibleFiles) fileTop = fileSel - kVisibleFiles + 1;
        dirty = true;
    }
    if (deleteArmed && millis() - deleteAt > 2000) {
        deleteArmed = false;
        if (strcmp(status, "PRESS D AGAIN") == 0) snprintf(status, sizeof(status), "READY");
        dirty = true;
    }
    if ((pressed & kDelete) && fileCount > 0) {
        if (!deleteArmed) {
            deleteArmed = true;
            deleteAt = millis();
            snprintf(status, sizeof(status), "PRESS D AGAIN");
            dirty = true;
        } else {
            char path[64];
            snprintf(path, sizeof(path), "/recordings/%s", fileNames[fileSel]);
            const bool ok = SD.remove(path);
            deleteArmed = false;
            scanFiles();
            snprintf(status, sizeof(status), ok ? "DELETED" : "DELETE FAILED");
            dirty = true;
        }
    }
    if (pressed & kEnter) {
        startRecording();
    }
    if (pressed & kPlay) {
        playSelected();
    }
    if (dirty) draw();
}

}  // namespace recapp
