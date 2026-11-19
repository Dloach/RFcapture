#include "harness_ws.h"

#include <esp_random.h>
#include <mbedtls/base64.h>

namespace harnessws {
namespace {

// A reconnect attempt is one short blocking TCP connect; everything else is
// driven from loop().  1500 ms is the worst case a key press waits while the
// link is down, and it is long enough for a station that just got its lease.
constexpr uint32_t kRetryMs            = 2500;
constexpr uint32_t kConnectTimeoutMs   = 1500;
constexpr uint32_t kHandshakeTimeoutMs = 1500;
constexpr uint32_t kPingMs             = 20000;
// Largest frame accepted from the Host.  The device asks for small pages, so a
// normal frame is 1-6 KiB.  A frame above this is discarded (never delivered,
// link kept) rather than treated as a protocol error, so one oversized
// snapshot cannot trap the client in a reconnect loop.
constexpr size_t   kRxLimit            = 16384;
// Bound on the unconsumed span of rx_: parseFrames() drains every complete
// frame, so the span is at most one maximum-size frame being assembled plus a
// read chunk.  The old check in pump() compared the whole buffered stream
// against kRxLimit, which dropped a legal 16 KiB frame (header pushed it over)
// and misreported several coalesced small frames as "too large".
constexpr size_t   kStreamLimit        = kRxLimit + 512;
constexpr size_t   kHandshakeLimit     = 1024;

// Index just past the "\r\n\r\n" that ends the HTTP response headers, or -1.
int findHeaderEnd(const std::vector<uint8_t>& buffer)
{
    if (buffer.size() < 4) return -1;
    for (size_t i = 0; i + 4 <= buffer.size(); ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' &&
            buffer[i + 3] == '\n') {
            return static_cast<int>(i + 4);
        }
    }
    return -1;
}

}  // namespace

void Client::begin(const String& host, uint16_t port, const String& path, const String& token)
{
    host_   = host;
    port_   = port;
    path_   = path;
    if (!token.isEmpty()) {
        path_ += path_.indexOf('?') >= 0 ? "&token=" : "?token=";
        path_ += token;
    }
    booted_          = true;
    state_           = State::kDown;
    resolved_        = false;
    lastAttemptMs_   = 0;
    handshake_.clear();
    rx_.clear();
    rxOff_           = 0;
    skipRemaining_   = 0;
    discardFragment_ = false;
    fragments_.clear();
    fragmentOpcode_  = 0;
}

void Client::restart()
{
    drop("restart");
    lastAttemptMs_ = 0;
}

void Client::loop()
{
    if (!booted_) return;
    const uint32_t now = millis();

    switch (state_) {
        case State::kDown:
            if (WiFi.status() != WL_CONNECTED) return;
            if (now - lastAttemptMs_ < kRetryMs) return;
            lastAttemptMs_ = now;
            connectAttempt(now);
            return;

        case State::kHandshake: {
            uint8_t buffer[128];
            while (socket_.available() > 0) {
                const int n = socket_.read(buffer, sizeof(buffer));
                if (n <= 0) break;
                if (handshake_.size() + static_cast<size_t>(n) > kHandshakeLimit) {
                    drop("handshake too long");
                    return;
                }
                handshake_.insert(handshake_.end(), buffer, buffer + n);
            }

            const int bodyStart = findHeaderEnd(handshake_);
            if (bodyStart >= 0) {
                const String status(reinterpret_cast<const char*>(handshake_.data()),
                                    static_cast<unsigned>(bodyStart));
                if (status.indexOf(" 101 ") < 0) {
                    drop("handshake rejected");
                    return;
                }
                state_      = State::kOpen;
                lastRxMs_   = now;
                lastPingMs_ = now;
                rx_.clear();
                rxOff_ = 0;

                // The Host sends `hello` immediately after the 101, usually in
                // the same TCP segment.  Those bytes are the start of the frame
                // stream, not part of the response: dropping them meant the
                // device never saw `hello` and so never asked for the Session
                // list until a key press triggered a later request.
                const size_t leftover = handshake_.size() - static_cast<size_t>(bodyStart);
                if (leftover > 0) {
                    rx_.insert(rx_.end(), handshake_.begin() + bodyStart, handshake_.end());
                }
                handshake_.clear();
                parseFrames();
                return;
            }
            if (now > handshakeDueMs_ || !socket_.connected()) drop("handshake timeout");
            return;
        }

        case State::kOpen:
            if (!socket_.connected()) {
                drop("closed");
                return;
            }
            pump();
            // Dead-link detection: a bridge that died without a FIN (host
            // sleep, power cut, cable pull) keeps socket_.connected() true and
            // the writes "succeed" into lwIP's buffer for minutes.  Every ping
            // is answered with a pong, so a healthy link refreshes lastRxMs_
            // within one ping interval; three intervals with no traffic in
            // either direction means the socket is gone.
            //
            // The clock is read HERE, not from the `now` captured at loop()
            // entry: pump() may have delivered a frame and refreshed
            // lastRxMs_ milliseconds after that snapshot, and the unsigned
            // subtraction below would wrap to ~2^32 for that 1-2 ms skew --
            // which dropped a healthy link on the very same tick the sessions
            // list arrived (seen live as "now=26388 last=26390").  The
            // timestamps ride along in the drop reason so the next such
            // dispute is settled by one glance at the log.
            const uint32_t nowOpen = millis();
            if (state_ == State::kOpen && nowOpen - lastRxMs_ >= 3u * kPingMs) {
                static char why[64];
                snprintf(why, sizeof(why), "rx timeout now=%lu last=%lu",
                         static_cast<unsigned long>(nowOpen),
                         static_cast<unsigned long>(lastRxMs_));
                drop(why);
                return;
            }
            if (state_ == State::kOpen && nowOpen - lastPingMs_ >= kPingMs) {
                lastPingMs_ = nowOpen;
                sendFrame(0x9, nullptr, 0);
            }
            return;
    }
}

