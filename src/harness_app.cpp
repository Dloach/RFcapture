/*
 * HARNESS - live DeepSeek Harness sessions on the Cardputer.
 *
 * Home screen is the Session deck (title / preview / short id).  ENT opens a
 * Session, Fn+N creates one, Fn+S returns to the deck, Fn+V dictates, and
 * Fn+; / Fn+. scroll the transcript.
 *
 * The whole UI is drawn into one 240x135 M5Canvas backbuffer and pushed only
 * when something changed, so a key press never repaints the panel from scratch.
 * The network side is a non-blocking WebSocket client (harness_ws.*): no
 * HTTPClient, no request/response blocking in the loop.
 *
 * The Host plugin (harness-ws/) shares the Desktop's own Session through
 * ctx.sessionController, so this is the same conversation, not a relay copy.
 */

#include "harness_app.h"

#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <esp_system.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <vector>

#include "harness_ws.h"
#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "usb_log.h"
#include "wifi_store.h"

namespace harnessapp {
namespace {

// Palette (RGB565).
constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kBar    = 0x2104;
constexpr uint16_t kHead   = 0x18E3;

constexpr uint16_t kPort   = 9101;
constexpr int      kDeckRows = 4;
// Keep the transcript bounded: 160 wrapped lines is ~7 screens and roughly
// 12 KiB, which leaves the canvas the largest allocation on the heap.
constexpr int      kMaxLines       = 160;
constexpr int      kMaxStreamLines = 48;
// Requested page sizes: small frames keep the device's heap flat.  A 10-message
// snapshot plus its JSON document was the peak that pushed the heap low.
constexpr int      kDeckPage = 6;
constexpr int      kChatPage = 6;
constexpr int      kMaxInput = 300;

// Voice capture constants and buffers live with the capture code below; they
// follow rec_app.cpp's double-buffered release-callback scheme.

auto& lcd = M5Cardputer.Display;
M5Canvas canvas;

enum class Screen : uint8_t { kDeck, kChat, kConfig };
Screen screen = Screen::kDeck;
bool   dirty  = true;

harnessws::Client ws;

String host  = "192.168.0.88";
String token = "cardputer-dev";
uint16_t port = kPort;
String sessionId;
String statusText = "STARTING";

struct SessionItem {
    String   id;
    String   shortId;
    String   title;
    String   preview;
    uint32_t updatedAt = 0;
    bool     running   = false;
    bool     blank     = true;
};
std::vector<SessionItem> sessions;
int deckSel = 0;
int deckTop = 0;

String streamText;

// Wrapped transcript, kept as *lines only*.  The raw message list is not
// retained: on a 320 KiB board two copies of a long chat (raw text + wrapped
// lines) is what pushed the heap into an abort a few keys later.  Lines are
// capped and the oldest are dropped, so memory stays flat however long the
// conversation gets.
std::vector<String> committedLines;
std::vector<String> streamLines;
int  scroll     = 0;
uint16_t parseFailures = 0;

bool   running = false;
String input;
String lastError;
int    configField = 0;
String configEdit;

bool wifiStarted = false;
bool wsStarted   = false;
bool wifiOk      = false;
bool canvasReady = false;

enum class Voice : uint8_t { kIdle, kWarmup, kRecording, kWaiting };
Voice    voice      = Voice::kIdle;
uint32_t voiceStartedMs = 0;

// ---------------------------------------------------------------------------
// small helpers

void markDirty()
{
    dirty = true;
}

int utf8Len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    return 4;
}

void deleteLastUtf8(String& s)
{
    if (s.isEmpty()) return;
    int i = static_cast<int>(s.length()) - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.remove(i);
}

/// Cut to maxChars whole UTF-8 characters.  Slicing a String by bytes would
/// leave half a Chinese character (or half an emoji) at the end of every line.
String truncateUtf8(const String& text, int maxChars)
{
    int chars = 0;
    int i     = 0;
    const int len = static_cast<int>(text.length());
    while (i < len && chars < maxChars) {
        int n = utf8Len(static_cast<unsigned char>(text[i]));
        if (i + n > len) n = len - i;
        i += n;
        ++chars;
    }
    if (i >= len) return text;
    return text.substring(0, i) + "...";
}

String jsonEscape(const String& value)
{
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04X", static_cast<unsigned char>(c));
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// transcript wrapping

void wrapInto(std::vector<String>& out, const String& text, const String& prefix, int maxWidth)
{
    const int prefixWidth = canvas.textWidth(prefix);
    String line = prefix;
    int    i    = 0;
    while (i < static_cast<int>(text.length())) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int n = utf8Len(c);
        if (i + n > static_cast<int>(text.length())) n = static_cast<int>(text.length()) - i;
        const String part = text.substring(i, i + n);
        const int width = canvas.textWidth(line + part);
        if (width > maxWidth && line.length() > prefix.length()) {
            out.push_back(line);
            line = "  ";
            line += part;
        } else {
            line += part;
        }
        i += n;
    }
    if (line.length() > prefix.length()) out.push_back(line);
}

/// Append a streamed fragment to the tail.  Rebuilding every wrapped line on
/// each delta churned hundreds of small Strings per second and fragmented the
/// heap; this only touches the last line.
void appendStream(const String& delta)
{
    if (streamLines.empty()) streamLines.push_back("AI:  ");
    if (streamText.length() < 8000) streamText += delta;

    canvas.setFont(&lgfx::fonts::efontCN_12);
    const int maxWidth = lcd.width() - 6;
    int i = 0;
    const int len = static_cast<int>(delta.length());
    while (i < len) {
        int n = utf8Len(static_cast<unsigned char>(delta[i]));
        if (i + n > len) n = len - i;
        const String part = delta.substring(i, i + n);
        if (canvas.textWidth(streamLines.back() + part) > maxWidth) {
            streamLines.push_back("  ");
            streamLines.back() += part;
        } else {
            streamLines.back() += part;
        }
        i += n;
    }
    canvas.setTextFont(1);
    if (streamLines.size() > kMaxStreamLines) {
        streamLines.erase(streamLines.begin(), streamLines.begin() + (streamLines.size() - kMaxStreamLines));
    }
}

/// Append one message's wrapped text, trimming the oldest lines to stay bounded.
void appendWrapped(const String& text, const String& prefix)
{
    canvas.setFont(&lgfx::fonts::efontCN_12);
    wrapInto(committedLines, text, prefix, lcd.width() - 6);
    canvas.setTextFont(1);
    if (committedLines.size() > kMaxLines) {
        committedLines.erase(committedLines.begin(), committedLines.begin() + (committedLines.size() - kMaxLines));
    }
}

int totalLines()
{
    return static_cast<int>(committedLines.size() + streamLines.size());
}

/// The i-th line of the committed+streaming concatenation, without copying either.
const String* lineAt(int index)
{
    if (index < 0) return nullptr;
    const int committed = static_cast<int>(committedLines.size());
    if (index < committed) return &committedLines[index];
    const int tail = index - committed;
    if (tail < static_cast<int>(streamLines.size())) return &streamLines[tail];
    return nullptr;
}

// ---------------------------------------------------------------------------
// network

void requestSessions()
{
    char frame[64];
    snprintf(frame, sizeof(frame), "{\"type\":\"sessions\",\"limit\":%d}", kDeckPage);
    ws.sendText(String(frame));
}

void openSession(const String& id)
{
    sessionId = id;
    streamText = "";
    committedLines.clear();
    streamLines.clear();
    scroll = 0;
    running = false;
    statusText = "LOADING";
    screen = Screen::kChat;
    markDirty();

    char frame[128];
    snprintf(frame, sizeof(frame), "{\"type\":\"open\",\"sessionId\":\"%s\",\"maxMessages\":%d}",
             jsonEscape(id).c_str(), kChatPage);
    ws.sendText(String(frame));
}

void sendPrompt()
{
    if (input.isEmpty()) return;
    if (sessionId.isEmpty()) {
        statusText = "SELECT CHAT";
        markDirty();
        return;
    }
    if (!ws.open()) {
        statusText = "OFFLINE";
        markDirty();
        return;
    }
    String frame = "{\"type\":\"prompt\",\"sessionId\":\"";
    frame += jsonEscape(sessionId);
    frame += "\",\"text\":\"";
    frame += jsonEscape(input);
    frame += "\"}";
    if (ws.sendText(frame)) {
        input = "";
        statusText = "SENT";
    } else {
        statusText = "SEND FAILED";
    }
    markDirty();
}

void sendCancel()
{
    if (sessionId.isEmpty() || !ws.open()) return;
    String frame = "{\"type\":\"cancel\",\"sessionId\":\"";
    frame += jsonEscape(sessionId);
    frame += "\"}";
    ws.sendText(frame);
    statusText = "CANCEL";
    markDirty();
}

void applySessions(JsonArray items)
{
    sessions.clear();
    sessions.reserve(items.size());
    for (JsonObject item : items) {
        SessionItem s;
        s.id        = item["id"].as<String>();
        s.shortId   = item["shortId"].as<String>();
        s.title     = item["title"].as<String>();
        s.preview   = item["preview"].as<String>();
        s.updatedAt = item["updatedAt"] | 0;
        s.running   = item["running"] | false;
        s.blank     = item["blank"] | true;
        if (s.id.isEmpty()) continue;
        if (s.title.isEmpty()) s.title = s.blank ? "New session" : "(untitled)";
        if (s.preview.isEmpty()) s.preview = s.blank ? "No messages yet" : "";
        sessions.push_back(s);
    }
    if (deckSel >= static_cast<int>(sessions.size())) deckSel = std::max(0, static_cast<int>(sessions.size()) - 1);
    if (deckTop > deckSel) deckTop = deckSel;
    markDirty();
}

void applySnapshot(JsonArray messages)
{
    committedLines.clear();
    streamText = "";
    streamLines.clear();
    scroll = 0;
    for (JsonObject item : messages) {
        const String role = item["role"].as<String>();
        const String text = item["text"].as<String>();
        if (text.isEmpty()) continue;
        appendWrapped(text, role == "user" ? "You: " : "AI:  ");
    }
    statusText = "READY";
    markDirty();
}

void handleMessage(const char* json, size_t length)
{
    // Parsing a frame with almost no heap left is how loading a session turned
    // into a panic (reset=4).  Refuse it and say so instead of dying.
    if (ESP.getFreeHeap() < 18000) {
        usbLogf("[rx] skip len=%u low heap=%u", static_cast<unsigned>(length), ESP.getFreeHeap());
        return;
    }
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json, length);
    if (error) {
        // A rejected frame used to be silent, which looked exactly like "the
        // list is empty".  Report it once per few failures over USB.
        ++parseFailures;
        if (parseFailures <= 5) {
            usbLogf("[rx] parse fail %s len=%u head=%.60s", error.c_str(), static_cast<unsigned>(length), json);
        }
        return;
    }
    const char* type = doc["type"] | "";
    static uint16_t rxLogged = 0;
    if (rxLogged < 80) {
        ++rxLogged;
        usbLogf("[rx] %s len=%u free=%u", type, static_cast<unsigned>(length), ESP.getFreeHeap());
    }

