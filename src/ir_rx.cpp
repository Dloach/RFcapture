#include "ir_rx.h"

#include <Arduino.h>

#include <driver/rmt.h>

namespace irrx {
namespace {

// RMT channels 4..7 receive on the ESP32-S3 (0..3 are transmit), so this never
// collides with ir_tx.cpp's channel 0 -- the LEARN page replays captures while
// it is still listening.
constexpr rmt_channel_t kChannel = RMT_CHANNEL_4;
constexpr uint8_t kClockDivider  = 80;  // 1 us per tick
constexpr uint8_t kMemBlocks     = 2;

// Glitches shorter than this are the module's own noise floor.
constexpr uint16_t kFilterTicks = 40;

// Ring buffer for captured items; 2 kB is a couple of frames of slack.
constexpr size_t kRingSize = 2048;

int g_pin          = -1;
bool g_ready       = false;
RingbufHandle_t g_rb = nullptr;
uint32_t g_lastEdge = 0;

// Accumulated frame: alternating mark/space, starting with a mark.
uint16_t g_frame[512];
int g_count         = 0;
bool g_frameTooBig  = false;

void resetFrame()
{
    g_count        = 0;
    g_frameTooBig  = false;
}

// Append one run, merging with the previous one when the level repeats (the
// RMT splits a long run at memory-block boundaries, so this does happen).
void appendRun(bool mark, uint32_t us)
{
    if (us == 0) return;

    // A leading space is the idle before the first mark: not part of the frame.
    if (g_count == 0 && !mark) {
        return;
    }
    if (g_count > 0) {
        const bool prevMark = (g_count % 2) == 1;
        if (prevMark == mark) {
            const uint32_t sum = static_cast<uint32_t>(g_frame[g_count - 1]) + us;
            g_frame[g_count - 1] = static_cast<uint16_t>(sum > 0xFFFFu ? 0xFFFFu : sum);
            return;
        }
    }
    if (g_count >= static_cast<int>(sizeof(g_frame) / sizeof(g_frame[0]))) {
        g_frameTooBig = true;
        return;
    }
    g_frame[g_count++] = static_cast<uint16_t>(us > 0xFFFFu ? 0xFFFFu : us);
}

}  // namespace

bool begin(int pin, int idleMs)
{
    end();
    g_pin   = pin;
    g_ready = false;

    if (pin < 0) return false;
    if (idleMs < 5) idleMs = 5;
    if (idleMs > 200) idleMs = 200;

    pinMode(pin, INPUT_PULLUP);

    rmt_config_t cfg     = {};
    cfg.channel          = kChannel;
    cfg.gpio_num         = static_cast<gpio_num_t>(pin);
    cfg.clk_div          = kClockDivider;
    cfg.mem_block_num    = kMemBlocks;
    cfg.rmt_mode         = RMT_MODE_RX;
    cfg.rx_config.filter_en        = true;
    cfg.rx_config.filter_ticks_thresh = kFilterTicks;
    // idle_threshold is in ticks: a real gap this long ends the capture, and
    // poll() then hands the frame over.
    cfg.rx_config.idle_threshold = static_cast<uint16_t>(idleMs * 1000);

    if (rmt_config(&cfg) != ESP_OK) {
        Serial.printf("[ir] rx rmt_config failed on G%d\n", pin);
        return false;
    }
    if (rmt_driver_install(kChannel, kRingSize, 0) != ESP_OK) {
        Serial.printf("[ir] rx rmt_driver_install failed\n");
        return false;
    }
    if (rmt_get_ringbuf_handle(kChannel, &g_rb) != ESP_OK || g_rb == nullptr) {
        rmt_driver_uninstall(kChannel);
        return false;
    }
    resetFrame();
    rmt_rx_start(kChannel, true);

    g_ready = true;
    Serial.printf("[ir] rx ready on G%d (RMT ch%d, %d ms idle threshold)\n", pin,
                  static_cast<int>(kChannel), idleMs);
    return true;
}

void end()
{
    if (g_ready) {
        rmt_rx_stop(kChannel);
        rmt_driver_uninstall(kChannel);
        g_rb = nullptr;
        g_ready = false;
    }
    g_pin = -1;
}

bool ready() { return g_ready; }

int pin() { return g_pin; }

uint32_t lastEdgeMs() { return g_lastEdge; }

void flush()
{
    resetFrame();
    if (!g_ready || g_rb == nullptr) return;
    size_t size = 0;
    while (true) {
        void* item = xRingbufferReceive(g_rb, &size, 0);
        if (item == nullptr) break;
        vRingbufferReturnItem(g_rb, item);
    }
}

bool poll(uint16_t* out, int maxCount, int* outCount)
{
    if (outCount != nullptr) *outCount = 0;
    if (!g_ready || g_rb == nullptr || out == nullptr || maxCount < 4) return false;

    // Drain everything the hardware queued, folding the runs into one frame.
    size_t size = 0;
    while (true) {
        rmt_item32_t* items = static_cast<rmt_item32_t*>(xRingbufferReceive(g_rb, &size, 0));
        if (items == nullptr) break;
        const int n = static_cast<int>(size / sizeof(rmt_item32_t));
        for (int i = 0; i < n; ++i) {
            const rmt_item32_t& it = items[i];
            // The receiver output is active low: level 0 is the carrier (mark).
            if (it.duration0 != 0) {
                g_lastEdge = millis();
                appendRun(it.level0 == 0, it.duration0);
            }
            if (it.duration1 != 0) {
                appendRun(it.level1 == 0, it.duration1);
            }
            // The idle threshold ends the capture: the last item's space is the
            // inter-frame gap, and everything before it is one frame.
            const bool gap = (it.level0 != 0 && it.duration0 >= kFrameGapUs) ||
                             (it.level1 != 0 && it.duration1 >= kFrameGapUs);
            if (gap && g_count >= 4) {
                vRingbufferReturnItem(g_rb, items);
                items = nullptr;
                break;
            }
        }
        if (items != nullptr) vRingbufferReturnItem(g_rb, items);

        if (g_count >= 4) {
            // Hand the frame over; anything longer than the caller's buffer is
            // still reported truncated so the UI can say so.
            const int count = g_count < maxCount ? g_count : maxCount;
            for (int i = 0; i < count; ++i) out[i] = g_frame[i];
            if (outCount != nullptr) *outCount = count;
            const bool tooBig = g_frameTooBig;
            resetFrame();
            return !tooBig;
        }
        // A partial frame is still arriving; keep the runs for the next call.
        if (g_count > 0 && g_frameTooBig) resetFrame();
    }
    return false;
}

}  // namespace irrx