void Client::connectAttempt(uint32_t now)
{
    socket_.stop();

    if (!resolved_ || ip_ == IPAddress()) {
        // A configured IP literal skips DNS entirely, so a normal reconnect
        // never blocks on name resolution.
        IPAddress literal;
        if (literal.fromString(host_)) {
            ip_ = literal;
        } else if (!WiFi.hostByName(host_.c_str(), ip_)) {
            lastDrop_ = "resolve failed";
            return;
        }
        resolved_ = true;
    }
    if (!socket_.connect(ip_, port_, kConnectTimeoutMs)) {
        lastDrop_ = "tcp connect failed";
        resolved_ = false;  // force a fresh resolve next time (DHCP may have changed)
        return;
    }
    socket_.setNoDelay(true);
    socket_.setTimeout(kConnectTimeoutMs);

    handshake_.clear();
    state_          = State::kHandshake;
    handshakeDueMs_ = now + kHandshakeTimeoutMs;
    sendHandshake();
}

void Client::sendHandshake()
{
    uint8_t key[16];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(esp_random() & 0xFF);

    char encoded[32] = {0};
    size_t encodedLen = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded), sizeof(encoded) - 1,
                              &encodedLen, key, sizeof(key)) != 0) {
        drop("key");
        return;
    }
    encoded[encodedLen] = '\0';

    String request;
    request.reserve(220);
    request += "GET " + path_ + " HTTP/1.1\r\n";
    request += "Host: " + host_ + ":" + String(port_) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: ";
    request += encoded;
    request += "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    socket_.write(reinterpret_cast<const uint8_t*>(request.c_str()), request.length());
}

void Client::pump()
{
    uint8_t buffer[128];
    while (socket_.available() > 0) {
        const int n = socket_.read(buffer, sizeof(buffer));
        if (n <= 0) break;
        size_t off  = 0;
        size_t keep = static_cast<size_t>(n);
        if (skipRemaining_ > 0) {
            // Inside an oversized frame: its payload goes straight to the
            // floor, so a hostile or runaway frame costs no heap.  The bytes
            // past its end belong to the next frame and are kept.
            const uint64_t take = skipRemaining_ < keep ? skipRemaining_ : keep;
            skipRemaining_ -= take;
            off  += static_cast<size_t>(take);
            keep -= static_cast<size_t>(take);
        }
        if (keep > 0) {
            if (rx_.size() - rxOff_ + keep > kStreamLimit) {
                drop("stream overflow");
                return;
            }
            rx_.insert(rx_.end(), buffer + off, buffer + off + keep);
        }
        // Parse after EVERY read, not once after the whole socket drained.
        // A burst larger than kStreamLimit (the old plugin pushes uncollapsed
        // snapshots of 30 KiB+) arrives faster than a deferred parse could
        // consume it: the span cap above fired mid-burst, the link dropped,
        // the reconnect re-requested the session, and the same burst came
        // back -- an endless offline/online/offline cycle.  Parsing here lets
        // parseFrames() see the oversized header early and enter skip mode,
        // after which the bytes are discarded right at the read.
        if (keep > 0 || skipRemaining_ == 0) parseFrames();
        if (state_ != State::kOpen) return;
    }
    parseFrames();
}

