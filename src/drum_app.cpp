#include "drum_app.h"

#include <M5Cardputer.h>
#include <Preferences.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "launcher.h"

namespace drumapp {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kRed    = 0xF800;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;
constexpr int kTracks      = 8;
constexpr int kSteps       = 16;
constexpr uint32_t kRate   = 16000;
constexpr size_t kMaxSamples = 8192;  // 512 ms at 16 kHz, enough for 60 BPM

auto& lcd = M5Cardputer.Display;
const char* const kNames[kTracks] = {"KICK", "SNARE", "CLAP", "HAT",
                                     "OHAT", "TOM", "LTOM", "PERC"};

uint16_t patterns[kTracks] = {
    0x1111,  // kick 1 and 3
    0x1010,  // snare 2 and 4
    0x0000,
    0x5555,  // closed hat on every eighth
    0x4040,  // open hat at 7 and 15
    0x0000,
    0x0000,
    0x0000,
};
uint8_t muted = 0;
int bpm = 120;
uint8_t volume = 180;
int track = 0;
int step = 0;
int playStep = 0;
bool playing = false;
bool dirty = true;
uint32_t nextStepMs = 0;
uint32_t rng = 0x12345678;
int16_t stepBuf[kMaxSamples];
float mixBuf[kMaxSamples];
size_t stepSamples = 0;
size_t stepOffset = 0;

uint32_t white()
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

float noise()
{
    return (static_cast<int32_t>(white() >> 8) - 8388608) / 8388608.0f;
}

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float env(float t, float decay)
{
    return expf(-t * decay);
}

void addVoice(float* mix, size_t count, int which, float t0)
{
    // The voices are deliberately tiny and synthetic: no samples means this
    // fits an 8 MB/no-PSRAM Cardputer and starts instantly.
    for (size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / kRate - t0;
        if (t < 0.0f) continue;
        float v = 0.0f;
        switch (which) {
            case 0: {  // kick
                const float f = 45.0f + 95.0f * expf(-t * 28.0f);
                v = sinf(6.2831853f * f * t) * env(t, 7.0f) * 1.05f;
                v += sinf(6.2831853f * 55.0f * t) * env(t, 18.0f) * 0.25f;
                break;
            }
            case 1: {  // snare
                const float body = sinf(6.2831853f * 185.0f * t) * env(t, 22.0f);
                v = body * 0.45f + noise() * env(t, 16.0f) * 0.65f;
                break;
            }
            case 2: {  // clap: three short noise bursts
                float burst = 0.0f;
                for (float d : {0.0f, 0.012f, 0.024f}) {
                    if (t >= d) burst += expf(-(t - d) * 95.0f) * (t - d < 0.035f ? 1.0f : 0.0f);
                }
                v = noise() * burst * 0.7f;
                break;
            }
            case 3: {  // closed hat
                const float n = noise() - 0.35f * noise();
                v = n * env(t, 65.0f) * 0.38f;
                break;
            }
            case 4: {  // open hat
                const float n = noise() - 0.25f * noise();
                v = n * env(t, 13.0f) * 0.28f;
                break;
            }
            case 5: {  // tom
                const float f = 95.0f + 85.0f * expf(-t * 16.0f);
                v = sinf(6.2831853f * f * t) * env(t, 11.0f) * 0.8f;
                break;
            }
            case 6: {  // low tom
                const float f = 55.0f + 55.0f * expf(-t * 12.0f);
                v = sinf(6.2831853f * f * t) * env(t, 9.0f) * 0.9f;
                break;
            }
            default: {  // perc / rim
                v = (noise() * 0.45f + sinf(6.2831853f * 1250.0f * t) * 0.5f) *
                    env(t, 42.0f) * 0.55f;
                break;
            }
        }
        mix[i] += v;
    }
}

void renderStep(int s)
{
    const float seconds = 60.0f / static_cast<float>(bpm) / 4.0f;
    stepSamples = std::min(kMaxSamples, static_cast<size_t>(seconds * kRate));
    if (stepSamples < 64) stepSamples = 64;
    memset(mixBuf, 0, sizeof(float) * stepSamples);
    for (int t = 0; t < kTracks; ++t) {
        if (muted & (1u << t)) continue;
        if (patterns[t] & (1u << s)) addVoice(mixBuf, stepSamples, t, 0.0f);
    }
    for (size_t i = 0; i < stepSamples; ++i) {
        const float v = clampf(mixBuf[i], -1.0f, 1.0f);
        stepBuf[i] = static_cast<int16_t>(v * 30000.0f);
    }
    stepOffset = 0;
}

void load()
{
    Preferences p;
    if (!p.begin("drum", true)) return;
    bpm = p.getInt("bpm", bpm);
    volume = p.getUChar("vol", volume);
    muted = p.getUChar("mute", muted);
    for (int i = 0; i < kTracks; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "p%d", i);
        patterns[i] = p.getUShort(key, patterns[i]);
    }
    p.end();
    bpm = std::max(60, std::min(220, bpm));
}

