#include "web_app.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>

#include <memory>

#include "launcher.h"
#include "polling_keyboard.h"
#include "rf_config.h"
#include "sd_mount.h"
#include "ui_hint.h"
#include "wifi_store.h"

namespace webapp {
namespace {

constexpr uint16_t kBg     = 0x0000;
constexpr uint16_t kText   = 0xFFFF;
constexpr uint16_t kDim    = 0x8410;
constexpr uint16_t kAccent = 0xFD20;
constexpr uint16_t kOk     = 0x07E0;
constexpr uint16_t kWarn   = 0xF800;
constexpr uint16_t kHead   = 0x18E3;

// Basic auth: change both strings if the Cardputer is on an untrusted LAN.
constexpr char kUser[] = "admin";
constexpr char kPass[] = "cardputer";

auto& lcd = M5Cardputer.Display;
WebServer server(80);
bool sdOk = false;
bool wifiOk = false;
bool dirty = true;
uint32_t prevActions = 0;
uint32_t requests = 0;
char status[64] = {0};
String uploadPath = "/";
String browsePath = "/";
File uploadFile;
size_t uploadBytes = 0;
// Per-upload state.  The upload callback fires while the request body is
// still being parsed -- long before uploadDone()'s auth() runs -- so an
// unauthenticated POST /upload would otherwise land a file on the card and
// only then receive its 401.  uploadAuthorized is checked on every chunk.
bool uploadAuthorized = false;
bool uploadTooBig     = false;
String uploadFilePath;
// Hard cap per uploaded file, and the free space that must remain before an
// upload may start.  These bound a hostile or runaway POST even though the
// body itself is consumed by the parser regardless.
constexpr size_t kUploadMaxBytes = 256u * 1024u * 1024u;
constexpr uint64_t kUploadMinFreeBytes = 8u * 1024u * 1024u;

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

void header(const char* title)
{
    lcd.fillRect(0, 0, lcd.width(), 13, kHead);
    lcd.setTextColor(kText, kHead);
    lcd.drawString(title, 3, 2);
    char b[20];
    snprintf(b, sizeof(b), "%d%%", M5.Power.getBatteryLevel());
    lcd.drawRightString(b, lcd.width() - 3, 2);
}

String htmlEscape(const String& in)
{
    String out;
    out.reserve(in.length() + 16);
    for (size_t i = 0; i < in.length(); ++i) {
        const char c = in[i];
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

String urlEncode(const String& in)
{
    String out;
    const char* hex = "0123456789ABCDEF";
    for (size_t i = 0; i < in.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(in[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

String cleanPath(const String& raw)
{
    String p = raw;
    if (!p.startsWith("/")) p = "/" + p;
    if (p.indexOf("..") >= 0) return "/";
    while (p.indexOf("//") >= 0) p.replace("//", "/");
    if (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
    return p;
}

String baseName(const String& path)
{
    const int slash = path.lastIndexOf('/');
    return slash >= 0 ? path.substring(slash + 1) : path;
}

bool auth()
{
    if (server.authenticate(kUser, kPass)) return true;
    server.requestAuthentication();
    return false;
}

String pageHeader()
{
    String h;
    h.reserve(1024);
    h += "<!doctype html><html><head><meta charset='utf-8'>";
    h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    h += "<title>Cardputer TF</title><style>";
    h += "body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}";
    h += "main{max-width:860px;margin:0 auto;padding:16px}";
    h += "a{color:#fd8}h1{font-size:20px}li{margin:6px 0}";
    h += "button{padding:8px 12px;border:0;border-radius:6px;background:#fd8;font-weight:700}";
    h += "input[type=file]{color:#eee}.muted{color:#999}.danger{color:#f66}";
    h += "</style></head><body><main>";
    return h;
}

void sendDir()
{
    if (!auth()) return;
    ++requests;
    browsePath = cleanPath(server.arg("path"));
    if (browsePath.length() == 0) browsePath = "/";
    uploadPath = browsePath;

    File dir = SD.open(browsePath);
    if (!dir) {
        server.send(404, "text/plain", "cannot open directory");
        return;
    }
    if (!dir.isDirectory()) {
        dir.close();
        server.send(400, "text/plain", "not a directory");
        return;
    }

    String html = pageHeader();
    html += "<h1>Cardputer TF</h1>";
    html += "<p class='muted'>path: ";
    html += htmlEscape(browsePath);
    html += " &middot; <a href='/'>root</a></p>";
    if (browsePath != "/") {
        String parent = browsePath;
        parent.remove(parent.lastIndexOf('/'));
        if (parent.length() == 0) parent = "/";
        html += "<p><a href='/?path=" + urlEncode(parent) + "'>&larr; up</a></p>";
    }
    html += "<form method='post' enctype='multipart/form-data' action='/upload'>";
    html += "<input type='file' name='file' required> <button>Upload</button>";
    html += "</form><ul>";

    int n = 0;
    while (n < 200) {
        File f = dir.openNextFile();
        if (!f) break;
        const char* raw = f.name();
        const char* slash = strrchr(raw, '/');
        const char* base = slash ? slash + 1 : raw;
        if (base[0] == 0 || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
            f.close();
            continue;
        }
        const bool isDir = f.isDirectory();
        const String name = base;
        String path = browsePath == "/" ? "/" + name : browsePath + "/" + name;
        if (isDir) {
            html += "<li><a href='/?path=" + urlEncode(path) + "'>" + htmlEscape(name) + "/</a></li>";
        } else {
            html += "<li><a href='/download?path=" + urlEncode(path) + "'>" +
                    htmlEscape(name) + "</a> <span class='muted'>" +
                    String(static_cast<unsigned long>(f.size())) + " B</span> ";
            html += "<a class='danger' href='/delete?path=" + urlEncode(path) +
                    "' onclick=\"return confirm('Delete this file?')\">delete</a></li>";
        }
        f.close();
        ++n;
    }
    dir.close();
    html += "</ul><p class='muted'>Only the TF card is exposed. ";
    html += "Cardputer Web Files / LAN use only.</p></main></body></html>";
    server.send(200, "text/html", html);
}

void sendDownload()
{
    if (!auth()) return;
    const String path = cleanPath(server.arg("path"));
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        server.send(404, "text/plain", "not found");
        return;
    }
    const size_t bytes = f.size();
    server.setContentLength(bytes);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + baseName(path) + "\"");
    server.send(200, "application/octet-stream", "");

    WiFiClient client = server.client();
    client.setNoDelay(true);
    uint8_t buf[1024];
    size_t sent = 0;
    const uint32_t start = millis();
    while (f.available() && client.connected()) {
        const size_t n = f.read(buf, sizeof(buf));
        if (n == 0) break;
        size_t off = 0;
        while (off < n) {
            const size_t w = client.write(buf + off, n - off);
            if (w == 0) {
                delay(1);
                if (millis() - start > 30000) break;
                continue;
            }
            off += w;
            sent += w;
        }
        delay(1);
        if (millis() - start > 30000) break;
    }
    f.close();
    client.flush();
    Serial.printf("[web] download %s %u/%u\n", path.c_str(), static_cast<unsigned>(sent),
                  static_cast<unsigned>(bytes));
}

void sendDelete()
{
    if (!auth()) return;
    const String path = cleanPath(server.arg("path"));
    if (path == "/") {
        server.send(400, "text/plain", "refuse root");
        return;
    }
    const bool ok = SD.remove(path);
    server.sendHeader("Location", "/?path=" + urlEncode(browsePath));
    server.send(303, "text/plain", ok ? "deleted" : "delete failed");
}

void handleUploadChunk()
{
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        // The request headers are fully parsed by now, so the credentials (if
        // any) can be checked here, on the first chunk, instead of waiting for
        // uploadDone() -- which only runs after the whole body has been
        // consumed and the file is already on the card.
        uploadAuthorized = server.authenticate(kUser, kPass);
        uploadTooBig     = false;
        uploadFilePath   = "";
        if (!uploadAuthorized) {
            snprintf(status, sizeof(status), "UPLOAD DENIED");
            dirty = true;
            return;
        }
        String name = baseName(up.filename);
        if (name.length() == 0 || name.indexOf("..") >= 0) name = "upload.bin";
        String path = uploadPath == "/" ? "/" + name : uploadPath + "/" + name;
        const uint64_t total = SD.totalBytes();
        const uint64_t used  = SD.usedBytes();
        if (total > used && total - used < kUploadMinFreeBytes) {
            snprintf(status, sizeof(status), "TF NEARLY FULL");
            dirty = true;
            return;
        }
        uploadFile = SD.open(path, FILE_WRITE);
        uploadBytes = 0;
        if (uploadFile) {
            uploadFilePath = path;
            snprintf(status, sizeof(status), "UPLOAD %s", name.c_str());
            dirty = true;
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!uploadAuthorized) return;
        if (uploadFile) {
            if (uploadBytes + up.currentSize > kUploadMaxBytes) {
                // Over the cap: stop accepting, drop what was written so far.
                uploadTooBig = true;
                uploadFile.close();
                SD.remove(uploadFilePath);
                uploadFilePath = "";
                snprintf(status, sizeof(status), "UPLOAD TOO BIG");
                dirty = true;
                return;
            }
            const size_t w = uploadFile.write(up.buf, up.currentSize);
            uploadBytes += w;
        }
    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) uploadFile.close();
        if (!uploadAuthorized || uploadTooBig || up.status == UPLOAD_FILE_ABORTED) {
            // Never leave half a file behind: an aborted, oversized or
            // unauthorized upload is removed rather than kept.
            if (uploadFilePath.length() > 0) SD.remove(uploadFilePath);
            uploadFilePath = "";
            uploadBytes = 0;
            snprintf(status, sizeof(status),
                     up.status == UPLOAD_FILE_ABORTED ? "UPLOAD ABORTED"
                                                      : (uploadTooBig ? "UPLOAD TOO BIG"
                                                                      : "UPLOAD DENIED"));
        } else {
            snprintf(status, sizeof(status), "UPLOAD %u B", static_cast<unsigned>(uploadBytes));
        }
        dirty = true;
    }
}

void uploadDone()
{
    if (!auth()) return;
    if (uploadTooBig) {
        server.send(413, "text/plain", "upload too large");
        return;
    }
    server.sendHeader("Location", "/?path=" + urlEncode(uploadPath));
    server.send(303, "text/plain", "uploaded");
}

void onNotFound()
{
    if (!auth()) return;
    server.send(404, "text/plain", "not found");
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

void connectWifi()
{
    wifistore::begin();
    const auto* net = wifistore::last();
    if (net == nullptr) {
        snprintf(status, sizeof(status), "NO WIFI - USE SETTINGS");
        dirty = true;
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(net->ssid, net->pass);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        M5Cardputer.update();
        delay(50);
    }
    wifiOk = WiFi.status() == WL_CONNECTED;
    if (wifiOk) {
        snprintf(status, sizeof(status), "http://%s/", WiFi.localIP().toString().c_str());
        Serial.printf("[web] %s\n", status);
    } else {
        snprintf(status, sizeof(status), "WIFI FAILED");
    }
    dirty = true;
}

void draw()
{
    lcd.fillScreen(kBg);
    header("WEB FILES");
    lcd.setTextColor(kText, kBg);
    lcd.drawString(status, 4, 24);
    lcd.setTextColor(kDim, kBg);
    lcd.drawString("TF root exposed over HTTP", 4, 42);
    lcd.drawString("user admin / pass cardputer", 4, 54);
    lcd.drawString("Trusted LAN only.", 4, 66);
    lcd.setTextColor(kAccent, kBg);
    lcd.drawString(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "NO IP",
                   4, 84);
    lcd.setTextColor(kDim, kBg);
    char req[32];
    snprintf(req, sizeof(req), "requests %lu", static_cast<unsigned long>(requests));
    lcd.drawString(req, 4, 100);
    static const Hint h[] = {{"esc", "back"}};
    hints(h, 1);
    dirty = false;
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
        draw();
        return;
    }
    connectWifi();
    if (!wifiOk) {
        draw();
        return;
    }

    server.on("/", HTTP_GET, sendDir);
    server.on("/download", HTTP_GET, sendDownload);
    server.on("/delete", HTTP_GET, sendDelete);
    server.on("/upload", HTTP_POST, uploadDone, handleUploadChunk);
    server.onNotFound(onNotFound);
    server.begin();
    server.enableCORS(true);
    snprintf(status, sizeof(status), "http://%s/", WiFi.localIP().toString().c_str());
    draw();
}

void loop()
{
    M5Cardputer.update();
    server.handleClient();

    const uint32_t actions = readActions();
    const uint32_t pressed = actions & ~prevActions;
    prevActions = actions;
    if (pressed & kBack) {
        server.stop();
        launcher::rebootInto(launcher::kAppMenu);
    }
    if (dirty) draw();
    delay(2);
}

}  // namespace webapp
