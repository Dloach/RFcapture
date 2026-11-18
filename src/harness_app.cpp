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
#include <ArduinoOTA.h>
#include <M5Cardputer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/i2s.h>
#include <esp_system.h>
#include <mbedtls/base64.h>

#include <algorithm>
#include <vector>

#include "harness_ws.h"
#include "flash_ui.h"
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

enum class Screen : uint8_t { kDeck, kChat, kPreview, kEdit, kConfig, kApproval, kNew, kOta };
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
// Last few characters of the model's reasoning stream, shown next to RUN.
String thinkingText;

// Messages are kept as (role, text): the chat list shows one truncated line per
// message and the full-screen view wraps on demand, so no wrapped copies are
// retained (keeping both was what pushed the heap into an abort earlier).
struct ChatMsg {
    String role;  // "user" | "assistant"
    String text;
};
std::vector<ChatMsg> msgs;
int      cursor       = 0;   // message index, or msgs.size() == the input row
int      caret        = 0;   // byte offset into `input`
int      previewIndex = 0;
int      previewScroll = 0;      // first visible wrapped line in the preview
int      previewMaxScroll = 0;   // clamped by drawPreview, used by the keys
bool     caretOn      = true;
uint32_t caretAt      = 0;
uint16_t parseFailures = 0;

// One DSH tool-approval prompt.  The turn is blocked until it is answered, so
// it takes over whatever screen is showing and returns there afterwards.
// ENT must be held for a second: a plain tap is ignored so that typing a
// prompt when the prompt appears cannot silently grant a tool.
String   approvalId;
String   approvalTool;
String   approvalReason;
Screen   approvalReturn = Screen::kChat;
uint32_t approvalAt     = 0;   // millis() when the prompt arrived
uint32_t approvalHoldAt = 0;   // 0 = ENT not held
bool     approvalArmed  = false;  // ENT was released since the prompt appeared
bool     approvalHoldFull = false;  // hold is Fn+ENT (full access), not plain ENT

// New-session chooser: 0 = workspace-write (asks before wider steps),
// 1 = full access (no further approval prompts for that Session).
int    newControl = 0;
Screen newReturn  = Screen::kDeck;

bool   running = false;
String input;
String lastError;
int    configField = 0;
String configEdit;

bool wifiStarted = false;
bool wsStarted   = false;
bool wifiOk      = false;
bool canvasReady = false;

// OTA runs inside HARNESS (started once WiFi is up) so a flash does not need
// leaving the app: the upload replaces the image and the reboot comes back to
// HARNESS.  While otaUploading every key is ignored.
bool          otaStarted  = false;
volatile bool otaUploading = false;
volatile bool otaFailed   = false;
volatile int  otaPercent  = 0;
char          otaStatus[32] = "";
Screen        otaReturn   = Screen::kChat;

bool     archiveArmed = false;
uint32_t archiveAt    = 0;

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