void save()
{
    Preferences p;
    if (!p.begin("drum", false)) return;
    p.putInt("bpm", bpm);
    p.putUChar("vol", volume);
    p.putUChar("mute", muted);
    for (int i = 0; i < kTracks; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "p%d", i);
        p.putUShort(key, patterns[i]);
    }
    p.end();
}

void header()
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString("DRUM", 3, 2);
    char b[40];
    snprintf(b, sizeof(b), "%d BPM  V%d%%", bpm, volume * 100 / 255);
    lcd.drawRightString(b, lcd.width() - 3, 2);
}

void draw()
{
    lcd.fillScreen(kBg);
    header();
    lcd.setTextFont(1);
    const int gx = 38;
    const int gy = 24;
    const int cw = 11;
    const int ch = 9;
    for (int t = 0; t < kTracks; ++t) {
        const int y = gy + t * ch;
        lcd.setTextColor((muted & (1u << t)) ? kDim : kText, kBg);
        lcd.drawString(kNames[t], 2, y);
        for (int s = 0; s < kSteps; ++s) {
            const int x = gx + s * cw;
            const bool on = patterns[t] & (1u << s);
            const bool playingHere = playing && s == playStep;
            uint16_t fill = on ? (playingHere ? kOk : kAccent) : (playingHere ? kBar : 0x0841);
            lcd.fillRect(x, y, cw - 1, ch - 1, fill);
            if (t == track && s == step) {
                lcd.drawRect(x - 1, y - 1, cw + 1, ch + 1, kRed);
            }
        }
    }
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("step ", 2, 108);
    char pos[32];
    snprintf(pos, sizeof(pos), "%02d  track %s%s", step + 1, kNames[track],
             (muted & (1u << track)) ? " MUTE" : "");
    lcd.drawString(pos, 30, 108);
    static const char* hints[] = {"ENT hit", "P play", "+/- bpm", "esc back"};
    int x = 3;
    for (const char* h : hints) {
        lcd.setTextColor(kOk, kBg);
        lcd.drawString(h, x, 121);
        x += lcd.textWidth(h) + 8;
    }
    dirty = false;
}

void togglePlay()
{
    playing = !playing;
    if (playing) {
        M5.Speaker.begin();
        M5.Speaker.setVolume(volume);
        playStep = 0;
        nextStepMs = millis();
        renderStep(playStep);
        M5.Speaker.playRaw(stepBuf, stepSamples, kRate, false);
        playStep = 1;
        nextStepMs = millis() + (60000u / bpm / 4u);
    } else {
        M5.Speaker.stop();
        M5.Speaker.end();
    }
    dirty = true;
}

void tickAudio()
{
    if (!playing) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextStepMs) < 0) return;
    const int s = playStep;
    renderStep(s);
    M5.Speaker.stop();
    M5.Speaker.playRaw(stepBuf, stepSamples, kRate, false);
    playStep = (playStep + 1) % kSteps;
    const uint32_t stepMs = 60000u / bpm / 4u;
    nextStepMs += stepMs;
    if (static_cast<int32_t>(now - nextStepMs) > static_cast<int32_t>(stepMs)) {
        nextStepMs = now + stepMs;
    }
    dirty = true;
}

void handle()
{
    if (!M5Cardputer.Keyboard.isChange()) return;
    const auto& ks = M5Cardputer.Keyboard.keysState();
    char c = 0;
    if (ks.enter) c = '\n';
    else if (ks.del) c = '\b';
    else if (!ks.word.empty()) c = ks.word[0];
    if (c == 0) return;

    if (c == '`') {
        if (playing) togglePlay();
        save();
        launcher::rebootInto(launcher::kAppMenu);
        return;
    }
    if (c == '\n') {
        patterns[track] ^= (1u << step);
        save();
        dirty = true;
        return;
    }
    if (c == 'p' || c == 'P') {
        togglePlay();
        return;
    }
    if (c == ';' && step > 0) { --step; dirty = true; return; }
    if (c == '.' && step + 1 < kSteps) { ++step; dirty = true; return; }
    if (c == ',' && track > 0) { --track; dirty = true; return; }
    if (c == '/' && track + 1 < kTracks) { ++track; dirty = true; return; }
    if (c == '=') { bpm = std::min(220, bpm + 5); save(); dirty = true; return; }
    if (c == '-') { bpm = std::max(60, bpm - 5); save(); dirty = true; return; }
    if (c == 'm' || c == 'M') {
        muted ^= (1u << track);
        save();
        dirty = true;
        return;
    }
}

}  // namespace

void setup()
{
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    lcd.setRotation(rfcfg::screenRotation);
    lcd.setTextFont(1);
    lcd.setTextSize(1);
    lcd.setTextWrap(false);
    load();
    dirty = true;
    draw();
}

void loop()
{
    M5Cardputer.update();
    handle();
    tickAudio();
    if (dirty) draw();
    delay(2);
}

}  // namespace drumapp
