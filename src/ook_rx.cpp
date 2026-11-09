#include "ook_rx.h"

#include <soc/gpio_struct.h>

namespace {
OokReceiver* g_activeReceiver = nullptr;
}

// Direct register read, not digitalRead(): this runs from an IRAM_ATTR ISR, and
// digitalRead() lives in flash, so calling it while a flash write has the cache
// disabled (any NVS commit) would fault.
uint8_t IRAM_ATTR OokReceiver::readPin() const
{
    return gpio_ < 32 ? static_cast<uint8_t>((GPIO.in >> gpio_) & 1u)
                      : static_cast<uint8_t>((GPIO.in1.data >> (gpio_ - 32)) & 1u);
}

void IRAM_ATTR OokReceiver::isrThunk()
{
    if (g_activeReceiver) g_activeReceiver->onEdge();
}

bool OokReceiver::begin(int gpio)
{
    if (installed_) return true;

    gpio_      = gpio;
    installed_ = true;
    suspended_ = false;

    head_ = tail_ = 0;
    pulses_       = 0;
    qHead_        = 0;
    qCount_       = 0;
    current_.count = 0;

    pinMode(gpio_, INPUT);
    level_  = readPin();
    lastUs_ = micros();

    g_activeReceiver = this;
    attachInterrupt(digitalPinToInterrupt(gpio_), isrThunk, CHANGE);
    return true;
}

void OokReceiver::end()
{
    if (!installed_) return;
    detachInterrupt(digitalPinToInterrupt(gpio_));
    g_activeReceiver = nullptr;
    installed_       = false;
}

void OokReceiver::suspend()
{
    if (!installed_ || suspended_) return;
    detachInterrupt(digitalPinToInterrupt(gpio_));
    suspended_ = true;
}

void OokReceiver::resume()
{
    if (!installed_ || !suspended_) return;

    // Drop whatever was captured while the transmitter owned the pin.
    head_      = tail_ = 0;
    current_.count = 0;
    level_     = readPin();
    lastUs_    = micros();
    suspended_ = false;
    attachInterrupt(digitalPinToInterrupt(gpio_), isrThunk, CHANGE);
}

void IRAM_ATTR OokReceiver::onEdge()
{
    edgeIsr_++;
    const uint32_t now = micros();
    const uint8_t current = readPin();
    {
        const uint32_t d = now - lastUs_;
        if (d < minDelta_) minDelta_ = static_cast<uint16_t>(d > 0xFFFF ? 0xFFFF : d);
    }
    // `delta` is how long the level that just ended was held, so the level that
    // goes with it is the OPPOSITE of what the pin reads now; storing the new
    // level here would label every pulse with the next segment's level and the
    // decoder would never find the long sync LOW.
    //
    // The level is read from the pin rather than toggled from a cached flag.
    // Toggling assumes every interrupt corresponds to exactly one real
    // transition, and one missed edge then inverts every label for the rest of
    // the session - the stored train stops alternating, decodeFrame() never
    // finds a HIGH to start from, and wholeFrameRange() cuts at the wrong place
    // because it reconstructs levels from the array parity.  Reading the pin
    // makes a missed or spurious edge cost at most one pulse, which push() then
    // merges away as noise.
    const uint8_t endedLevel = current ? 0 : 1;
    uint32_t delta           = now - lastUs_;
    lastUs_                  = now;
    level_                   = current;

    if (delta > 0xFFFF) delta = 0xFFFF;

    const uint32_t next = (head_ + 1) & kRingMask;
    if (next == tail_) {
        dropped_++;  // consumer too slow, drop this edge
        return;
    }
    ringUs_[head_]    = static_cast<uint16_t>(delta);
    ringLevel_[head_] = endedLevel;
    head_             = next;
}

void OokReceiver::push(uint16_t us, uint8_t level)
{
    // A short spike is a pair of edges: the spike itself and the continuation of
    // the level it interrupted.  Both have to be folded back into the pulse they
    // split, otherwise the stored train stops alternating high/low and the
    // decoder can no longer pair the pulses.
    if (mergeNext_) {
        mergeNext_ = false;
        if (current_.count > 0) {
            uint32_t merged = uint32_t(current_.pulses[current_.count - 1].us) + us;
            current_.pulses[current_.count - 1].us = merged > 0xFFFF ? 0xFFFF : uint16_t(merged);
        }
        return;
    }

    if (us < rfcfg::noiseUs) {
        if (current_.count > 0) {
            uint32_t merged = uint32_t(current_.pulses[current_.count - 1].us) + us;
            current_.pulses[current_.count - 1].us = merged > 0xFFFF ? 0xFFFF : uint16_t(merged);
            mergeNext_ = true;
        }
        return;
    }

    // A long low that has just ended marks the end of a transmission: the next
    // level after it is the carrier of the following burst.  The duration of
    // that low is `us` - looking at the previously stored pulse instead (as this
    // used to) tests the wrong interval and the frame never closes.
    if (current_.count >= 8 && level == 0 && us >= rfcfg::frameGapUs) {
        closeFrame(false);
    }

    if (current_.count >= rfcfg::maxFramePulses) {
        closeFrame(true);
    }

    current_.pulses[current_.count].us    = us;
    current_.pulses[current_.count].level = level;
    current_.count++;
    pulses_++;
}

void OokReceiver::closeFrame(bool truncated)
{
    if (current_.count < 4) {
        current_.count = 0;
        return;
    }

    current_.truncated = truncated;
    current_.atMs      = millis();

    if (qCount_ == kQueueLen) {
        qHead_ = (qHead_ + 1) % kQueueLen;  // drop the oldest pending frame
        qCount_--;
        dropped_++;
    }
    queue_[(qHead_ + qCount_) % kQueueLen] = current_;
    qCount_++;

    current_.count     = 0;
    current_.truncated = false;
    mergeNext_         = false;
}

bool OokReceiver::poll(RfFrame& out)
{
    if (!installed_ || suspended_) return false;


    // Drain the edge ring into the frame assembler, with a hard cap so a noisy
    // line can never starve the UI.
    for (int n = 0; n < 256; ++n) {
        const uint32_t tail = tail_;
        if (tail == head_) break;
        const uint16_t us  = ringUs_[tail];
        const uint8_t lvl  = ringLevel_[tail];
        tail_              = (tail + 1) & kRingMask;
        push(us, lvl);
    }

    // Releasing the remote produces no further edge, so the last burst would sit
    // half assembled forever (it used to be emitted only on the next press, or
    // when it hit RF_MAX_FRAME_PULSES).  Close it once the line has been quiet
    // for longer than the frame gap.
    if (current_.count >= 8) {
        // `level_` is the level the line is in RIGHT NOW, and lastUs_ is when it
        // started, so this is the duration of the pulse currently in progress.
        // Looking at the last *stored* pulse instead does not work: a burst ends
        // on a data low that is still in progress and therefore not stored yet,
        // so the previous entry is a HIGH and the test never fires.
        if (level_ == 0 && (micros() - lastUs_) >= rfcfg::frameGapUs) {
            closeFrame(false);
        }
    }

    if (qCount_ == 0) return false;

    out    = queue_[qHead_];
    qHead_ = (qHead_ + 1) % kQueueLen;
    qCount_--;
    return true;
}