void Client::parseFrames()
{
    while (state_ == State::kOpen) {
        const size_t available = rx_.size() - rxOff_;
        if (available < 2) break;

        const uint8_t* p = rx_.data() + rxOff_;
        const bool     fin    = (p[0] & 0x80) != 0;
        const uint8_t  opcode = p[0] & 0x0F;
        const bool     masked = (p[1] & 0x80) != 0;
        uint64_t       length = p[1] & 0x7F;
        size_t         header = 2;

        if (length == 126) {
            if (available < 4) break;
            length = (static_cast<uint16_t>(p[2]) << 8) | p[3];
            header = 4;
        } else if (length == 127) {
            if (available < 10) break;
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | p[2 + i];
            header = 10;
        }
        const size_t maskOffset = header;
        if (masked) header += 4;

        // Consume this frame without delivering it: advance past what is
        // buffered, and enter skip mode when only part of the payload has
        // arrived.  (When the frame is incomplete, everything buffered is
        // payload -- no later frame's bytes can exist yet.)
        auto discardFrame = [&]() {
            if (available <= header) {
                // Not even the full header (mask bytes included) has arrived:
                // skip what is left of header plus the whole payload.
                skipRemaining_ = length + (header - available);
                rxOff_         = rx_.size();
                return;
            }
            const uint64_t have = available - header;
            if (length <= have) {
                rxOff_ += header + static_cast<size_t>(length);
            } else {
                skipRemaining_ = length - have;
                rxOff_         = rx_.size();
            }
        };

        if (length > kRxLimit) {
            // Oversized but structurally valid: throw the frame away and keep
            // the link.  The old behaviour dropped the socket, the reconnect
            // re-requested the same snapshot, and the same oversized frame came
            // back -- the session could never be opened, all the user saw was
            // an endless RECONNECT.
            discardFrame();
            // A message this large is never delivered, and if it was itself a
            // fragment the message it belonged to is doomed: swallow its
            // remaining continuation frames until the FIN.
            fragmentOpcode_  = 0;
            fragments_.clear();
            discardFragment_ = !fin;
            continue;
        }
        if (discardFragment_ && opcode == 0x0) {
            // Continuation of a message whose earlier fragment was discarded.
            discardFrame();
            if (fin) discardFragment_ = false;
            continue;
        }
        if (available < header + length) break;

        // A server must not mask, but tolerate one that does.
        std::vector<uint8_t> unmasked;
        const uint8_t* payload = rx_.data() + rxOff_ + header;
        if (masked) {
            const uint8_t* key = rx_.data() + rxOff_ + maskOffset;
            unmasked.resize(static_cast<size_t>(length));
            for (size_t i = 0; i < length; ++i) unmasked[i] = payload[i] ^ key[i & 3];
            payload = unmasked.data();
        }

        handleFrame(opcode, payload, static_cast<size_t>(length), fin);
        rxOff_ += header + static_cast<size_t>(length);
        if (state_ != State::kOpen) return;
    }

    // Reclaim the consumed prefix once it is worth the memmove.
    if (rxOff_ > 0 && (rxOff_ == rx_.size() || rxOff_ > 2048)) {
        if (rxOff_ == rx_.size()) {
            rx_.clear();
            rxOff_ = 0;
        } else {
            rx_.erase(rx_.begin(), rx_.begin() + rxOff_);
            rxOff_ = 0;
        }
    }
}