    if (strcmp(type, "hello") == 0) {
        statusText = "LINK UP";
        requestSessions();
    } else if (strcmp(type, "ping") == 0) {
        // The Host times out a client that goes quiet; answer its keepalive.
        ws.sendText("{\"type\":\"pong\"}");
    } else if (strcmp(type, "sessions") == 0) {
        applySessions(doc["items"].as<JsonArray>());
        statusText = String(static_cast<int>(sessions.size())) + " CHATS";
    } else if (strcmp(type, "opened") == 0) {
        statusText = "LOADING";
    } else if (strcmp(type, "created") == 0) {
        const String id = doc["sessionId"].as<String>();
        if (!id.isEmpty()) openSession(id);
    } else if (strcmp(type, "snapshot") == 0) {
        applySnapshot(doc["messages"].as<JsonArray>());
    } else if (strcmp(type, "delta") == 0) {
        const char* text = doc["text"] | "";
        if (text[0] != '\0') {
            appendStream(String(text));
            markDirty();
        }
    } else if (strcmp(type, "message") == 0) {
        const char* role = doc["role"] | "";
        const char* text = doc["text"] | "";
        if (text[0] != '\0') {
            const bool assistant = strcmp(role, "assistant") == 0;
            if (assistant) {
                streamText = "";
                streamLines.clear();
            }
            appendWrapped(String(text), assistant ? "AI:  " : "You: ");
            if (scroll > 0) ++scroll;
            markDirty();
        }
    } else if (strcmp(type, "status") == 0) {
        running = doc["running"] | false;
        markDirty();
    } else if (strcmp(type, "accepted") == 0) {
        statusText = "ACCEPTED";
        markDirty();
    } else if (strcmp(type, "cancelled") == 0) {
        statusText = "CANCELLED";
        markDirty();
    } else if (strcmp(type, "stt") == 0) {
        const char* text = doc["text"] | "";
        if (text[0] != '\0') input += text;
        statusText = "VOICE OK";
        voice = Voice::kIdle;
        markDirty();
    } else if (strcmp(type, "stt.error") == 0) {
        lastError  = doc["error"] | "voice failed";
        statusText = "VOICE FAILED";
        voice = Voice::kIdle;
        markDirty();
    } else if (strcmp(type, "error") == 0) {
        lastError  = doc["error"] | "error";
        statusText = "ERR";
        markDirty();
    }

