/*
 * Capture of the demodulated OOK envelope from the CC1101 GDO0 pin.
 *
 * The line is sampled by a GPIO change interrupt and timestamped with micros().
 * An earlier version used the RMT peripheral, but its ping-pong buffer overflows
 * whenever the demodulator outputs a continuous noise/clock stream (which happens
 * at 433 MHz on this module): the driver then spams "RX buffer too small" and no
 * frame is ever delivered.  A plain interrupt plus a software ring buffer keeps
 * working under that condition - excess edges are simply dropped, and the frame
 * assembler below only closes a frame when the line actually goes idle.
 *
 * Interrupt jitter is a couple of microseconds, far below the 300-500 us pulses
 * of the EV1527 / PT2262 remotes this firmware targets.
 */

#pragma once

#include <Arduino.h>

#include "rf_config.h"
#include "rf_frame.h"

class OokReceiver {
public:
    bool begin(int gpio);
    void end();
    bool ready() const { return installed_; }

    /// Release the antenna pin before the transmitter drives it.
    void suspend();
    /// Take the pin back and start capturing again.
    void resume();

    /// Non blocking.  Returns true when a complete burst was assembled.
    bool poll(RfFrame& out);

    uint32_t pulseCount() const { return pulses_; }
    uint32_t droppedEdges() const { return dropped_; }
    /// Diagnostics: is the input detached, and what is pending?
    bool suspended() const { return suspended_; }
    /// Raw ISR activity, independent of the noise filter in push().
    uint32_t isrEdges() const { return edgeIsr_; }
    uint16_t isrMinDelta() const { return minDelta_; }
    int pending() const { return current_.count; }

private:
    static void IRAM_ATTR isrThunk();

    void IRAM_ATTR onEdge();
    /// Current pad level, read straight from the GPIO input register.
    uint8_t IRAM_ATTR readPin() const;
    void push(uint16_t us, uint8_t level);
    void closeFrame(bool truncated);

    static constexpr int kQueueLen = 4;
    static constexpr int kRingSize = 512;  // must stay a power of two
    static constexpr uint32_t kRingMask = kRingSize - 1;

    // Written by the ISR, drained by poll().
    volatile uint16_t ringUs_[kRingSize];
    volatile uint8_t ringLevel_[kRingSize];
    volatile uint32_t head_    = 0;
    volatile uint32_t tail_    = 0;
    volatile uint32_t lastUs_  = 0;
    volatile uint8_t level_    = 0;
    volatile uint32_t dropped_ = 0;

    bool installed_ = false;
    bool suspended_ = false;
    int gpio_       = -1;

    RfFrame current_;
    RfFrame queue_[kQueueLen];
    int qHead_  = 0;
    int qCount_ = 0;

    volatile uint32_t edgeIsr_  = 0;
    volatile uint16_t minDelta_ = 0xFFFF;

    bool mergeNext_ = false;
    uint32_t pulses_ = 0;
};
