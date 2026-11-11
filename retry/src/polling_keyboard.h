#pragma once

#include <M5Cardputer.h>
#include <utility/Adafruit_TCA8418/Adafruit_TCA8418.h>

#include <algorithm>
#include <memory>

#include "rf_config.h"

// Diagnostics shared with the heartbeat: how many FIFO events the chip handed
// over and how long ago the last one arrived.  A frozen UI with ev frozen means
// the TCA8418 (or its I2C link) went quiet, not the app.
inline uint32_t& kbdEventCount()
{
    static uint32_t value = 0;
    return value;
}

inline uint32_t& kbdLastEventMs()
{
    static uint32_t value = 0;
    return value;
}

// Polling replacement for M5Cardputer's bundled TCA8418 keyboard reader.
//
// The stock reader is edge-driven: a GPIO ISR on the TCA8418 /INT line sets a
// flag and update() only advances while that flag is set. /INT is a level, not
// a pulse, so once an edge is missed (or an unused matrix pin latches it), the
// flag sticks low and the 10-deep key FIFO fills up - the keyboard dies within a
// handful of presses while the main loop keeps running. Draining the FIFO by
// polling instead removes that failure mode entirely.
class PollingKeyboardReader : public KeyboardReader {
public:
    void begin() override
    {
        _tca.reset(new Adafruit_TCA8418());
        _tca->begin();
        _tca->matrix(7, 8);
        _tca->flush();

        // Keep key-event capture; drop GPIO pin interrupts, which only fire on
        // the three matrix pins the 7x8 layout leaves unused.
        _tca->enableInterrupts();
        const uint8_t cfg = _tca->readRegister8(TCA8418_REG_CFG);
        _tca->writeRegister8(TCA8418_REG_CFG, cfg & ~TCA8418_REG_CFG_GPI_IEN);
        _tca->writeRegister8(TCA8418_REG_GPIO_INT_EN_1, 0x00);
        _tca->writeRegister8(TCA8418_REG_GPIO_INT_EN_2, 0x00);
        _tca->writeRegister8(TCA8418_REG_GPIO_INT_EN_3, 0x00);
        _tca->writeRegister8(TCA8418_REG_INT_STAT, 0x03);
#if RF_DIAG
        Serial.printf("[kbd] polling reader active cfg=0x%02X\n",
                      static_cast<unsigned>(_tca->readRegister8(TCA8418_REG_CFG)));
#endif
    }

    void update() override
    {
        // Drain the FIFO with getEvent() until it reports empty, the same way the
        // library's own flush() does - available() proved unreliable here.
        bool drained = false;
        for (int i = 0; i < 32; ++i) {
            const uint8_t event = _tca->getEvent();
            if (event == 0) break;
            kbdEventCount()++;
            kbdLastEventMs() = millis();
#if RF_DIAG
            Serial.printf("[ev] 0x%02X\n", static_cast<unsigned>(event));
#endif
            apply(decode(event));
            drained = true;
        }
        if (drained) _tca->writeRegister8(TCA8418_REG_INT_STAT, 0x03);
    }

private:
    struct Event {
        bool state;
        uint8_t row;
        uint8_t col;
    };

    std::unique_ptr<Adafruit_TCA8418> _tca;

    // Decode + remap copied verbatim from the stock TCA8418KeyboardReader so the
    // resulting points match _key_value_map exactly.
    static Event decode(uint8_t raw)
    {
        Event e;
        e.state         = raw & 0x80;
        uint16_t buffer = raw & 0x7F;
        buffer--;
        e.row = static_cast<uint8_t>(buffer / 10);
        e.col = static_cast<uint8_t>(buffer % 10);

        uint8_t col = static_cast<uint8_t>(e.row * 2);
        if (e.col > 3) col++;
        const uint8_t row = static_cast<uint8_t>((e.col + 4) % 4);
        e.row             = row;
        e.col             = col;
        return e;
    }

    void apply(const Event& e)
    {
        Point2D_t point;
        point.x = e.col;
        point.y = e.row;

        auto it = std::find(_key_list.begin(), _key_list.end(), point);
        if (e.state) {
            if (it == _key_list.end()) _key_list.push_back(point);
        } else if (it != _key_list.end()) {
            _key_list.erase(it);
        }
    }
};