    // The two frames that carry bulk text: log the heap after they are applied,
    // so the next crash is measurable rather than a guess.
    if (strcmp(type, "snapshot") == 0 || strcmp(type, "sessions") == 0) {
        usbLogf("[rx] %s done free=%u lines=%d", type, ESP.getFreeHeap(), totalLines());
    }
}

// harnessws hands over a length-delimited buffer; the JSON parser takes both.
void handleWsMessage(const char* data, size_t length, void* arg)
{
    (void)arg;
    handleMessage(data, length);
}

// ---------------------------------------------------------------------------
// voice

void render();  // defined in the rendering section below

void voiceAbort(const char* why)
{
    M5.Mic.end();
    if (voice != Voice::kIdle) ws.sendText("{\"type\":\"stt.abort\"}");
    voice      = Voice::kIdle;
    statusText = "VOICE FAILED";
    lastError  = why;
    usbLogf("[voice] abort: %s", why);
    markDirty();
}

// --- capture: direct I2S read -----------------------------------------------
// M5.Mic.begin() initialises the ES8311 (I2C registers) and installs the I2S
// driver for the microphone (port 0, BCLK 41, WS 43, DIN 46 on Cardputer ADV).
// Its record()/isRecording() request machinery delivers a buffer only every
// ~3 s here, so the audio is pulled straight off the installed driver with
// i2s_read() instead: one blocking read per 240 ms block, continuously.
constexpr size_t   kVoiceSamples = 3840;   // 240 ms at 16 kHz per read/send
constexpr uint32_t kVoiceRate    = 16000;
constexpr uint32_t kVoiceMaxMs   = 20000;  // hard cap; ENT/Fn+V stops earlier

int16_t voiceReadBuf[kVoiceSamples];
uint8_t voiceB64[10800];
uint8_t  voiceLevel  = 0;  // 0..100 for the on-screen meter
uint32_t voiceMeasuredHz = 0;
uint32_t voiceHwHz = 0;    // true hardware I2S rate from a pure-read self-test
bool     g_micUp = false;  // mic/codec brought up once, shared by self-test + capture

size_t voiceSendPcm(const int16_t* pcm, size_t samples)
{
    size_t encodedLen = 0;
    if (mbedtls_base64_encode(voiceB64, sizeof(voiceB64), &encodedLen,
                              reinterpret_cast<const unsigned char*>(pcm),
                              samples * sizeof(int16_t)) != 0) {
        return 0;
    }
    String frame;
    frame.reserve(encodedLen + 40);
    frame += "{\"type\":\"stt.chunk\",\"data\":\"";
    frame.concat(reinterpret_cast<const char*>(voiceB64), encodedLen);
    frame += "\"}";
    return ws.sendText(frame) ? encodedLen : 0;
}

/// Dump the ES8311 register file after M5Unified configured it: that dump is
/// the exact codec state a self-contained I2S bring-up has to reproduce.
void voiceDumpCodec()
{
    for (int base = 0; base < 0x20; base += 8) {
        char line[96];
        int  n = snprintf(line, sizeof(line), "[voice] reg%02X:", base);
        for (int r = base; r < base + 8 && n < static_cast<int>(sizeof(line)) - 4; ++r) {
            uint8_t v = 0;
            if (!M5.In_I2C.readRegister(0x18, static_cast<uint8_t>(r), &v, 1, 100000)) v = 0xEE;
            n += snprintf(line + n, sizeof(line) - n, " %02X", static_cast<unsigned>(v));
        }
        usbLogf("%s", line);
    }
}

/// I2S port in use for capture; M5Unified installs the mic on port 0.
i2s_port_t g_voicePort = I2S_NUM_0;