void Client::handleFrame(uint8_t opcode, const uint8_t* payload, size_t length, bool fin)
{
    switch (opcode) {
        case 0x1:  // text
        case 0x2:  // binary
            if (fragmentOpcode_ != 0) {
                drop("nested fragment");
                return;
            }
            if (fin) {
                if (opcode == 0x1) {
                    ++received_;
                    lastRxMs_ = millis();
                    if (handler_ != nullptr) {
                        // The handler reads a NUL-terminated string; the frame
                        // buffer itself is reused, so hand it a copy.
                        String text(reinterpret_cast<const char*>(payload), length);
                        handler_(text.c_str(), text.length(), handlerArg_);
                    }
                }
                return;
            }
            fragmentOpcode_ = opcode;
            fragments_.assign(payload, payload + length);
            return;

        case 0x0:  // continuation
            if (fragmentOpcode_ == 0) {
                drop("stray continuation");
                return;
            }
            if (fragments_.size() + length > kRxLimit) {
                // The assembled message would exceed the frame limit: abandon
                // the whole message but keep the link (same reasoning as the
                // single-frame oversize path in parseFrames).
                fragmentOpcode_  = 0;
                fragments_.clear();
                discardFragment_ = !fin;
                return;
            }
            fragments_.insert(fragments_.end(), payload, payload + length);
            if (!fin) return;
            if (fragmentOpcode_ == 0x1) {
                ++received_;
                lastRxMs_ = millis();
                if (handler_ != nullptr) {
                    String text(reinterpret_cast<const char*>(fragments_.data()), fragments_.size());
                    handler_(text.c_str(), text.length(), handlerArg_);
                }
            }
            fragments_.clear();
            fragmentOpcode_ = 0;
            return;

        case 0x8:  // close
            drop("peer close");
            return;

        case 0x9:  // ping
            sendFrame(0xA, payload, length);
            return;

        case 0xA:  // pong
            // On an otherwise idle link the only traffic is our own
            // ping/pong pair, so a pong is the liveness proof the rx
            // watchdog in loop() judges by.
            lastRxMs_ = millis();
            return;

        default:
            return;
    }
}

bool Client::sendText(const String& json)
{
    return sendFrame(0x1, reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
}

bool Client::sendFrame(uint8_t opcode, const uint8_t* data, size_t length)
{
    if (state_ != State::kOpen) return false;
    if (length > 16000) return false;

    uint8_t header[14];
    size_t  h = 0;
    header[h++] = static_cast<uint8_t>(0x80 | opcode);
    if (length < 126) {
        header[h++] = static_cast<uint8_t>(0x80 | length);
    } else {
        header[h++] = static_cast<uint8_t>(0x80 | 126);
        header[h++] = static_cast<uint8_t>((length >> 8) & 0xFF);
        header[h++] = static_cast<uint8_t>(length & 0xFF);
    }
    const uint32_t mask = esp_random();
    const uint8_t  mb[4] = {static_cast<uint8_t>(mask >> 24), static_cast<uint8_t>(mask >> 16),
                            static_cast<uint8_t>(mask >> 8), static_cast<uint8_t>(mask)};
    for (int i = 0; i < 4; ++i) header[h++] = mb[i];

    // NOTE: WiFiClient does not override availableForWrite() on this core
    // (the base Client returns 0), so it cannot be used to gate sends.  The
    // 60 ms audio chunks are small enough for lwIP's send buffer instead.

    if (socket_.write(header, h) != h) {
        drop("write header");
        return false;
    }

    // Mask and send the whole payload in ONE write. The old 128-byte chunking
    // made ~80 separate tcp_write calls per frame; once the lwIP send buffer
    // (~5.7 KiB) filled, each small write stalled on a delayed ACK (~60 ms),
    // so a 10 KiB audio frame took ~4.8 s and starved the capture loop. A single
    // write lets lwIP segment and pipeline the frame instead. The mask buffer is
    // static (not malloc): the heap is fragmented enough that a second ~10 KiB
    // contiguous block fails even with ~76 KiB total free.
    static uint8_t masked[16000];
    for (size_t i = 0; i < length; ++i) {
        masked[i] = static_cast<uint8_t>(data[i] ^ mb[i & 3]);
    }
    if (socket_.write(masked, length) != length) {
        drop("write payload");
        return false;
    }
    return true;
}

void Client::drop(const char* why)
{
    lastDrop_ = why;
    socket_.stop();
    state_           = State::kDown;
    fragmentOpcode_  = 0;
    fragments_.clear();
    rx_.clear();
    rxOff_           = 0;
    skipRemaining_   = 0;
    discardFragment_ = false;
    handshake_.clear();
    lastAttemptMs_   = millis();
}

}  // namespace harnessws
