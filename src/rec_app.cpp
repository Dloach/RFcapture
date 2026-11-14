#include "rec_app.h"

#include <M5Cardputer.h>
#include <SD.h>

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

constexpr uint32_t kRate = 16000;
constexpr size_t kBlock = 240;  // 15 ms, matches M5's mic_wav_record example

auto& lcd = M5Cardputer.Display;
bool sdOk = false;
bool recording = false;
bool micOk = false;
bool dirty = true;
uint32_t prevActions = 0;
uint32_t samples = 0;
uint32_t savedBytes = 0;
int peak = 0;
char status[64] = "S start";
char lastFile[64] = {0};
File wav;

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

void draw()
{
    lcd.fillScreen(kBg);
    header();
    lcd.setTextColor(recording ? kWarn : kText, kBg);
    lcd.drawString(status, 4, 24);
    lcd.setTextColor(micOk ? kOk : kWarn, kBg);
    lcd.drawString(micOk ? "MIC OK" : "MIC FAIL", 4, 36);
    char line[64];
    snprintf(line, sizeof(line), "TIME %lu.%01lus", static_cast<unsigned long>(samples / kRate),
             static_cast<unsigned long>((samples % kRate) / (kRate / 10)));
    lcd.setTextColor(kText, kBg);
    lcd.drawString(line, 4, 48);
    snprintf(line, sizeof(line), "FILE %s", lastFile[0] ? lastFile : "-");
    lcd.setTextColor(kDim, kBg);
    lcd.drawString(line, 4, 60);
    snprintf(line, sizeof(line), "DATA %lu B", static_cast<unsigned long>(savedBytes));
    lcd.drawString(line, 4, 72);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("LEVEL", 4, 88);
    lcd.drawRect(52, 88, 180, 10, kDim);
    const int w = peak * 176 / 32767;
    lcd.fillRect(54, 90, w, 6, peak > 20000 ? kWarn : kOk);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("WAV 16 kHz mono", 4, 102);
    static const Hint h[] = {{"S", "record"}, {"P", "play"}, {"esc", "back"}};
    hints(h, 3);
    dirty = false;
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
        samples = 0;
        peak = 0;
        return true;
    }
    return false;
}

void beginMic()
{
    M5.Mic.end();
    M5.Speaker.end();   // official Cardputer mic_wav_record sequence
    delay(100);
    micOk = M5.Mic.begin();
    if (micOk) {
        M5.Mic.setSampleRate(kRate);
        delay(1200);  // ES8311 ADC warm-up
    }
    Serial.printf("[rec] mic=%d\n", micOk ? 1 : 0);
}

void stopRecording()
{
    if (!recording) return;
    recording = false;
    writeHeader(samples * 2);
    wav.close();
    savedBytes = samples * 2;
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
    static int16_t buf[kBlock];
    bool sReleased = false;
    int drawCounter = 0;
    while (recording) {
        if (M5Cardputer.Keyboard.isKeyPressed('`') ||
            M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
            break;
        }
        bool armed = false;
        for (int retry = 0; retry < 8 && !armed; ++retry) {
            armed = M5.Mic.record(buf, kBlock, kRate, false);
            if (!armed) delay(5);
        }
        if (!armed) {
            snprintf(status, sizeof(status), "MIC BUSY");
            break;
        }
        // The official mic_wav_record example does not spin on isRecording();
        // it records the next block while the previous one drains.  240
        // samples are 15 ms; 20 ms is enough for one double-buffered block.
        delay(20);
        if (samples == 0) writeHeader(0);
        const size_t wrote = wav.write(reinterpret_cast<const uint8_t*>(buf), kBlock * 2);
        if (wrote != kBlock * 2) {
            snprintf(status, sizeof(status), "SD WRITE FAILED");
            break;
        }
        samples += kBlock;
        int p = 0;
        for (size_t i = 0; i < kBlock; ++i) {
            const int v = abs(buf[i]);
            if (v > p) p = v;
        }
        peak = p;
        if (++drawCounter >= 8) {
            drawCounter = 0;
            dirty = true;
        }
        const bool sNow = M5Cardputer.Keyboard.isKeyPressed('s') ||
                          M5Cardputer.Keyboard.isKeyPressed('S');
        if (!sNow) sReleased = true;
        if (sReleased && sNow) break;
        if (samples > kRate * 300) {  // 5 minute safety cap
            snprintf(status, sizeof(status), "MAX 5 MIN");
            break;
        }
    }
    stopRecording();
    draw();
}

void playLast()
{
    if (!lastFile[0]) {
        snprintf(status, sizeof(status), "NO RECORDING");
        dirty = true;
        return;
    }
    File f = SD.open(lastFile, FILE_READ);
    if (!f) return;
    const size_t bytes = f.size();
    if (bytes <= 44) {
        f.close();
        snprintf(status, sizeof(status), "EMPTY RECORDING");
        dirty = true;
        return;
    }
    if (bytes > 400000) {
        f.close();
        snprintf(status, sizeof(status), "TOO LARGE TO PLAY");
        dirty = true;
        return;
    }
    auto data = std::unique_ptr<int16_t[]>(new (std::nothrow) int16_t[(bytes - 44) / 2]);
    if (!data) {
        f.close();
        snprintf(status, sizeof(status), "NO MEMORY");
        dirty = true;
        return;
    }
    f.seek(44);
    f.read(reinterpret_cast<uint8_t*>(data.get()), bytes - 44);
    f.close();
    M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(160);
    M5.Speaker.playRaw(data.get(), (bytes - 44) / 2, kRate, false);
    while (M5.Speaker.isPlaying()) {
        M5Cardputer.update();
        delay(10);
    }
    M5.Speaker.end();
    beginMic();
    snprintf(status, sizeof(status), "PLAYED");
    dirty = true;
}

enum : uint32_t { kBack = 1, kStart = 2, kPlay = 4 };

uint32_t readActions()
{
    uint32_t a = 0;
    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t c = static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        if (c == '`' || c == '~' || c == 0x1B) a |= kBack;
        if (c == 's' || c == 'S') a |= kStart;
        if (c == 'p' || c == 'P') a |= kPlay;
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
    if (!sdOk) snprintf(status, sizeof(status), "NO TF CARD");
    else beginMic();
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
        draw();
        return;
    }
    if (pressed & kBack) {
        M5.Mic.end();
        launcher::rebootInto(launcher::kAppMenu);
        return;
    }
    if (pressed & kStart) {
        if (recording) stopRecording();
        else startRecording();
        draw();
    }
    if (pressed & kPlay) {
        playLast();
        draw();
    }
    if (dirty) draw();
}

}  // namespace recapp