// Diagnostic: measure the TRUE hardware I2S rate with pure blocking reads — no
// render, no WS send, no keyboard scan — so the capture loop cannot starve it.
// This separates "bit clock wrong" (hw rate low here too) from "loop drops
// samples" (hw rate fine here, low only in the capture loop). Runs once, no
// speech needed. The steady-state rate is ~16 kHz, matching the rate:16000 the
// device reports to the Host (the capture loop reads 3840 samples in ~237 ms).
void voiceSelfTest()
{
    uint8_t r01 = 0xEE, r02 = 0xEE;
    M5.In_I2C.readRegister(0x18, 0x01, &r01, 1, 100000);
    M5.In_I2C.readRegister(0x18, 0x02, &r02, 1, 100000);

    int16_t  buf[256];
    // Drain the RX backlog first. After begin() + the settle delays the DMA
    // holds samples captured before this test started; counting them inflates
    // the rate (an un-drained 600 ms window read ~17.5 kHz instead of ~16 kHz).
    const uint32_t drainStart = millis();
    while (millis() - drainStart < 250) {
        size_t got = 0;
        i2s_read(g_voicePort, buf, sizeof(buf), &got, 100 / portTICK_PERIOD_MS);
    }

    uint32_t totalBytes = 0;
    const uint32_t start = millis();
    while (millis() - start < 600) {
        size_t got = 0;
        if (i2s_read(g_voicePort, buf, sizeof(buf), &got, 100 / portTICK_PERIOD_MS) == ESP_OK) {
            totalBytes += got;
        }
    }
    const uint32_t elapsed = millis() - start;
    voiceHwHz = elapsed ? (totalBytes / 2) * 1000u / elapsed : 0;
    usbLogf("[voice] SELFTEST hw=%uHz bytes=%u/%ums ES8311 01=%02X 02=%02X",
            static_cast<unsigned>(voiceHwHz), static_cast<unsigned>(totalBytes),
            static_cast<unsigned>(elapsed), static_cast<unsigned>(r01), static_cast<unsigned>(r02));
    // Persist so the result survives reboot and can be read back at any time
    // (the periodic [mem] line carries it), not just in the live USB window.
    Preferences p;
    if (p.begin("harness", false)) { p.putUInt("hwHz", voiceHwHz); p.end(); }
}

bool voiceI2sBegin()
{
    usbLogf("[voice] begin");

    // Let M5Unified bring the ES8311 up exactly as it does for its own capture
    // (its register file is the only one that produced real audio here).
    M5.Speaker.setVolume(0);
    const bool powered = M5.Speaker.begin();
    M5.Speaker.stop();
    delay(30);

    auto micCfg = M5.Mic.config();
    micCfg.magnification      = 16;
    micCfg.noise_filter_level = 0;
    // Deepen the RX DMA (default 8x128 = 1024 samples) so the ~24 ms WS send per
    // block does not overflow the FIFO during a long dictation: 12x256 = 3072
    // samples gives ~192 ms of slack instead of ~64 ms. Kept below 16x256 so the
    // boot low-watermark stays clear of the ~19 KiB session-load panic floor.
    micCfg.dma_buf_len        = 256;
    micCfg.dma_buf_count      = 12;
    M5.Mic.config(micCfg);
    if (!M5.Mic.begin()) {
        usbLogf("[voice] mic begin failed");
        return false;
    }
    delay(200);
    usbLogf("[voice] speaker=%d codec ready", powered ? 1 : 0);
    voiceDumpCodec();

    // Keep M5Unified's I2S channel and read it directly. The ES8311 is an I2S
    // slave (reg 0x01=0xBA: MCLK=BCLK), clocked by the S3's I2S peripheral. Its
    // record()/isRecording() request machinery only hands back a buffer every
    // few seconds here, so i2s_read() pulls samples straight off the driver.
    g_voicePort = I2S_NUM_0;
    usbLogf("[voice] reading M5Unified's channel on port 0");
    delay(30);
    usbLogf("[voice] ready (borrowed)");
    voiceSelfTest();
    return true;
}

size_t voiceRead(int16_t* buffer, size_t samples, uint32_t timeoutMs)
{
    size_t bytesRead = 0;
    const esp_err_t err = i2s_read(g_voicePort, buffer, samples * sizeof(int16_t), &bytesRead,
                                   timeoutMs / portTICK_PERIOD_MS);
    if (err != ESP_OK) return 0;
    return bytesRead / sizeof(int16_t);
}

/// +6 dB, soft-limit the peaks (as Hermes does) and track the peak for the log.
void voiceCondition(int16_t* buffer, size_t count, int32_t& peak)
{
    for (size_t i = 0; i < count; ++i) {
        int32_t value   = static_cast<int32_t>(buffer[i]) * 2;
        const bool negative = value < 0;
        int32_t magnitude = negative ? -value : value;
        if (magnitude > 24500) magnitude = 24500 + (magnitude - 24500) / 4;
        if (magnitude > 32767) magnitude = 32767;
        if (magnitude > peak) peak = magnitude;
        buffer[i] = static_cast<int16_t>(negative ? -magnitude : magnitude);
    }
}

