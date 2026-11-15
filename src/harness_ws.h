/*
 * Minimal non-blocking WebSocket (RFC 6455) client for the HARNESS app.
 *
 * The stock HTTP path used HTTPClient, whose begin()/POST()/getString() block
 * the UI loop for the whole request.  This client never blocks while it is
 * connected: loop() drains a socket that already has bytes and returns, so a
 * key press or a redraw is never delayed by the network.
 *
 * Only text frames are used in both directions.  Server-to-client frames are
 * unmasked (RFC 6455 5.1); client-to-server frames are masked here.
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <vector>

namespace harnessws {

enum class State : uint8_t {
    kDown = 0,     // no socket; waiting for the retry interval
    kHandshake,    // TCP is up, waiting for "101 Switching Protocols"
    kOpen,         // upgraded; text frames flow both ways
};

using MessageHandler = void (*)(const char* data, size_t length, void* arg);

class Client {
public:
    /// Point the client at ws://host:port/path?token=... and start retrying.
    void begin(const String& host, uint16_t port, const String& path, const String& token);

    /// Advance the state machine.  Never blocks while kOpen.
    void loop();

    /// Queue one JSON text frame.  False when the socket is not open.
    bool sendText(const String& json);

    State state() const { return state_; }
    bool open() const { return state_ == State::kOpen; }

    /// Count of complete text messages received (diagnostics).
    uint32_t received() const { return received_; }

    /// Why the last connection ended (diagnostics; never null).
    const char* lastDrop() const { return lastDrop_; }

    void setMessageHandler(MessageHandler handler, void* arg)
    {
        handler_     = handler;
        handlerArg_  = arg;
    }

    /// Drop the socket and retry immediately on the next loop().
    void restart();

private:
    void connectAttempt(uint32_t now);
    void sendHandshake();
    void pump();
    void parseFrames();
    void handleFrame(uint8_t opcode, const uint8_t* payload, size_t length, bool fin);
    bool sendFrame(uint8_t opcode, const uint8_t* data, size_t length);
    void drop(const char* why);

    WiFiClient socket_;
    String     host_;
    String     path_;
    uint16_t   port_ = 0;

    State    state_          = State::kDown;
    const char* lastDrop_    = "";
    uint32_t lastAttemptMs_  = 0;
    uint32_t handshakeDueMs_ = 0;
    uint32_t lastPingMs_     = 0;
    uint32_t lastRxMs_       = 0;
    uint32_t received_       = 0;

    IPAddress ip_;
    bool      resolved_ = false;
    bool      booted_   = false;

    std::vector<uint8_t> handshake_;  // raw bytes, may already contain frames

    std::vector<uint8_t> rx_;
    size_t               rxOff_ = 0;

    std::vector<uint8_t> fragments_;
    uint8_t              fragmentOpcode_ = 0;

    MessageHandler handler_    = nullptr;
    void*          handlerArg_ = nullptr;
};

}  // namespace harnessws
