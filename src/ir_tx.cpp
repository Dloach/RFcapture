#include "ir_tx.h"

#include <Arduino.h>

#include <driver/rmt.h>
#include <soc/gpio_struct.h>

namespace irtx {
namespace {

// RMT channel 0..3 are transmit-only on the ESP32-S3.  Nothing else in this
// firmware claims one: the RF transmitter is a different app (one feature per
// boot), and M5Unified only installs an RMT channel on its IDF 5 path.
constexpr rmt_channel_t kChannel = RMT_CHANNEL_0;

// 80 MHz APB / 80 -> one tick is 1 us, the same unit the timings arrive in.
constexpr uint8_t kClockDivider = 80;

// 48 words per block on the S3; four blocks stream ~380 items, enough for a
// whole A/C frame (512 mark/space pairs) without chunking.
constexpr uint8_t kMemBlocks = 4;

constexpr uint32_t kMaxTicks  = 0x7FFF;
constexpr uint32_t kApbHz     = 80000000u;

int g_pin      = -1;
bool g_ready   = false;

uint16_t clampTicks(uint32_t us)
{
    return static_cast<uint16_t>(us > kMaxTicks ? kMaxTicks : us);
}

// The carrier is programmed in raw 80 MHz source ticks, not hertz: the
// peripheral divides by (high + low).  38 kHz -> 2105 ticks (702/1403 at 33 %).
void setCarrier(bool enable, uint32_t hz, uint8_t dutyPercent)
{
    if (hz < 1000) hz = 1000;
    uint32_t total = kApbHz / hz;
    if (total < 2) total = 2;
    if (total > 0xFFFF) total = 0xFFFF;
    if (dutyPercent < 1) dutyPercent = 1;
    if (dutyPercent > 99) dutyPercent = 99;

    uint32_t high = total * dutyPercent / 100;
    if (high < 1) high = 1;
    if (high >= total) high = total - 1;
    const uint32_t low = total - high;

    rmt_set_tx_carrier(kChannel, enable, static_cast<uint16_t>(high), static_cast<uint16_t>(low),
                       RMT_CARRIER_LEVEL_HIGH);
}

}  // namespace

bool begin(int gpio)
{
    g_pin   = gpio;
    g_ready = false;

    rmt_config_t cfg      = {};
    cfg.channel           = kChannel;
    cfg.gpio_num          = static_cast<gpio_num_t>(gpio);
    cfg.clk_div           = kClockDivider;
    cfg.mem_block_num     = kMemBlocks;
    cfg.rmt_mode          = RMT_MODE_TX;
    cfg.tx_config.loop_en = false;
    // Carrier is configured here so the channel is usable immediately; the
    // frequency is reprogrammed per transmission, the protocols differ.
    cfg.tx_config.carrier_en           = true;
    cfg.tx_config.carrier_freq_hz      = 38000;
    cfg.tx_config.carrier_level        = RMT_CARRIER_LEVEL_HIGH;
    cfg.tx_config.carrier_duty_percent = 33;
    // Idle output enabled at a low level keeps the LED hard off between frames
    // and stops the pad from floating into UART0's RX input.
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;

    if (rmt_config(&cfg) != ESP_OK) {
        Serial.printf("[ir] rmt_config failed on G%d\n", gpio);
        return false;
    }
    if (rmt_driver_install(kChannel, 0, 0) != ESP_OK) {
        Serial.printf("[ir] rmt_driver_install failed\n");
        return false;
    }

    // Idle low, carrier off: nothing is emitted until a frame is queued.
    setCarrier(false, 38000, 33);
    g_ready = true;
    Serial.printf("[ir] tx ready on G%d (RMT ch%d, 38 kHz carrier in hardware)\n", gpio,
                  static_cast<int>(kChannel));
    return true;
}

void end()
{
    if (g_ready) {
        setCarrier(false, 38000, 33);
        rmt_driver_uninstall(kChannel);
        g_ready = false;
    }
    if (g_pin >= 0) {
        pinMode(g_pin, OUTPUT);
        digitalWrite(g_pin, LOW);
    }
}

bool ready() { return g_ready; }

bool send(const uint16_t* us, int count, uint32_t carrierHz, uint8_t dutyPercent, int repeats,
          uint32_t gapUs)
{
    if (!g_ready || us == nullptr || count <= 0) return false;
    if (repeats < 1) repeats = 1;

    // One RMT item carries a mark and the space after it.
    const int itemsPerCopy = (count + 1) / 2;
    const size_t totalItems = static_cast<size_t>(itemsPerCopy) * static_cast<size_t>(repeats);
    if (totalItems == 0 || totalItems > 4096) return false;

    rmt_item32_t* items = static_cast<rmt_item32_t*>(malloc(totalItems * sizeof(rmt_item32_t)));
    if (items == nullptr) return false;

    size_t n = 0;
    for (int r = 0; r < repeats; ++r) {
        for (int i = 0; i < count; i += 2) {
            rmt_item32_t it;
            it.level0    = 1;
            it.duration0 = clampTicks(us[i]);

            // A zero duration terminates the stream, so an odd trailing mark
            // gets a minimal space; the inter-frame gap is added to the last
            // space of every copy.
            uint32_t low = (i + 1 < count) ? us[i + 1] : 1u;
            if (i + 2 >= count) low += gapUs;
            it.level1    = 0;
            it.duration1 = clampTicks(low);
            if (it.duration1 == 0) it.duration1 = 1;
            items[n++] = it;
        }
    }

    setCarrier(true, carrierHz, dutyPercent);

    // rmt_config() pointed the GPIO matrix at the transmitter once; keep the pad
    // driven by RMT (do not call pinMode() here, that would steal the pad back
    // for the plain GPIO output and silence the frame -- same trap documented in
    // src/ook_tx.cpp).
    rmt_set_gpio(kChannel, RMT_MODE_TX, static_cast<gpio_num_t>(g_pin), false);
    if (g_pin < 32) {
        GPIO.enable_w1ts = (1u << g_pin);
    } else {
        GPIO.enable1_w1ts.val = (1u << (g_pin - 32));
    }
    rmt_set_idle_level(kChannel, true, RMT_IDLE_LEVEL_LOW);

    const esp_err_t err = rmt_write_items(kChannel, items, static_cast<int>(n), true);
    free(items);

    // Park the channel: carrier off, pad back to a plain low output.
    setCarrier(false, carrierHz, dutyPercent);
    rmt_set_idle_level(kChannel, true, RMT_IDLE_LEVEL_LOW);
    digitalWrite(g_pin, LOW);

    return err == ESP_OK;
}

bool carrierBurst(uint32_t ms, uint32_t carrierHz, uint8_t dutyPercent)
{
    if (!g_ready) return false;
    if (ms == 0) return false;
    if (ms > 2000) ms = 2000;

    // One item can only hold 32767 ticks (32.7 ms), so the burst is a chain of
    // 30 ms marks.  They are adjacent, so the carrier runs uninterrupted.
    constexpr uint32_t kChunkMs = 30;
    const int chunks = static_cast<int>((ms + kChunkMs - 1) / kChunkMs);
    rmt_item32_t items[70];
    if (chunks > static_cast<int>(sizeof(items) / sizeof(items[0]))) return false;

    for (int i = 0; i < chunks; ++i) {
        items[i].level0    = 1;
        items[i].duration0 = static_cast<uint16_t>(kChunkMs * 1000);
        items[i].level1    = 0;
        // A zero duration would terminate the stream; the last chunk gets a
        // minimal space, the others are bridged by the next mark.
        items[i].duration1 = (i + 1 == chunks) ? 1 : 1;
    }

    setCarrier(true, carrierHz, dutyPercent);
    rmt_set_gpio(kChannel, RMT_MODE_TX, static_cast<gpio_num_t>(g_pin), false);
    if (g_pin < 32) {
        GPIO.enable_w1ts = (1u << g_pin);
    } else {
        GPIO.enable1_w1ts.val = (1u << (g_pin - 32));
    }
    rmt_set_idle_level(kChannel, true, RMT_IDLE_LEVEL_LOW);

    const esp_err_t err = rmt_write_items(kChannel, items, chunks, true);
    setCarrier(false, carrierHz, dutyPercent);
    digitalWrite(g_pin, LOW);
    return err == ESP_OK;
}

}  // namespace irtx