bool voiceCapture(uint32_t maxMs, bool allowStop)
{
    // Initialise once and leave the codec running: end()/begin() around every
    // capture is what left the ADC delivering a couple of buffers and stalling.
    if (!g_micUp) {
        if (!voiceI2sBegin()) return false;
        g_micUp = true;
    }

    ws.sendText(String("{\"type\":\"stt.begin\",\"rate\":") + kVoiceRate +
                ",\"channels\":1,\"bits\":16}");

    voiceLevel = 0;
    int     sent   = 0;
    int32_t peak   = 0;
    uint32_t totalSamples = 0;
    const uint32_t start = millis();
    while (millis() - start < maxMs) {
        const uint32_t tRead0 = millis();
        const size_t got = voiceRead(voiceReadBuf, kVoiceSamples, 500);
        const uint32_t tRead1 = millis();
        if (got == 0) continue;

        voiceCondition(voiceReadBuf, got, peak);
        int32_t blockPeak = 0;
        for (size_t i = 0; i < got; ++i) {
            const int32_t v = voiceReadBuf[i];
            const int32_t a = v < 0 ? -v : v;
            if (a > blockPeak) blockPeak = a;
        }
        uint32_t level = static_cast<uint32_t>(blockPeak) * 100u / 12000u;
        if (level > 100) level = 100;
        voiceLevel = static_cast<uint8_t>(level);

        const uint32_t tSend0 = millis();
        if (voiceSendPcm(voiceReadBuf, got) > 0) { ++sent; totalSamples += got; }
        const uint32_t tSend1 = millis();
        {
            // Honest loop throughput: count samples actually sent, not the
            // requested block size (partial reads mean the DMA overflowed).
            const uint32_t elapsed = millis() - start;
            if (elapsed > 0) voiceMeasuredHz = totalSamples * 1000u / elapsed;
        }
        if ((sent & 3) == 0) render();
        const uint32_t tRender1 = millis();
        M5Cardputer.update();
        const uint32_t tUpd1 = millis();

        // ENT (or Fn+V again) finishes early. handlePress only runs from loop(),
        // which is blocked inside this capture, so poll the press queue here.
        // Popping also consumes the key so it cannot fire a prompt afterwards.
        bool stop = false;
        if (allowStop) {
            Point2D_t pt;
            while (kbdPopPress(pt)) {
                const char kc = static_cast<char>(M5Cardputer.Keyboard.getKey(pt));
                const auto ks = M5Cardputer.Keyboard.keysState();
                if (kc == KEY_ENTER || (ks.fn && (kc == 'v' || kc == 'V'))) stop = true;
            }
        }

        if (sent <= 4) {
            usbLogf("[voice] buf%d got=%u read=%ums send=%ums render=%ums upd=%ums lvl=%u",
                    sent - 1, static_cast<unsigned>(got),
                    static_cast<unsigned>(tRead1 - tRead0), static_cast<unsigned>(tSend1 - tSend0),
                    static_cast<unsigned>(tRender1 - tSend1), static_cast<unsigned>(tUpd1 - tRender1),
                    static_cast<unsigned>(voiceLevel));
        }
        if (stop) break;
    }

    ws.sendText("{\"type\":\"stt.end\"}");
    usbLogf("[voice] %d buffers, measured=%uHz, %ums wall, peak=%d", sent,
            static_cast<unsigned>(voiceMeasuredHz), static_cast<unsigned>(millis() - start),
            static_cast<int>(peak));
    return sent > 0;
}

void voiceStart()
{
    if (!ws.open()) {
        statusText = "OFFLINE";
        markDirty();
        return;
    }
    if (voice != Voice::kIdle) return;

    voice      = Voice::kRecording;
    statusText = "ENT stop";
    markDirty();
    render();  // show LISTENING before the blocking capture

    const bool ok = voiceCapture(kVoiceMaxMs, true);
    if (!ok) {
        voice      = Voice::kIdle;
        statusText = "VOICE FAILED";
        lastError  = "no audio";
        markDirty();
        return;
    }

    voice          = Voice::kWaiting;
    voiceStartedMs = millis();
    statusText     = "TRANSCRIBE";
    markDirty();
    render();
}

void voiceService()
{
    // Only the transcript is asynchronous; guard against a lost reply.
    if (voice != Voice::kWaiting) return;
    if (millis() - voiceStartedMs > 30000) {
        voiceAbort("no transcript");
    }
}

// ---------------------------------------------------------------------------
// input

void deckEnsureVisible()
{
    if (deckSel < deckTop) deckTop = deckSel;
    if (deckSel >= deckTop + kDeckRows) deckTop = deckSel - kDeckRows + 1;
    if (deckTop < 0) deckTop = 0;
    const int maxTop = std::max(0, static_cast<int>(sessions.size()) - kDeckRows);
    if (deckTop > maxTop) deckTop = maxTop;
}

void scrollBy(int delta)
{
    const int total = totalLines();
    const int visible = 6;
    const int maxScroll = std::max(0, total - visible);
    scroll += delta;
    if (scroll < 0) scroll = 0;
    if (scroll > maxScroll) scroll = maxScroll;
    markDirty();
}

void openSelected()
{
    if (sessions.empty()) return;
    if (deckSel < 0 || deckSel >= static_cast<int>(sessions.size())) return;
    openSession(sessions[deckSel].id);
}

void createSession()
{
    if (!ws.open()) {
        statusText = "OFFLINE";
        markDirty();
        return;
    }
    statusText = "NEW...";
    ws.sendText("{\"type\":\"create\"}");
    markDirty();
}

void savePrefs()
{
    Preferences p;
    if (!p.begin("harness", false)) return;
    p.putString("host", host);
    p.putString("token", token);
    p.putUShort("port", port);
    p.putString("session", sessionId);
    p.end();
}

void loadPrefs()
{
    Preferences p;
    if (!p.begin("harness", true)) return;
    host    = p.getString("host", host);
    token   = p.getString("token", token);
    port    = p.getUShort("port", kPort);
    if (port == 0) port = kPort;
    sessionId = p.getString("session", "");
    p.end();
}

void restartLink()
{
    wsStarted = false;
    ws.restart();
    markDirty();
}

