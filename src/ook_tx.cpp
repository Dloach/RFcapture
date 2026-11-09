#include "ook_tx.h"

#if RF_TX_USE_RMT
#include <driver/rmt.h>
#include <soc/gpio_struct.h>
#if RF_RX_SELFTEST
#include <soc/io_mux_reg.h>
#endif
#endif

namespace {

#if RF_TX_USE_RMT

// RMT channels 0..3 are transmit-only on the ESP32-S3.  Nothing else in this
// firmware claims one: the receiver is a plain GPIO interrupt, and M5Unified's
// LedBus_RMT only exists for the IDF 5 path (M5UNIFIED_RMT_VERSION == 2), so on
// the pinned IDF 4.4 toolchain it installs no RMT channel at all.
constexpr rmt_channel_t kTxChannel = RMT_CHANNEL_0;

// 80 MHz APB / 80 -> one tick is 1 us, the same resolution the receiver uses.
constexpr uint8_t kClockDivider = 80;

// The S3 has 48 words per memory block; four blocks let a single call stream a
// train of up to 384 pulses without hitting the chunking path.
constexpr uint8_t kMemBlocks = 4;

// A duration field is 15 bits wide.
constexpr uint32_t kMaxTicks = 0x7FFF;

uint16_t clampTicks(uint32_t us)
{
    return static_cast<uint16_t>(us > kMaxTicks ? kMaxTicks : us);
}

#endif  // RF_TX_USE_RMT

}  // namespace

bool OokTransmitter::begin(int gpio)
{
    pin_   = gpio;
    ready_ = true;
    pinMode(pin_, INPUT);  // high-Z: the CC1101 drives GDO0 while receiving

    rmtReady_ = configureRmt();

    Serial.printf("[tx] backend=%s\n", rmtReady_ ? "RMT" : "bit-bang");
    return ready_;
}

bool OokTransmitter::configureRmt()
{
#if RF_TX_USE_RMT
    rmt_config_t cfg = {};
    cfg.channel            = kTxChannel;
    cfg.gpio_num           = static_cast<gpio_num_t>(pin_);
    cfg.clk_div            = kClockDivider;
    cfg.mem_block_num      = kMemBlocks;
    cfg.rmt_mode           = RMT_MODE_TX;
    cfg.tx_config.loop_en  = false;
    cfg.tx_config.carrier_en = false;
    // Idle output stays enabled so the line is defined while the channel is
    // configured; releasePin() turns it off again after every transmission.
    cfg.tx_config.idle_output_en = true;
    cfg.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;

    if (rmt_config(&cfg) != ESP_OK) {
        return false;
    }
    if (rmt_driver_install(cfg.channel, 0, 0) != ESP_OK) {
        return false;
    }

    // rmt_config() points the GPIO matrix at the transmitter and drives the pad
    // at the idle level, which would hold GDO0 low and make the CC1101's own
    // output invisible to the receiver - capture then silently stops working.
    // Hand the pin straight back; sendRmt() takes it over again for the short
    // moment a frame is on air.
    rmt_set_idle_level(cfg.channel, false, RMT_IDLE_LEVEL_LOW);
    pinMode(pin_, INPUT);
    return true;
#else
    return false;
#endif
}

bool OokTransmitter::send(const uint16_t* pulses, int count, int repeats, uint32_t gapUs)
{
    if (pin_ < 0 || pulses == nullptr || count <= 0 || repeats <= 0) return false;

    if (rmtReady_ && sendRmt(pulses, count, repeats, gapUs)) {
        return true;
    }
    // Either RMT was never configured or the transfer failed; the software
    // backend produces the same waveform, just with coarser timing.
    return sendBitBang(pulses, count, repeats, gapUs);
}

bool OokTransmitter::sendRmt(const uint16_t* pulses, int count, int repeats, uint32_t gapUs)
{
#if RF_TX_USE_RMT
    // One RMT item holds a high pulse and the low that follows it.
    const int    itemsPerPass = (count + 1) / 2;
    const size_t totalItems   = size_t(itemsPerPass) * size_t(repeats);
    if (totalItems == 0) return false;

    rmt_item32_t* items =
        static_cast<rmt_item32_t*>(malloc(totalItems * sizeof(rmt_item32_t)));
    if (items == nullptr) return false;

    size_t n = 0;
    for (int r = 0; r < repeats; ++r) {
        for (int i = 0; i < count; i += 2) {
            rmt_item32_t it;
            it.level0    = 1;
            it.duration0 = clampTicks(pulses[i]);

            // A zero duration would terminate the stream, so an odd trailing
            // pulse gets a minimal low instead.
            uint32_t low = (i + 1 < count) ? pulses[i + 1] : 1u;
            if ((i + 2 >= count) && gapUs != 0) {
                low += gapUs;
            }
            it.level1    = 0;
            it.duration1 = clampTicks(low);
            if (it.duration1 == 0) it.duration1 = 1;

            items[n++] = it;
        }
    }

    // releasePin() left the pad as an input.  Route the RMT signal back to the
    // pad; do NOT touch the pin direction here - pinMode()/gpio_set_direction()
    // re-points the GPIO matrix at the plain GPIO output and would disconnect
    // the RMT signal, leaving the pad stuck at its idle level.
    rmt_set_gpio(kTxChannel, RMT_MODE_TX, static_cast<gpio_num_t>(pin_), false);
#if RF_RX_SELFTEST
    // Keep the input buffer alive while the RMT drives the pad, otherwise the
    // loopback test cannot observe its own transmission.
    PIN_INPUT_ENABLE(IO_MUX_GPIO15_REG);
#endif
    // That pinMode(INPUT) also cleared the output driver, so turn it back on
    // through the enable register - from the second transmission on the pad
    // would otherwise stay silent.
    if (pin_ < 32) {
        GPIO.enable_w1ts = (1u << pin_);
    } else {
        GPIO.enable1_w1ts.val = (1u << (pin_ - 32));
    }
    rmt_set_idle_level(kTxChannel, true, RMT_IDLE_LEVEL_LOW);

    const esp_err_t err = rmt_write_items(kTxChannel, items, static_cast<int>(n), true);
    free(items);

    releasePin();
    return err == ESP_OK;
#else
    (void)pulses;
    (void)count;
    (void)repeats;
    (void)gapUs;
    return false;
#endif
}

bool OokTransmitter::sendBitBang(const uint16_t* pulses, int count, int repeats, uint32_t gapUs)
{
    if (pin_ < 0) return false;

    pinMode(pin_, OUTPUT);
    digitalWrite(pin_, LOW);
    delayMicroseconds(200);

    for (int r = 0; r < repeats; ++r) {
        for (int i = 0; i < count; ++i) {
            digitalWrite(pin_, (i & 1) ? LOW : HIGH);
            delayMicroseconds(pulses[i]);
        }
        digitalWrite(pin_, LOW);
        delayMicroseconds(gapUs);
    }

    releasePin();
    return true;
}

void OokTransmitter::releasePin()
{
#if RF_TX_USE_RMT
    if (rmtReady_) {
        // Stop the transmitter from holding the line at its idle level, then
        // drop the pad to input so the CC1101 can drive GDO0 again.
        rmt_set_idle_level(kTxChannel, false, RMT_IDLE_LEVEL_LOW);
    }
#endif
    digitalWrite(pin_, LOW);
    pinMode(pin_, INPUT);
}