/// Last `chars` UTF-8 characters, with control bytes folded to spaces so the
/// one-line header cannot grow a newline.
String tailUtf8(const String& text, int chars)
{
    int i = static_cast<int>(text.length());
    while (i > 0 && chars > 0) {
        int j = i - 1;
        while (j > 0 && (static_cast<unsigned char>(text[j]) & 0xC0) == 0x80) --j;
        i = j;
        --chars;
    }
    String out = text.substring(i);
    String clean;
    clean.reserve(out.length());
    for (size_t k = 0; k < out.length(); ++k) {
        const char ch = out[k];
        clean += (static_cast<unsigned char>(ch) < 0x20) ? ' ' : ch;
    }
    return clean;
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

// --- chat model: one row per message plus the input row at the bottom --------
constexpr int kChatRows    = 5;    // message rows; row 6 is the streaming tail
constexpr int kMaxMsgs     = 24;
constexpr int kMaxMsgChars = 700;  // per-message cap; a full preview is ~350 chars

void chatTrim()
{
    while (msgs.size() > static_cast<size_t>(kMaxMsgs)) msgs.erase(msgs.begin());
    if (cursor > static_cast<int>(msgs.size())) cursor = static_cast<int>(msgs.size());
    if (previewIndex >= static_cast<int>(msgs.size())) {
        previewIndex = std::max(0, static_cast<int>(msgs.size()) - 1);
    }
}

void chatAddMessage(const String& role, const String& text)
{
    ChatMsg m;
    m.role = role;
    m.text = truncateUtf8(text, kMaxMsgChars);
    msgs.push_back(m);
    chatTrim();
    cursor = static_cast<int>(msgs.size());  // leave the cursor on the input row
    markDirty();
}

void audioTone(float hz, uint32_t ms);
void audioSilence(uint32_t ms);

/// Short descending two-note cue when the assistant commits a reply.  Kept
/// deliberately unlike the rising OTA chime.  The I2S path is full-duplex, so
/// this can play while the microphone stays initialized.
void assistantChime()
{
    if (voice != Voice::kIdle) return;
    static uint32_t lastAt = 0;
    const uint32_t now = millis();
    if (lastAt != 0 && now - lastAt < 1200) return;
    lastAt = now;
    audioTone(1568, 80);   // G6
    audioSilence(20);
    audioTone(1047, 160);  // C6
    audioSilence(30);
}

void chatClear()
{
    msgs.clear();
    streamText  = "";
    cursor      = 0;
    caret       = 0;
    previewIndex = 0;
}

/// Move the selection through the message rows and finally onto the input row.
void moveCursor(int delta)
{
    const int last = static_cast<int>(msgs.size());  // the input row
    cursor += delta;
    if (cursor < 0) cursor = 0;
    if (cursor > last) cursor = last;
    markDirty();
}

void caretLeft()
{
    if (caret <= 0) return;
    int i = caret - 1;
    while (i > 0 && (static_cast<unsigned char>(input[i]) & 0xC0) == 0x80) --i;
    caret = i;
    caretOn = true;
    markDirty();
}

void caretRight()
{
    if (caret >= static_cast<int>(input.length())) return;
    int step = utf8Len(static_cast<unsigned char>(input[caret]));
    caret += step;
    if (caret > static_cast<int>(input.length())) caret = input.length();
    caretOn = true;
    markDirty();
}

void inputInsert(const String& text)
{
    if (input.length() + text.length() > static_cast<size_t>(kMaxInput)) return;
    input = input.substring(0, caret) + text + input.substring(caret);
    caret += text.length();
    caretOn = true;
    markDirty();
}

void inputBackspace()
{
    if (caret <= 0) return;
    int i = caret - 1;
    while (i > 0 && (static_cast<unsigned char>(input[i]) & 0xC0) == 0x80) --i;
    input.remove(i, caret - i);
    caret = i;
    caretOn = true;
    markDirty();
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
    chatClear();
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
        caret = 0;
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

/// Leave the approval overlay and tell the Host which way it went. `defer`
/// hands the request back to the Desktop answerer.
void answerApproval(const char* decision)
{
    if (screen != Screen::kApproval) return;
    String frame = "{\"type\":\"approval.answer\",\"id\":\"";
    frame += jsonEscape(approvalId);
    frame += "\",\"decision\":\"";
    frame += decision;
    frame += "\"}";
    ws.sendText(frame);
    usbLogf("[approval] %s id=%s", decision, approvalId.c_str());
    screen         = approvalReturn;
    approvalId     = "";
    approvalHoldAt = 0;
    approvalArmed  = false;
    approvalHoldFull = false;
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
    chatClear();
    for (JsonObject item : messages) {
        const String role = item["role"].as<String>();
        const String text = item["text"].as<String>();
        if (text.isEmpty()) continue;
        ChatMsg m;
        m.role = role == "user" ? "user" : "assistant";
        m.text = truncateUtf8(text, kMaxMsgChars);
        msgs.push_back(m);
    }
    chatTrim();
    cursor = static_cast<int>(msgs.size());
    statusText = "READY";
    markDirty();
}

void handleMessage(const char* json, size_t length)
{
    // Parsing a frame with almost no heap left is how loading a session turned
    // into a panic (reset=4).  Refuse it and say so instead of dying.  The floor
    // scales with the frame: a 55-byte hello or a 2 KiB session list needs only a
    // few KiB, and blocking those at a flat 18 KiB floor meant the deck never
    // populated even though the link was up.  Bulk snapshots still need headroom.
    const uint32_t need = static_cast<uint32_t>(length) * 2 + 3000;
    if (ESP.getFreeHeap() < need) {
        usbLogf("[rx] skip len=%u need=%u heap=%u", static_cast<unsigned>(length),
                static_cast<unsigned>(need), ESP.getFreeHeap());
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
        if (text[0] != '\0' && streamText.length() < 4000) {
            streamText += text;
            markDirty();
        }
    } else if (strcmp(type, "message") == 0) {
        const char* role = doc["role"] | "";
        const char* text = doc["text"] | "";
        if (text[0] != '\0') {
            const bool assistant = strcmp(role, "assistant") == 0;
            if (assistant) streamText = "";
            chatAddMessage(assistant ? "assistant" : "user", String(text));
            if (assistant) assistantChime();
        }
    } else if (strcmp(type, "status") == 0) {
        running = doc["running"] | false;
        if (!running) thinkingText = "";
        markDirty();
    } else if (strcmp(type, "thinking") == 0) {
        const char* text = doc["text"] | "";
        if (text[0] != '\0') {
            thinkingText += text;
            const int cap = 64;
            if (thinkingText.length() > cap) {
                thinkingText.remove(0, thinkingText.length() - cap);
            }
            markDirty();
        }
    } else if (strcmp(type, "accepted") == 0) {
        statusText = "ACCEPTED";
        markDirty();
    } else if (strcmp(type, "cancelled") == 0) {
        statusText = "CANCELLED";
        markDirty();
    } else if (strcmp(type, "archived") == 0) {
        const String id = doc["sessionId"] | "";
        for (size_t i = 0; i < sessions.size(); ++i) {
            if (sessions[i].id == id) {
                sessions.erase(sessions.begin() + static_cast<int>(i));
                break;
            }
        }
        if (deckSel >= static_cast<int>(sessions.size())) {
            deckSel = std::max(0, static_cast<int>(sessions.size()) - 1);
        }
        if (deckTop > deckSel) deckTop = deckSel;
        statusText = "ARCHIVED";
        markDirty();
        requestSessions();
    } else if (strcmp(type, "stt") == 0) {
        const char* text = doc["text"] | "";
        if (text[0] != '\0') inputInsert(String(text));  // insert at the caret
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
    } else if (strcmp(type, "approval") == 0) {
        const char* id = doc["id"] | "";
        if (id[0] != '\0' && screen != Screen::kApproval) {
            approvalId     = id;
            approvalTool   = doc["tool"] | "tool";
            approvalReason = doc["reason"] | "";
            approvalReturn = screen;
            approvalAt     = millis();
            approvalHoldAt = 0;
            approvalArmed  = false;
            approvalHoldFull = false;
            screen         = Screen::kApproval;
            markDirty();
        }
    } else if (strcmp(type, "approval.closed") == 0) {
        const char* id = doc["id"] | "";
        if (screen == Screen::kApproval && approvalId == id) {
            screen         = approvalReturn;
            approvalId     = "";
            approvalHoldAt = 0;
            approvalArmed  = false;
            approvalHoldFull = false;
            markDirty();
        }
    }

    // The two frames that carry bulk text: log the heap after they are applied,
    // so the next crash is measurable rather than a guess.
    if (strcmp(type, "snapshot") == 0 || strcmp(type, "sessions") == 0) {
        usbLogf("[rx] %s done free=%u msgs=%d", type, ESP.getFreeHeap(), static_cast<int>(msgs.size()));
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
    // The full-duplex driver is never torn down; a failed capture only resets
    // state, so the next Fn+V can read the same live channel again.
    if (voice != Voice::kIdle) ws.sendText("{\"type\":\"stt.abort\"}");
    voice      = Voice::kIdle;
    statusText = "VOICE FAILED";
    lastError  = why;
    usbLogf("[voice] abort: %s", why);
    markDirty();
}

// --- capture: full-duplex I2S read ------------------------------------------
// audioInit() owns the ES8311 codec and I2S0 in full duplex (BCLK 41, WS 43,
// DOUT 42, DIN 46).  A capture only reads interleaved stereo frames from the
// already-running driver and keeps the left (microphone) slot.
constexpr size_t   kVoiceSamples = 1920;   // 120 ms at 16 kHz per read/send
constexpr uint32_t kVoiceRate    = 16000;
constexpr uint32_t kVoiceMaxMs   = 20000;  // hard cap; ENT/Fn+V stops earlier

// Allocated only while dictating (voiceStart) and freed afterwards: keeping
// them as static BSS held ~18 KiB even at idle, which starved lwIP below the
// point where it could open a socket or send a frame ("no session").
constexpr size_t kVoiceB64Size = 6000;
int16_t* voiceReadBuf = nullptr;
int16_t* voiceStereoBuf = nullptr;  // interleaved L/R scratch for the RX read
uint8_t* voiceB64     = nullptr;
uint8_t  voiceLevel  = 0;  // 0..100 for the on-screen meter
uint32_t voiceMeasuredHz = 0;
uint32_t voiceHwHz = 0;    // declared rate; audioInit() sets it to kVoiceRate
bool     g_micUp = false;  // full-duplex audio brought up
bool     g_audioReady = false;
i2s_port_t g_voicePort = I2S_NUM_0;

// --- full-duplex ES8311 + I2S ----------------------------------------------
// The speaker and microphone share one ES8311 codec and one I2S clock.  The
// M5Unified Speaker/Mic helpers each install a single-direction driver and
// uninstall the other, so this path drives the codec and a full-duplex I2S0
// directly: both directions stay live, and a prompt tone no longer costs the
// microphone.  Pinout and the REG01=0xBF clock union follow the proven
// Cardputer ADV implementation.
constexpr uint8_t kCodecAddr = 0x18;
constexpr int     kAudioBclk = 41;
constexpr int     kAudioWs   = 43;
constexpr int     kAudioDout = 42;  // speaker data
constexpr int     kAudioDin  = 46;  // microphone data

bool audioWriteReg(uint8_t reg, uint8_t value)
{
    return M5.In_I2C.writeRegister8(kCodecAddr, reg, value, 100000);
}

bool audioInit()
{
    if (g_audioReady) return true;

    // M5Unified's mic value (0xBA) and speaker value (0xB5) differ only in the
    // independent ADC/DAC clock gates; 0xBF keeps both chains clocked.
    bool ok = true;
    ok &= audioWriteReg(0x00, 0x80);  // reset / power on
    ok &= audioWriteReg(0x01, 0xBF);  // MCLK=BCLK, ADC+DAC clocks
    ok &= audioWriteReg(0x02, 0x18);
    ok &= audioWriteReg(0x0D, 0x01);
    ok &= audioWriteReg(0x0E, 0x02);  // PGA + ADC modulator
    ok &= audioWriteReg(0x14, 0x16);  // mic PGA ~ +18 dB
    ok &= audioWriteReg(0x16, 0x02);  // ADC scale +12 dB
    ok &= audioWriteReg(0x17, 0xBF);  // ADC digital 0 dB
    ok &= audioWriteReg(0x1C, 0x6A);  // HPF
    ok &= audioWriteReg(0x12, 0x00);  // DAC power up
    ok &= audioWriteReg(0x13, 0x10);  // HP drive
    ok &= audioWriteReg(0x32, 0xB8);  // DAC volume
    ok &= audioWriteReg(0x37, 0x08);  // DAC EQ bypass
    delay(20);
    if (!ok) usbLogf("[audio] codec write failed");

    i2s_config_t cfg = {};
    cfg.mode                 = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    cfg.sample_rate          = kVoiceRate;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    cfg.fixed_mclk           = 0;

    if (i2s_driver_install(g_voicePort, &cfg, 0, nullptr) != ESP_OK) {
        usbLogf("[audio] i2s install failed");
        return false;
    }
    i2s_pin_config_t pins = {};
    pins.bck_io_num   = kAudioBclk;
    pins.ws_io_num    = kAudioWs;
    pins.data_out_num = kAudioDout;
    pins.data_in_num  = kAudioDin;
    if (i2s_set_pin(g_voicePort, &pins) != ESP_OK) {
        usbLogf("[audio] i2s pin failed");
        return false;
    }
    i2s_zero_dma_buffer(g_voicePort);
    i2s_start(g_voicePort);
    g_audioReady = true;
    voiceHwHz    = kVoiceRate;
    usbLogf("[audio] full duplex ready");
    return true;
}

/// One square-wave note on the speaker, written straight into the shared I2S
/// TX stream.  Paces itself on the DMA so the caller needs no delay.
void audioTone(float hz, uint32_t ms)
{
    if (!audioInit()) return;
    static int16_t frames[320 * 2];
    const uint32_t step  = static_cast<uint32_t>(hz * 65536.0f / kVoiceRate);
    uint32_t       total = kVoiceRate * ms / 1000;
    uint32_t       phase = 0;
    while (total > 0) {
        const size_t n = total < 320 ? total : 320;
        for (size_t i = 0; i < n; ++i) {
            phase += step;
            const int16_t v = (phase & 0x8000) ? 7000 : -7000;
            frames[i * 2]     = v;
            frames[i * 2 + 1] = v;
        }
        size_t written = 0;
        i2s_write(g_voicePort, frames, n * 2 * sizeof(int16_t), &written,
                  100 / portTICK_PERIOD_MS);
        total -= n;
    }
}

void audioSilence(uint32_t ms)
{
    if (!audioInit()) return;
    static int16_t frames[320 * 2] = {0};
    uint32_t total = kVoiceRate * ms / 1000;
    while (total > 0) {
        const size_t n = total < 320 ? total : 320;
        size_t written = 0;
        i2s_write(g_voicePort, frames, n * 2 * sizeof(int16_t), &written,
                  100 / portTICK_PERIOD_MS);
        total -= n;
    }
}

size_t voiceSendPcm(const int16_t* pcm, size_t samples)
{
    if (voiceB64 == nullptr) return 0;
    size_t encodedLen = 0;
    if (mbedtls_base64_encode(voiceB64, kVoiceB64Size, &encodedLen,
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

bool voiceI2sBegin()
{
    const bool ok = audioInit();
    if (ok) g_micUp = true;
    return ok;
}

size_t voiceRead(int16_t* buffer, size_t samples, uint32_t timeoutMs)
{
    if (voiceStereoBuf == nullptr || samples > kVoiceSamples) return 0;
    size_t bytesRead = 0;
    const esp_err_t err = i2s_read(g_voicePort, voiceStereoBuf,
                                   samples * 2 * sizeof(int16_t), &bytesRead,
                                   timeoutMs / portTICK_PERIOD_MS);
    if (err != ESP_OK) return 0;
    const size_t frames = bytesRead / (2 * sizeof(int16_t));
    for (size_t i = 0; i < frames; ++i) buffer[i] = voiceStereoBuf[i * 2];
    return frames;
}

/// Unity software gain + soft-limit, tracking the peak for the log. The analog
/// PGA (+21 dB) and ADC (+6 dB) now amplify BEFORE the converter, so the signal
/// already arrives at a healthy level; multiplying here again would only stack
/// gain and drive the soft-limiter into distortion. Keep the limiter as a safety
/// net for loud speech.
void voiceCondition(int16_t* buffer, size_t count, int32_t& peak)
{
    for (size_t i = 0; i < count; ++i) {
        int32_t value   = static_cast<int32_t>(buffer[i]);
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

    // The full-duplex driver stays installed and started; a capture only reads
    // from it, so there is no per-capture begin/end cycle to destabilize.
    // M5Unified's capture task is not used at all here.

    // Declare the rate the hardware actually produces, not the nominal 16000:
    // the capture loop measures ~14.8 kHz, and telling Vosk 16 kHz makes speech
    // play ~8% fast, which costs recognition accuracy.
    const uint32_t declaredRate =
        (voiceHwHz >= 8000 && voiceHwHz <= 48000) ? voiceHwHz : kVoiceRate;
    ws.sendText(String("{\"type\":\"stt.begin\",\"rate\":") + String(declaredRate) +
                ",\"channels\":1,\"bits\":16}");

    voiceLevel = 0;
    int     sent   = 0;
    int32_t peak   = 0;
    uint32_t totalSamples = 0;
    uint32_t lastData     = millis();
    const uint32_t start = millis();
    while (millis() - start < maxMs) {
        const uint32_t tRead0 = millis();
        // Short wait per read: a 500 ms read could sit in the driver past the
        // no-data deadline and freeze the whole loop.  20 ms lets the guard
        // below run and turn a dead channel into VOICE FAILED, not offline.
        const size_t got = voiceRead(voiceReadBuf, kVoiceSamples, 20);
        const uint32_t tRead1 = millis();
        if (got == 0) {
            // If the channel stays dry for two seconds, abort instead of
            // spinning the full 20 s block: the UI and WS ping must keep going.
            if (millis() - lastData > 2000) {
                ws.sendText("{\"type\":\"stt.abort\"}");
                usbLogf("[voice] no data for 2s, abort");
                lastError = "no audio";
                return false;
            }
            continue;
        }
        lastData = millis();

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

    // Silence gate: SenseVoice happily invents a sentence for an empty clip
    // (a 1 s silent WAV transcribes to "没。"), so a capture whose loudest sample
    // never rises above room noise is dropped instead of transcribed.
    if (peak < 600) {
        ws.sendText("{\"type\":\"stt.abort\"}");
        usbLogf("[voice] silent (peak=%d), skipped", static_cast<int>(peak));
        lastError = "no speech";
        return false;
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
    if (!g_micUp) {
        // First dictation only: the codec needs ~1 s of bring-up.  Say so on
        // screen and wait for the user to see ENT stop before the capture
        // starts, instead of eating the first words of a sentence begun now.
        statusText = "MIC INIT";
        markDirty();
        render();
        if (!voiceI2sBegin()) {
            voice      = Voice::kIdle;
            statusText = "VOICE FAILED";
            lastError  = "mic";
            markDirty();
            return;
        }
        g_micUp = true;
    }
    statusText = "ENT stop";
    markDirty();
    render();  // the capture starts after this frame; speak from here

    if (voiceReadBuf == nullptr)
        voiceReadBuf = static_cast<int16_t*>(malloc(kVoiceSamples * sizeof(int16_t)));
    if (voiceStereoBuf == nullptr)
        voiceStereoBuf = static_cast<int16_t*>(malloc(kVoiceSamples * 2 * sizeof(int16_t)));
    if (voiceB64 == nullptr)
        voiceB64 = static_cast<uint8_t*>(malloc(kVoiceB64Size));
    if (voiceReadBuf == nullptr || voiceStereoBuf == nullptr || voiceB64 == nullptr) {
        free(voiceReadBuf);   voiceReadBuf   = nullptr;
        free(voiceStereoBuf); voiceStereoBuf = nullptr;
        free(voiceB64);       voiceB64       = nullptr;
        voice      = Voice::kIdle;
        statusText = "VOICE FAILED";
        lastError  = "no mem";
        markDirty();
        return;
    }

    const bool ok = voiceCapture(kVoiceMaxMs, true);
    free(voiceReadBuf);   voiceReadBuf   = nullptr;
    free(voiceStereoBuf); voiceStereoBuf = nullptr;
    free(voiceB64);       voiceB64       = nullptr;
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
    // Kept as a name for the deck's list paging; the chat now navigates a cursor.
    const int total = static_cast<int>(msgs.size());
    (void)total;
    moveCursor(delta);
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
        screen     = newReturn;
        markDirty();
        return;
    }
    statusText = "NEW...";
    String frame = "{\"type\":\"create\",\"control\":\"";
    frame += newControl == 1 ? "full" : "workspace";
    frame += "\"}";
    ws.sendText(frame);
    screen = newReturn;  // the host's `created` opens the new Session (kChat)
    markDirty();
}

/// Fn+N asks for the control boundary first; the Host applies the matching
/// permission preset when it creates the Session.
void openNewSession()
{
    newReturn  = screen;
    newControl = 0;
    screen     = Screen::kNew;
    markDirty();
}

/// DSH has no hard delete; archiving hides the Session from the deck.  The
/// deck asks for a second D so a stray keypress cannot hide a conversation.
void archiveSelected()
{
    if (sessions.empty()) return;
    if (deckSel < 0 || deckSel >= static_cast<int>(sessions.size())) return;
    String frame = "{\"type\":\"archive\",\"sessionId\":\"";
    frame += jsonEscape(sessions[deckSel].id);
    frame += "\"}";
    ws.sendText(frame);
    statusText   = "ARCHIVING...";
    archiveArmed = false;
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

    if (screen == Screen::kOta) {
        // Uploading: no key is accepted at all, including esc.  After an error
        // the overlay stays so the failure is readable, and esc returns.
        if (!otaUploading && c == '`') {
            screen = otaReturn;
            markDirty();
        }
        return;
    }

    if (screen == Screen::kApproval) {
        // ENT is deliberately not an edge action: the hold is polled in loop()
        // so an accidental tap cannot approve.  D rejects, esc defers.
        if (c == '`') { answerApproval("defer"); return; }
        if (c == KEY_BACKSPACE) { answerApproval("rejected"); return; }
        return;
    }

    if (screen == Screen::kNew) {
        if (c == '`') { screen = newReturn; markDirty(); return; }
        if (c == ';') { newControl = 0; markDirty(); return; }
        if (c == '.') { newControl = 1; markDirty(); return; }
        if (c == KEY_ENTER) { createSession(); return; }
        return;
    }

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
        // Both the DEL key and the literal D letter archive, because the deck
        // hint says "D" and the Cardputer labels its delete key differently.
        if (c == KEY_BACKSPACE || c == 'd' || c == 'D') {
            if (archiveArmed && millis() - archiveAt < 3000) {
                archiveSelected();
            } else {
                archiveArmed = true;
                archiveAt    = millis();
                statusText   = "ARCHIVE? D AGAIN";
                markDirty();
            }
            return;
        }
        archiveArmed = false;  // any other key cancels the two-step archive

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
            if (c == 'n' || c == 'N') { openNewSession(); return; }
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

    // Full-screen preview of one message: the whole screen is the content.
    // esc / ENT / space returns, up/down scroll the text.
    if (screen == Screen::kPreview) {
        if (c == '`' || c == KEY_ENTER || c == ' ') {
            screen = Screen::kChat;
            markDirty();
            return;
        }
        if (c == ';') {
            if (previewScroll > 0) --previewScroll;
            markDirty();
            return;
        }
        if (c == '.') {
            if (previewScroll < previewMaxScroll) ++previewScroll;
            markDirty();
            return;
        }
        if (c == ',') { previewScroll = 0; markDirty(); return; }              // top
        if (c == '/') { previewScroll = previewMaxScroll; markDirty(); return; }  // bottom
        return;
    }

    // Full-screen editor for the message being composed.
    if (screen == Screen::kEdit) {
        if (c == '`') {
            screen = Screen::kChat;
            markDirty();
            return;
        }
        if (c == KEY_ENTER) {
            screen = Screen::kChat;
            sendPrompt();
            return;
        }
        if (c == KEY_BACKSPACE) {
            inputBackspace();
            return;
        }
        if (fn && (c == 'v' || c == 'V')) {
            voiceStart();
            return;
        }
        if (fn && (c == ',' || c == '.')) {
            inputInsert(String(c));
            return;
        }
        if (c == ',') { caretLeft(); return; }
        if (c == '/') { caretRight(); return; }
        if (c >= 32 && c < 127) {
            inputInsert(String(c));
            return;
        }
        return;
    }

    // Chat: the cursor selects a message (ENTER opens it full screen) or sits on
    // the input row (typing edits it, ENTER sends). Left/right move the caret.
    if (c == '`') {
        screen = Screen::kDeck;
        requestSessions();
        markDirty();
        return;
    }
    if (fn) {
        if (c == 'v' || c == 'V') { voiceStart(); return; }
        if (c == KEY_ENTER) {  // full-screen editor
            screen = Screen::kEdit;
            markDirty();
            return;
        }
        if (c == 's' || c == 'S') {
            screen = Screen::kDeck;
            requestSessions();
            markDirty();
            return;
        }
        if (c == 'n' || c == 'N') { openNewSession(); return; }
        // Comma and period drive the caret/cursor bare, so Fn gives the literal
        // characters back to the composer (URLs, decimals, prose).
        if (c == ',' || c == '.') {
            if (cursor < static_cast<int>(msgs.size())) cursor = static_cast<int>(msgs.size());
            inputInsert(String(c));
            return;
        }
        if (c == 'x' || c == 'X') { sendCancel(); return; }
        return;
    }

    const bool onInput = cursor >= static_cast<int>(msgs.size());
    if (c == KEY_ENTER) {
        if (onInput) {
            sendPrompt();
        } else if (cursor >= 0 && cursor < static_cast<int>(msgs.size())) {
            previewIndex  = cursor;
            previewScroll = 0;
            screen        = Screen::kPreview;
            markDirty();
        }
        return;
    }
    if (c == KEY_BACKSPACE) {
        if (!onInput) cursor = static_cast<int>(msgs.size());
        inputBackspace();
        return;
    }
    if (c == ';') { moveCursor(-1); return; }  // up
    if (c == '.') { moveCursor(+1); return; }  // down
    if (c == ',') {
        if (onInput) caretLeft();
        return;
    }
    if (c == '/') {
        if (onInput) caretRight();
        return;
    }
    if (c >= 32 && c < 127) {
        if (!onInput) cursor = static_cast<int>(msgs.size());
        inputInsert(String(c));
        return;
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
        const String tick = tailUtf8(thinkingText, 8);
        if (!tick.isEmpty()) {
            // Eight-character thinking ticker: enough to guess what the turn is
            // doing without turning the header into a transcript.  efontCN_12
            // so Chinese reasoning renders too; the battery switches back.
            canvas.setFont(&lgfx::fonts::efontCN_12);
            canvas.setTextColor(kText, kHead);
            canvas.drawString(tick, 104, 0);
            canvas.setTextFont(1);
        }
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

    // Low-cost footer: pack the hints into as few pages as fit, then show one
    // page at a time.  A page advance repaints once every 3 s (the loop marks
    // dirty), instead of a 120 ms marquee that churned the heap continuously.
    int starts[8];
    int pages = 0;
    int i = 0;
    while (i < count && pages < 8) {
        starts[pages++] = i;
        int x = 3;
        while (i < count) {
            const int w = canvas.textWidth(keys[i]) + 3 + canvas.textWidth(labels[i]) + 12;
            if (x + w > lcd.width() - 4 && x > 3) break;
            x += w;
            ++i;
        }
    }
    if (pages == 0) return;

    const int page = static_cast<int>((millis() / 3000) % pages);
    int x = 3;
    for (int k = starts[page]; k < count; ++k) {
        const int kw = canvas.textWidth(keys[k]);
        const int lw = canvas.textWidth(labels[k]);
        const int w  = kw + 3 + lw + 12;
        if (x + w > lcd.width() - 4 && x > 3) break;
        canvas.setTextColor(kOk, kBg);
        canvas.drawString(keys[k], x, 122);
        x += kw + 3;
        canvas.setTextColor(kDim, kBg);
        canvas.drawString(labels[k], x, 122);
        x += lw + 12;
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

    static const char* const keys[]   = {"ENT", ";.", "FnN", "FnS", "FnC", "D", "esc"};
    static const char* const labels[] = {"open", "move", "new", "refresh", "cfg", "archive", "back"};
    drawHints(keys, labels, 7);
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
        const bool hwOk = voiceHwHz >= 15000 && voiceHwHz <= 17500;
        canvas.setTextColor(hwOk ? kOk : kWarn, kBg);
        canvas.drawRightString("H" + String(voiceHwHz / 1000) + "k", lcd.width() - 34, 15);
    }
    canvas.setTextColor(kDim, kBg);
    String label = sessionId;
    for (const auto& s : sessions) {
        if (s.id == sessionId) { label = s.shortId; break; }
    }
    canvas.drawRightString(label, lcd.width() - 3, 15);

    // One truncated row per message. The selection window follows the cursor.
    int top = cursor - kChatRows + 1;
    if (top > static_cast<int>(msgs.size()) - kChatRows) {
        top = static_cast<int>(msgs.size()) - kChatRows;
    }
    if (top < 0) top = 0;

    canvas.setFont(&lgfx::fonts::efontCN_12);
    const int rowH = 12;
    if (msgs.empty() && streamText.isEmpty()) {
        canvas.setTextColor(kDim, kBg);
        canvas.drawString(ws.open() ? "No messages here yet." : "Reconnecting...", 3, 28);
    }
    for (int r = 0; r < kChatRows; ++r) {
        const int idx = top + r;
        if (idx >= static_cast<int>(msgs.size())) break;
        const int  y   = 26 + r * rowH;
        const bool sel = idx == cursor;
        if (sel) canvas.fillRect(0, y - 1, lcd.width(), rowH, kBar);
        canvas.setTextColor(sel ? kAccent : kText, sel ? kBar : kBg);
        const String prefix = msgs[idx].role == "user" ? "You " : "AI  ";
        canvas.drawString(prefix + truncateUtf8(msgs[idx].text, 30), 3, y);
    }
    // Live streaming tail, one line, not selectable. Sits in the row directly
    // above the input separator.
    if (!streamText.isEmpty()) {
        canvas.setTextColor(kDim, kBg);
        canvas.drawString("AI  " + truncateUtf8(streamText, 30), 3, 26 + kChatRows * rowH);
    }
    canvas.setTextFont(1);

    // Input row: highlighted when the cursor is on it, with a blinking caret.
    const bool onInput = cursor >= static_cast<int>(msgs.size());
    const int  inputY  = 101;
    canvas.drawFastHLine(0, inputY - 3, lcd.width(), kDim);
    if (onInput) canvas.fillRect(0, inputY - 1, lcd.width(), 13, kBar);
    canvas.setFont(&lgfx::fonts::efontCN_12);
    // Keep the caret visible: drop leading characters until the prefix fits.
    String pre = input.substring(0, caret);
    while (canvas.textWidth("> " + pre) > lcd.width() - 16 && pre.length() > 0) {
        int step = utf8Len(static_cast<unsigned char>(pre[0]));
        if (step > static_cast<int>(pre.length())) step = pre.length();
        pre.remove(0, step);
    }
    const int caretX = 3 + canvas.textWidth("> " + pre);
    canvas.setTextColor(onInput ? kText : kDim, onInput ? kBar : kBg);
    canvas.drawString("> " + pre + input.substring(caret), 3, inputY);
    if (onInput && caretOn) canvas.drawFastVLine(caretX, inputY, 11, kAccent);
    canvas.setTextFont(1);

    static const char* const keys[]   = {"ENT", "FnENT", "FnV", "FnX", "FnN", "FnS", ";.", ",/", "esc"};
    static const char* const labels[] = {"open/send", "edit", "voice", "stop", "new", "deck", "move", "caret", "back"};
    drawHints(keys, labels, 9);
}

/// Full-screen reading view: no header, no footer, maximum content per screen.
void drawPreview()
{
    canvas.fillScreen(kBg);
    if (msgs.empty() || previewIndex < 0 || previewIndex >= static_cast<int>(msgs.size())) {
        canvas.setTextFont(1);
        canvas.setTextColor(kWarn, kBg);
        canvas.drawString("no message", 4, 60);
        return;
    }
    std::vector<String> lines;
    canvas.setFont(&lgfx::fonts::efontCN_12);
    wrapInto(lines, msgs[previewIndex].text, "", lcd.width() - 6);

    const int lineH = 12;
    const int topY  = 2;
    const int bottomY = lcd.height() - 12;
    int visible = (bottomY - topY) / lineH;
    if (visible < 1) visible = 1;
    int maxScroll = static_cast<int>(lines.size()) - visible;
    if (maxScroll < 0) maxScroll = 0;
    previewMaxScroll = maxScroll;
    if (previewScroll > maxScroll) previewScroll = maxScroll;
    if (previewScroll < 0) previewScroll = 0;

    canvas.setTextColor(kText, kBg);
    int y = topY;
    for (int i = previewScroll; i < static_cast<int>(lines.size()) && y < bottomY; ++i) {
        canvas.drawString(lines[i], 3, y);
        y += lineH;
    }

    canvas.setTextFont(1);
    canvas.setTextColor(kDim, kBg);
    if (maxScroll > 0) {
        const int page = previewScroll / visible + 1;
        const int pages = maxScroll / visible + 1;
        char buf[48];
        snprintf(buf, sizeof(buf), "%d/%d ;. scroll , top / bottom esc", page, pages);
        canvas.drawString(buf, 3, lcd.height() - 10);
    } else {
        canvas.drawString("ENT/esc back", 3, lcd.height() - 10);
    }
}

/// Full-screen composer: same content area as the preview, with the caret.
void drawEdit()
{
    canvas.fillScreen(kBg);
    std::vector<String> lines;
    canvas.setFont(&lgfx::fonts::efontCN_12);
    wrapInto(lines, input, "", lcd.width() - 6);
    canvas.setTextColor(kText, kBg);
    const int lineH = 12;
    int y = 2;
    for (size_t i = 0; i < lines.size() && y < lcd.height() - 14; ++i) {
        canvas.drawString(lines[i], 3, y);
        y += lineH;
    }
    // The caret belongs at the end of the wrapped prefix, which is exact for a
    // left-to-right wrapper.
    std::vector<String> preLines;
    wrapInto(preLines, input.substring(0, caret), "", lcd.width() - 6);
    const int caretLine = preLines.empty() ? 0 : static_cast<int>(preLines.size()) - 1;
    const int caretX = 3 + (preLines.empty() ? 0 : canvas.textWidth(preLines.back()));
    if (caretOn) canvas.drawFastVLine(caretX, 2 + caretLine * lineH, 11, kAccent);

    canvas.setTextFont(1);
    canvas.setTextColor(kDim, kBg);
    canvas.drawFastHLine(0, lcd.height() - 12, lcd.width(), kDim);
    canvas.setTextColor(kOk, kBg);
    canvas.drawString("ENT", 3, lcd.height() - 10);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("send", 22, lcd.height() - 10);
    canvas.setTextColor(kOk, kBg);
    canvas.drawString("esc", 58, lcd.height() - 10);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("back", 78, lcd.height() - 10);
    canvas.setTextColor(kOk, kBg);
    canvas.drawString("FnV", 108, lcd.height() - 10);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("voice", 128, lcd.height() - 10);
    canvas.setTextColor(kOk, kBg);
    canvas.drawString(",/", 158, lcd.height() - 10);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("caret", 174, lcd.height() - 10);
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

/// Full-screen DSH approval prompt.  The turn is blocked while this is up, so
/// it replaces the header/footer of whatever screen the user was on.
void drawApproval()
{
    canvas.fillScreen(kBg);
    canvas.fillRect(0, 0, lcd.width(), 13, kHead);
    canvas.setTextFont(1);
    canvas.setTextColor(kText, kHead);
    canvas.drawString("APPROVAL", 3, 2);

    const int left = 45 - static_cast<int>((millis() - approvalAt) / 1000);
    char timer[16];
    snprintf(timer, sizeof(timer), "%ds", left > 0 ? left : 0);
    canvas.setTextColor(left > 10 ? kDim : kWarn, kHead);
    canvas.drawRightString(timer, lcd.width() - 3, 2);

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("TOOL", 4, 17);
    canvas.setFont(&lgfx::fonts::efontCN_12);
    canvas.setTextColor(kAccent, kBg);
    canvas.drawString(truncateUtf8(approvalTool, 15), 40, 15);

    std::vector<String> lines;
    wrapInto(lines, approvalReason.isEmpty() ? String("no reason given") : approvalReason,
             "", lcd.width() - 8);
    canvas.setTextColor(kText, kBg);
    int y = 30;
    for (size_t i = 0; i < lines.size() && i < 4; ++i) {
        canvas.drawString(lines[i], 4, y);
        y += 12;
    }
    canvas.setTextFont(1);

    // Hold-to-allow: the bar fills for ~1.2 s while ENT is down.  A tap does
    // nothing, so a keystroke already in flight cannot approve a tool.
    const int barY = 86;
    canvas.drawRect(4, barY, lcd.width() - 8, 8, kDim);
    if (approvalHoldAt != 0) {
        int w = static_cast<int>(millis() - approvalHoldAt) * (lcd.width() - 12) / 1200;
        if (w > lcd.width() - 12) w = lcd.width() - 12;
        if (w > 0) canvas.fillRect(6, barY + 2, w, 4, approvalHoldFull ? kWarn : kOk);
    }

    canvas.setTextColor(kOk, kBg);
    canvas.drawString("hold ENT", 4, 100);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("allow once", 56, 100);

    canvas.setTextColor(kAccent, kBg);
    canvas.drawString("hold Fn+ENT", 4, 112);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("full access", 74, 112);

    canvas.setTextColor(kAccent, kBg);
    canvas.drawString("D", 4, 124);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("reject", 15, 124);

    canvas.setTextColor(kAccent, kBg);
    canvas.drawString("esc", 66, 124);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("desktop", 90, 124);
}

/// OTA overlay: one dedicated page, no HARNESS chrome.  While the image is
/// being written the loop returns before WS/voice/key handling, so this page
/// and ArduinoOTA.handle() are the only things running.
void drawOta()
{
    if (otaUploading) {
        flashui::draw(canvas, otaPercent, "DO NOT EXIT");
        return;
    }
    if (otaFailed) {
        flashui::drawError(canvas, otaStatus);
        return;
    }
    flashui::draw(canvas, otaPercent, "WAITING FOR UPLOAD");
}

/// New-session control chooser: the boundary is picked before the Session is
/// created, because the Host applies the matching permission preset at create.
void drawNew()
{
    canvas.fillScreen(kBg);
    drawHeader();
    canvas.setTextFont(1);
    canvas.setTextColor(kText, kBg);
    canvas.drawString("NEW SESSION", 4, 18);
    canvas.setTextColor(kDim, kBg);
    canvas.drawString("control boundary", 4, 30);

    static const char* const names[2] = {"WORKSPACE WRITE", "FULL ACCESS"};
    static const char* const notes[2] = {"asks before wider steps", "no approval prompts"};
    for (int i = 0; i < 2; ++i) {
        const int  y   = 44 + i * 26;
        const bool sel = i == newControl;
        if (sel) canvas.fillRect(0, y - 2, lcd.width(), 24, kBar);
        canvas.setTextColor(sel ? kAccent : kText, sel ? kBar : kBg);
        canvas.drawString(names[i], 8, y);
        canvas.setTextColor(sel ? kText : kDim, sel ? kBar : kBg);
        canvas.drawString(notes[i], 8, y + 11);
    }

    canvas.setTextColor(kDim, kBg);
    canvas.drawString("; . choose", 4, 120);
    canvas.drawString("ENT create", 78, 120);
    canvas.drawString("esc back", 160, 120);
}

void render()
{
    switch (screen) {
        case Screen::kDeck: drawDeck(); break;
        case Screen::kChat: drawChat(); break;
        case Screen::kPreview: drawPreview(); break;
        case Screen::kEdit: drawEdit(); break;
        case Screen::kConfig: drawConfig(); break;
        case Screen::kApproval: drawApproval(); break;
        case Screen::kNew: drawNew(); break;
        case Screen::kOta: drawOta(); break;
    }
    canvas.pushSprite(&lcd, 0, 0);
    dirty = false;
}

// ---------------------------------------------------------------------------
// wifi / link bring-up

// Start ArduinoOTA as soon as HARNESS has a station link.  Keeping it up in
// this app is what lets an OTA flash from the HARNESS screen; after the reboot
// the launcher's stored choice is still HARNESS.
void startOta()
{
    if (otaStarted || WiFi.status() != WL_CONNECTED) return;
    ArduinoOTA.setHostname("cardputer");
    ArduinoOTA.setPassword("cardputer-ota");
    ArduinoOTA.setPort(3232);
    ArduinoOTA.onStart([]() {
        otaUploading = true;
        otaFailed    = false;
        otaPercent   = 0;
        snprintf(otaStatus, sizeof(otaStatus), "UPLOADING");
        otaReturn = screen == Screen::kOta ? otaReturn : screen;
        screen    = Screen::kOta;
        kbdClearPresses();
        markDirty();
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        otaPercent = total ? static_cast<int>(progress * 100 / total) : 0;
        markDirty();
    });
    ArduinoOTA.onEnd([]() {
        otaUploading = false;
        otaPercent   = 100;
        snprintf(otaStatus, sizeof(otaStatus), "DONE - REBOOTING");
        // Same rising cue as the SETTINGS OTA page; the full-duplex path keeps
        // the speaker live regardless of microphone state.
        audioTone(1047, 90); audioSilence(20);
        audioTone(1319, 90); audioSilence(20);
        audioTone(1568, 90); audioSilence(20);
        audioTone(2093, 240); audioSilence(30);
        markDirty();
    });
    ArduinoOTA.onError([](ota_error_t error) {
        otaUploading = false;
        otaFailed    = true;
        snprintf(otaStatus, sizeof(otaStatus), "ERROR %d", static_cast<int>(error));
        kbdClearPresses();
        markDirty();
    });
    ArduinoOTA.begin();
    otaStarted = true;
    usbLogf("[ota] ready ip=%s", WiFi.localIP().toString().c_str());
}

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
    if (!otaStarted) startOta();
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
    // HARNESS owns the ES8311 and I2S0; stop M5Unified from installing its
    // single-direction audio drivers before the full-duplex one is created.
    cfg.internal_spk = false;
    cfg.internal_mic = false;
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
    ws.setMessageHandler(handleWsMessage, nullptr);

    // Bring the full-duplex audio up before the first reply/chime, so the
    // microphone and speaker are both available from the start.
    g_micUp = voiceI2sBegin();

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
        usbLogf("[mem] free=%u min=%u msgs=%d bad=%u reset=%d hw=%u", ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), static_cast<int>(msgs.size()), static_cast<unsigned>(parseFailures),
                static_cast<int>(esp_reset_reason()), static_cast<unsigned>(voiceHwHz));
    }

    M5Cardputer.update();
    if (otaStarted) ArduinoOTA.handle();

    // Flashing owns the device: no WS, microphone, chat, keys or marquee.  Only
    // the OTA callback and the dedicated progress page run until the reboot.
    if (otaUploading) {
        if (dirty) render();
        delay(2);
        return;
    }

    Point2D_t point;
    while (kbdPopPress(point)) handlePress(point);

    // DSH approval: ENT must be held for ~1.2 s to allow once.  The press edge
    // alone is intentionally ignored (handlePress), so a key already in flight
    // when the prompt appears cannot grant a tool.  D and esc are edge actions.
    if (screen == Screen::kApproval) {
        static uint32_t lastApprovalPaint = 0;
        const bool held = M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER);
        const bool full = held && M5Cardputer.Keyboard.keysState().fn;
        if (!held) {
            // Arming needs a release: if ENT was already down when the prompt
            // arrived, that keystroke must not count toward the hold.
            approvalArmed = true;
            if (approvalHoldAt != 0) {
                approvalHoldAt = 0;
                approvalHoldFull = false;
                dirty = true;
            }
        } else if (approvalArmed) {
            if (approvalHoldAt == 0) {
                approvalHoldAt  = millis();
                approvalHoldFull = full;
                dirty = true;
            } else if (approvalHoldFull != full) {
                // The chord changed mid-hold; restart so the wrong grant (once
                // vs full control) can never fire from a mixed hold.
                approvalHoldAt  = millis();
                approvalHoldFull = full;
                dirty = true;
            } else if (millis() - approvalHoldAt >= 1200) {
                answerApproval(full ? "full" : "allowed-once");
            }
        }
        if (screen == Screen::kApproval) {
            const uint32_t now = millis();
            if (now - lastApprovalPaint >= 200) {
                lastApprovalPaint = now;
                dirty = true;
            }
            // The Host defers to the Desktop at 45 s; close even if its final
            // frame was lost with a dropped link.
            if (now - approvalAt > 48000) {
                usbLogf("[approval] timeout, closing");
                screen           = approvalReturn;
                approvalId       = "";
                approvalHoldAt   = 0;
                approvalArmed    = false;
                approvalHoldFull = false;
                markDirty();
            }
        }
    }

    linkService();
    ws.loop();

    voiceService();

    // Blink the composer caret.
    if (screen == Screen::kChat || screen == Screen::kEdit) {
        if (millis() - caretAt > 500) {
            caretAt = millis();
            caretOn = !caretOn;
            markDirty();
        }
    }

    // Footer pages advance once every 3 s; one repaint per page, no marquee.
    if (screen == Screen::kDeck || screen == Screen::kChat) {
        static uint32_t lastHintPage = 0;
        if (millis() - lastHintPage >= 3000) {
            lastHintPage = millis();
            markDirty();
        }
    }

    if (dirty) render();
    delay(2);
}

}  // namespace harnessapp