void handlePress(Point2D_t point)
{
    char c = static_cast<char>(M5Cardputer.Keyboard.getKey(point));
    const auto ks = M5Cardputer.Keyboard.keysState();
    const bool fn = ks.fn;

    if (screen == Screen::kConfig) {
        if (c == KEY_ENTER) {
            if (configField == 0) host = configEdit;
            else if (configField == 1) token = configEdit;
            else {
                const long parsed = configEdit.toInt();
                if (parsed > 0 && parsed < 65536) port = static_cast<uint16_t>(parsed);
            }
            savePrefs();
            restartLink();
            statusText = "RECONNECT";
            screen = Screen::kDeck;
            markDirty();
            return;
        }
        if (c == KEY_BACKSPACE) {
            deleteLastUtf8(configEdit);
            markDirty();
            return;
        }
        if (c == '`') {
            screen = Screen::kDeck;
            markDirty();
            return;
        }
        if (fn && (c == ';' || c == '.')) {
            configField = (configField + 1) % 3;
            configEdit  = configField == 0 ? host : (configField == 1 ? token : String(port));
            markDirty();
            return;
        }
        if (c >= 32 && c < 127) {
            configEdit += c;
            markDirty();
        }
        return;
    }

    if (screen == Screen::kDeck) {
        if (c == KEY_ENTER) {
            openSelected();
            return;
        }
        if (c == ';' && deckSel > 0) {
            --deckSel;
            deckEnsureVisible();
            markDirty();
            return;
        }
        if (c == '.' && deckSel + 1 < static_cast<int>(sessions.size())) {
            ++deckSel;
            deckEnsureVisible();
            markDirty();
            return;
        }
        if (c == '`') {
            launcher::rebootInto(launcher::kAppMenu);
            return;
        }
        if (fn) {
            if (c == 'n' || c == 'N') { createSession(); return; }
            if (c == 's' || c == 'S') { requestSessions(); statusText = "REFRESH"; markDirty(); return; }
            if (c == 'c' || c == 'C') {
                screen      = Screen::kConfig;
                configField = 0;
                configEdit  = host;
                markDirty();
                return;
            }
        }
        return;
    }

    // Chat.
    if (c == KEY_ENTER) {
        sendPrompt();
        return;
    }
    if (c == KEY_BACKSPACE) {
        deleteLastUtf8(input);
        markDirty();
        return;
    }
    if (c == '`') {
        screen = Screen::kDeck;
        requestSessions();
        markDirty();
        return;
    }
    if (fn) {
        if (c == 'v' || c == 'V') { voiceStart(); return; }
        if (c == 's' || c == 'S') {
            screen = Screen::kDeck;
            requestSessions();
            markDirty();
            return;
        }
        if (c == 'n' || c == 'N') { createSession(); return; }
        if (c == 'c' || c == 'C') {
            screen      = Screen::kConfig;
            configField = 0;
            configEdit  = host;
            markDirty();
            return;
        }
        if (c == ';' || c == ',' || c == '[') { scrollBy(3); return; }
        if (c == '.' || c == '/' || c == ']') { scrollBy(-3); return; }
        if (c == 'x' || c == 'X') { sendCancel(); return; }
        return;
    }
    if (c == ';' || c == ',') { scrollBy(3); return; }
    if (c == '.' || c == '/') { scrollBy(-3); return; }
    if (c >= 32 && c < 127) {
        if (input.length() < kMaxInput) input += c;
        markDirty();
    }
}

// ---------------------------------------------------------------------------
// rendering (backbuffer only)

void drawHeader()
{
    canvas.fillRect(0, 0, lcd.width(), 13, kHead);
    canvas.setTextFont(1);
    canvas.setTextColor(kText, kHead);
    canvas.drawString("HARNESS", 3, 2);

    const char* link = ws.open() ? "WS" : "..";
    canvas.setTextColor(ws.open() ? kOk : kWarn, kHead);
    canvas.drawString(link, 62, 2);
    if (running) {
        canvas.setTextColor(kAccent, kHead);
        canvas.drawString("RUN", 82, 2);
    }
    char battery[16];
    snprintf(battery, sizeof(battery), "%d%%", M5.Power.getBatteryLevel());
    canvas.setTextColor(kText, kHead);
    canvas.drawRightString(battery, lcd.width() - 3, 2);
}

void drawHints(const char* const* keys, const char* const* labels, int count)
{
    canvas.drawFastHLine(0, 118, lcd.width(), kDim);
    canvas.setTextFont(1);
    int x = 3;
    for (int i = 0; i < count; ++i) {
        canvas.setTextColor(kOk, kBg);
        canvas.drawString(keys[i], x, 122);
        x += canvas.textWidth(keys[i]) + 3;
        canvas.setTextColor(kDim, kBg);
        canvas.drawString(labels[i], x, 122);
        x += canvas.textWidth(labels[i]) + 7;
        if (x > lcd.width() - 20) break;
    }
}

void drawDeck()
{
    canvas.fillScreen(kBg);
    drawHeader();

    canvas.setTextFont(1);
    canvas.setTextColor(ws.open() ? kOk : kWarn, kBg);
    canvas.drawString(ws.open() ? statusText.c_str() : (lastError.isEmpty() ? statusText.c_str() : lastError.c_str()), 3, 15);
    canvas.setTextColor(kDim, kBg);
    canvas.drawRightString(String(static_cast<int>(sessions.size())) + " chats", lcd.width() - 3, 15);

    if (sessions.empty()) {
        canvas.setFont(&lgfx::fonts::efontCN_12);
        canvas.setTextColor(kText, kBg);
        canvas.drawString(ws.open() ? "No sessions. FnN new" : "Waiting for Desktop...", 3, 36);
        canvas.setTextFont(1);
    }

    canvas.setFont(&lgfx::fonts::efontCN_12);
    for (int r = 0; r < kDeckRows; ++r) {
        const int idx = deckTop + r;
        if (idx >= static_cast<int>(sessions.size())) break;
        const int  y   = 27 + r * 23;
        const bool sel = idx == deckSel;
        if (sel) canvas.fillRect(0, y - 2, lcd.width(), 23, kBar);
        const SessionItem& s = sessions[idx];

        canvas.setTextColor(sel ? kAccent : kText, sel ? kBar : kBg);
        canvas.drawString(truncateUtf8(s.title, 22), 4, y);

        canvas.setTextFont(1);
        canvas.setTextColor(sel ? kText : kDim, sel ? kBar : kBg);
        String meta = s.shortId;
        if (s.running) meta += " *";
        meta += "  ";
        meta += truncateUtf8(s.preview, 30);
        canvas.drawString(meta, 4, y + 11);
        canvas.setFont(&lgfx::fonts::efontCN_12);
    }
    canvas.setTextFont(1);

    static const char* const keys[]   = {"FnN", "ENT", ";.", "FnC", "esc"};
    static const char* const labels[] = {"new", "open", "move", "cfg", "back"};
    drawHints(keys, labels, 5);
}

void drawChat()
{
    canvas.fillScreen(kBg);
    drawHeader();

    canvas.setTextFont(1);
    canvas.setTextColor(ws.open() ? (running ? kAccent : kOk) : kWarn, kBg);
    canvas.drawString(ws.open() ? statusText.c_str() : "OFFLINE", 3, 15);

    // Live input level while dictating: answers "is the microphone hearing me?"
    // without needing the serial log.
    if (voice != Voice::kIdle) {
        canvas.setTextColor(kAccent, kBg);
        canvas.drawString("MIC", 66, 15);
        canvas.drawRect(88, 16, 86, 8, kDim);
        const int width = static_cast<int>(voiceLevel) * 82 / 100;
        if (width > 0) canvas.fillRect(90, 18, width, 4, voiceLevel > 85 ? kWarn : kOk);
        // Hardware I2S rate from the pure-read self-test — the decisive number.
        // ~16k means the bit clock is fine and any shortfall in the capture loop
        // is starvation, not a clock fault.
        const bool hwOk = voiceHwHz >= 15000 && voiceHwHz <= 17500;
        canvas.setTextColor(hwOk ? kOk : kWarn, kBg);
        canvas.drawRightString("H" + String(voiceHwHz / 1000) + "k",
                               lcd.width() - 34, 15);
    }
    canvas.setTextColor(kDim, kBg);
    String label = sessionId;
    for (const auto& s : sessions) {
        if (s.id == sessionId) { label = s.shortId; break; }
    }
    canvas.drawRightString(label, lcd.width() - 3, 15);

    canvas.setFont(&lgfx::fonts::efontCN_12);
    canvas.setTextColor(kText, kBg);
    const int lineH   = 12;
    const int visible = 6;
    const int total   = totalLines();
    int first = total - visible - scroll;
    if (first < 0) first = 0;
    int y = 26;
    // Rendering with a nearly exhausted heap is how a key press turned into a
    // reboot; say so instead.
    if (ESP.getFreeHeap() < 9000) {
        canvas.setTextColor(kWarn, kBg);
        canvas.drawString("LOW MEM", 3, 30);
    } else {
        for (int i = first; i < total && i < first + visible; ++i) {
            const String* line = lineAt(i);
            if (line != nullptr) canvas.drawString(*line, 3, y);
            y += lineH;
        }
    }
    if (total == 0) {
        canvas.setTextColor(kDim, kBg);
        canvas.drawString(ws.open() ? "No messages here yet." : "Reconnecting...", 3, 30);
    }
    canvas.setTextFont(1);

    canvas.drawFastHLine(0, 101, lcd.width(), kDim);
    canvas.fillRect(0, 103, lcd.width(), 14, kBg);
    canvas.setFont(&lgfx::fonts::efontCN_12);
    canvas.setTextColor(kAccent, kBg);
    String shown = input.isEmpty() ? String("> ") : ("> " + input);
    // Keep the "> " prefix while dropping characters off the left of the text.
    while (canvas.textWidth(shown) > lcd.width() - 6 && shown.length() > 3) shown.remove(2, 1);
    canvas.drawString(shown, 3, 103);
    canvas.setTextFont(1);

    if (scroll > 0) {
        canvas.setTextColor(kAccent, kBg);
        canvas.drawRightString("^", lcd.width() - 4, 16);
    }

    static const char* const keys[]   = {"ENT", "FnV", "Fn;.", "FnS", "esc"};
    static const char* const labels[] = {"send", "voice", "scroll", "list", "back"};
    drawHints(keys, labels, 5);
}

void drawConfig()
{
    canvas.fillScreen(kBg);
    drawHeader();

    canvas.setTextFont(1);
    canvas.setTextColor(kText, kBg);
    canvas.drawString("HARNESS LINK", 3, 16);

    const String values[3] = {configField == 0 ? configEdit : host,
                              configField == 1 ? configEdit : token,
                              configField == 2 ? configEdit : String(port)};
    const char*  names[3]  = {"HOST ", "TOKEN", "PORT "};
    for (int i = 0; i < 3; ++i) {
        canvas.setTextColor(configField == i ? kAccent : kDim, kBg);
        canvas.drawString(names[i], 3, 32 + i * 14);
        canvas.setTextColor(kText, kBg);
        canvas.drawString(truncateUtf8(values[i], 26), 44, 32 + i * 14);
    }

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("ENT save  Fn;. field  esc back", 3, 86);
    canvas.drawString(String("heap ") + String(ESP.getFreeHeap() / 1024) + "K", 3, 100);
}

void render()
{
    switch (screen) {
        case Screen::kDeck: drawDeck(); break;
        case Screen::kChat: drawChat(); break;
        case Screen::kConfig: drawConfig(); break;
    }
    canvas.pushSprite(&lcd, 0, 0);
    dirty = false;
}

// ---------------------------------------------------------------------------
// wifi / link bring-up

void linkService()
{
    if (!wifiStarted) {
        wifistore::begin();
        const auto* network = wifistore::last();
        if (network == nullptr) {
            statusText = "NO WIFI CFG";
            wifiStarted = true;  // nothing more to try
            return;
        }
        WiFi.mode(WIFI_STA);
        WiFi.persistent(false);
        WiFi.setAutoReconnect(true);
        WiFi.begin(network->ssid, network->pass);
        // Modem sleep delays ARP replies and incoming frames by seconds, which
        // shows up as "connected but unreachable" and as stalled pushes.  This
        // device exists to receive pushed text, so keep the radio awake.
        WiFi.setSleep(false);
        wifiStarted = true;
        statusText = "WIFI...";
        markDirty();
        return;
    }

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected != wifiOk) {
        wifiOk = connected;
        usbLogf("[wifi] %s ip=%s", connected ? "up" : "down",
                connected ? WiFi.localIP().toString().c_str() : "-");
        markDirty();
    }
    if (!connected) {
        if (statusText != "NO WIFI") statusText = "NO WIFI";
        return;
    }
    // Give the station a moment to settle after DHCP before the first TCP
    // connect: a connect issued in that window failed with the 400 ms timeout.
    static uint32_t wifiUpAt = 0;
    if (!wsStarted) {
        if (wifiUpAt == 0) wifiUpAt = millis();
        if (millis() - wifiUpAt < 1500) return;
        statusText = "LINK...";
        usbLogf("[ws] connect %s:%u", host.c_str(), static_cast<unsigned>(port));
        ws.begin(host, port, "/cardputer", token);
        wsStarted = true;
        markDirty();
        return;
    }
    {
        // Repaint on connect/disconnect so the header WS indicator is truthful.
        static harnessws::State lastState = harnessws::State::kDown;
        if (ws.state() != lastState) {
            lastState = ws.state();
            usbLogf("[ws] state=%d drop=%s", static_cast<int>(lastState), ws.lastDrop());
            markDirty();
        }
        // While down, the state never changes; report the retry reason so a
        // mismatched host/port is visible over USB instead of guesswork.
        static uint32_t lastDownLog = 0;
        if (ws.state() == harnessws::State::kDown && millis() - lastDownLog > 5000) {
            lastDownLog = millis();
            usbLogf("[ws] down drop=%s", ws.lastDrop());
        }
        // Associated but unreachable (the AP dropped our entry, or the radio is
        // asleep): sit in "connected" forever and nothing recovers, so force a
        // reassociation now and then.
        static uint32_t downSince     = 0;
        static uint32_t lastRecovery  = 0;
        if (!ws.open()) {
            if (downSince == 0) downSince = millis();
            if (millis() - downSince > 20000 && millis() - lastRecovery > 20000) {
                lastRecovery = millis();
                usbLogf("[ws] unreachable 20s, reassociating");
                WiFi.reconnect();
                ws.restart();
            }
        } else {
            downSince = 0;
        }
    }
    if (ws.state() == harnessws::State::kDown) {
        if (statusText != "RECONNECT" && !ws.open()) {
            statusText = "RECONNECT";
            markDirty();
        }
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
    kbdClearPresses();

    lcd.setRotation(rfcfg::screenRotation);
    lcd.fillScreen(kBg);

    // One full-screen backbuffer.  8 bpp = 32.4 KiB instead of 64.8 KiB: the
    // UI uses eight flat colours, and halving the largest allocation is what
    // gives the JSON parses room to breathe (a session load used to panic).
    canvas.setPsram(false);
    canvas.setColorDepth(8);
    canvasReady = canvas.createSprite(lcd.width(), lcd.height()) != nullptr;
    if (!canvasReady) {
        lcd.setTextColor(kWarn, kBg);
        lcd.setTextFont(1);
        lcd.drawString("canvas alloc failed", 4, 40);
        return;
    }
    canvas.setTextWrap(false);
    canvas.setTextFont(1);
    canvas.setTextSize(1);

    loadPrefs();
    {
        Preferences p;
        if (p.begin("harness", true)) { voiceHwHz = p.getUInt("hwHz", 0); p.end(); }
    }
    ws.setMessageHandler(handleWsMessage, nullptr);

    screen     = Screen::kDeck;
    statusText = "STARTING";
    markDirty();
    render();
}

void loop()
{
    static bool firstLoop = true;
    if (firstLoop) {
        firstLoop = false;
        // reset=4 is ESP_RST_PANIC (exception/abort), 6/7 are task/int watchdog.
        usbLogf("harness ready heap=%u min=%u canvas=%d wifiCfg=%d reset=%d", ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), canvasReady ? 1 : 0, wifistore::last() != nullptr ? 1 : 0,
                static_cast<int>(esp_reset_reason()));
    }

    // One line every 5 s: enough to see the heap trend before a crash, cheap
    // enough to leave on.  reset= is repeated so a log captured after the boot
    // still says why the previous run ended (4 = panic, 6/7 = watchdog).
    static uint32_t lastMemLog = 0;
    if (millis() - lastMemLog > 5000) {
        lastMemLog = millis();
        usbLogf("[mem] free=%u min=%u lines=%d bad=%u reset=%d hw=%u", ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), totalLines(), static_cast<unsigned>(parseFailures),
                static_cast<int>(esp_reset_reason()), static_cast<unsigned>(voiceHwHz));
    }

    M5Cardputer.update();

    Point2D_t point;
    while (kbdPopPress(point)) handlePress(point);

    linkService();
    ws.loop();

    // One-shot mic self-test as soon as the link is up: brings the codec up and
    // measures the true hardware I2S rate with pure reads, logging [voice]
    // SELFTEST. No key press and no speech needed to capture the evidence.
    static bool selfTestDone = false;
    if (!selfTestDone && ws.open()) {
        selfTestDone = true;
        if (!g_micUp) g_micUp = voiceI2sBegin();
        // One instrumented capture (silence is fine) to log per-stage timing:
        // read vs WS send vs render vs keyboard, so the starvation culprit is
        // measured, not guessed. No speech needed.
        if (g_micUp) voiceCapture(2500, false);
    }

    voiceService();

    if (dirty) render();
    delay(2);
}

}  // namespace harnessapp
