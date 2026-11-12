#include "app.h"

#include <cstdio>
#include <cstring>
#include <esp_task_wdt.h>
#include <memory>

#include "launcher.h"
#include "polling_keyboard.h"
#include "bind_picker.h"
#include "quick_bind.h"
#include "rf_replay.h"
#include "trace.h"
#include "tf_backup.h"

namespace {

constexpr uint16_t kColBg     = 0x0000;
constexpr uint16_t kColText   = 0xFFFF;
constexpr uint16_t kColDim    = 0x8410;
constexpr uint16_t kColAccent = 0xFD20;
constexpr uint16_t kColOk     = 0x07E0;
constexpr uint16_t kColWarn   = 0xF800;
constexpr uint16_t kColBar    = 0x2104;
constexpr uint16_t kColHead   = 0x18E3;
constexpr uint16_t kColBox    = 0x2124;

constexpr int kHeaderH = 13;
constexpr int kFooterY = 118;
constexpr int kRowH    = 15;
constexpr int kRssiY   = 13;
constexpr int kListY   = 22;
constexpr int kRows    = 6;
// Does a burst look like a real OOK frame rather than the demodulator chattering?
// A remote's pulses are only 1T and 3T wide (plus the 31T sync), so nearly every
// pulse must fall into one of those two buckets.  Noise has random widths and
// fails this immediately, which keeps the capture list from filling up with junk
// the moment an undecodable frame is accepted.
bool looksLikeOok(const RfFrame& f)
{
    if (f.count < 24) return false;

    // estimateBaseUs() rather than a plain minimum: a single surviving noise
    // spike would drag the minimum down and make every 1T/3T window miss.
    const uint16_t minUs = estimateBaseUs(f);
    if (minUs < 100 || minUs > 2000) return false;

    int total   = 0;
    int good    = 0;
    bool hasSync = false;
    for (int i = 0; i < f.count; ++i) {
        const uint32_t u = f.pulses[i].us;
        // A long low is the sync gap.  Without one the burst has no frame
        // boundary at all - replaying it cannot work, so it is not worth
        // storing (those entries are the ones that "cannot be used").
        if (f.pulses[i].level == 0 && u >= 10u * minUs) hasSync = true;
        if (u >= 6u * minUs) continue;  // the sync low, which is expected to differ
        ++total;
        if (u * 10 >= uint32_t(minUs) * 6 && u * 100 <= uint32_t(minUs) * 145) {
            ++good;  // ~1T
        } else if (u * 10 >= uint32_t(minUs) * 24 && u * 100 <= uint32_t(minUs) * 360) {
            ++good;  // ~3T
        }
    }
    return hasSync && total >= 20 && good * 100 >= total * 80;
}

constexpr int kHomeItems = 3;

}  // namespace

void App::begin()
{
    Serial.begin(115200);
    delay(30);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    // M5Cardputer ADV keyboard: the stock reader is edge-driven off the TCA8418
    // /INT line, and a missed edge (or a GPIO interrupt on an unused matrix pin)
    // latches it low, so the 10-deep key FIFO fills and the keyboard dies after a
    // handful of presses while the main loop keeps running. Replace it with a
    // polling reader. Detach the default reader's ISR first so destroying it
    // leaves no dangling handler on GPIO 11.
    detachInterrupt(digitalPinToInterrupt(RF_KBD_INT));
    M5Cardputer.Keyboard.begin(std::unique_ptr<KeyboardReader>(new PollingKeyboardReader()));

    auto& display = M5Cardputer.Display;
    display.setRotation(rfcfg::screenRotation);  // landscape 240x135
    display.setBrightness(90);

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(0);
#endif

    canvas_.setColorDepth(16);
    if (canvas_.createSprite(display.width(), display.height()) == nullptr) {
        canvas_.setColorDepth(8);
        canvas_.createSprite(display.width(), display.height());
    }
    canvas_.setTextFont(1);
    canvas_.setTextSize(1);
    canvas_.setTextWrap(false);

    M5.Speaker.setVolume(48);

    // The CC1101 outputs a clock on GDO0 until it is configured, which would
    // flood the receiver with bogus edges - configure it first.
    if (!rf_.begin()) {
        Serial.println("[cc1101] NOT DETECTED - radio features disabled");
    }
    // Historical: the receiver used to be RMT based and its ISR level logging
    // could not be silenced at runtime.  No RMT driver is installed any more, so
    // this is a harmless no-op kept in case an RMT receiver ever comes back.
    esp_log_level_set("rmt", ESP_LOG_NONE);
    rx_.begin(rfcfg::gdo0Pin);
    tx_.begin(rfcfg::gdo0Pin);

    store_.begin();
    saved_.begin();

    applyBand();
    rebuildView();

#if RF_DIAG
    // Dump the permanently saved signals so a bad capture can be recognised
    // from the pulse train alone (this is what docs/signals-*.log is made of).
    for (int i = 0; i < saved_.size(); ++i) {
        const SavedSignal& s = saved_.at(i);
        Serial.printf("[saved] #%d '%s' band=%s code=%06lX bits=%u T=%u n=%u:",
                      i, s.name, rfcfg::bandName(s.band),
                      static_cast<unsigned long>(s.code & 0xFFFFFFu),
                      static_cast<unsigned>(s.bits), static_cast<unsigned>(s.baseUs),
                      static_cast<unsigned>(s.count));
        for (int k = 0; k < s.count; ++k) {
            Serial.printf(" %u", static_cast<unsigned>(s.pulses[k]));
        }
        Serial.println();
    }

    for (int i = 0; i < store_.size(); ++i) {
        const Capture& c = store_.at(i);
        Serial.printf("[capd] #%d band=%s code=%06lX bits=%u T=%u n=%u:",
                      i, rfcfg::bandName(c.band), static_cast<unsigned long>(c.code & 0xFFFFFFu),
                      static_cast<unsigned>(c.bits), static_cast<unsigned>(c.baseUs),
                      static_cast<unsigned>(c.count));
        for (int k = 0; k < c.count; ++k) {
            Serial.printf(" %u", static_cast<unsigned>(c.pulses[k]));
        }
        Serial.println();
    }
#endif

    Serial.printf("[app] band=%sMHz captured=%d saved=%d rx=%d cc1101=%d\n",
                  rfcfg::bandName(band_), viewCount_, saved_.size(), rx_.ready() ? 1 : 0,
                  rf_.ready() ? 1 : 0);

    dirty_ = true;
    render();

#if RF_DIAG
    // Panic instead of silently hanging: a backtrace is far more useful than a
    // dead screen, and it tells us whether a "freeze" is the main loop blocking
    // or just the UI.
    esp_task_wdt_deinit();
    if (esp_task_wdt_init(6, true) == ESP_OK) {
        esp_task_wdt_add(nullptr);
    }
#endif
}

void App::update()
{
#if RF_DIAG
    esp_task_wdt_reset();

    // Time the two phases that can block the loop.  A "freeze" shows up here as
    // one phase taking hundreds of ms.
    const uint32_t kbT0 = micros();
#endif

    M5Cardputer.update();

#if RF_DIAG
    const uint32_t kbUs = micros() - kbT0;
    if (kbUs > 100000) Serial.printf("[slow] kbd %luus\n", static_cast<unsigned long>(kbUs));
    const uint32_t rxT0 = micros();
#endif

    pollReceiver();

#if RF_DIAG
    const uint32_t rxUs = micros() - rxT0;
    if (rxUs > 100000) Serial.printf("[slow] rx %luus\n", static_cast<unsigned long>(rxUs));
#endif

    if (mode_ == Mode::kRename) {
        handleRenameInput();
    } else {
        actions_ = readActions();
        const uint32_t pressed = actions_ & ~prevActions_;
        prevActions_           = actions_;
        if (pressed) {
#if RF_DIAG
            Serial.printf("[key] act=0x%lX held=%u\n", static_cast<unsigned long>(pressed),
                          static_cast<unsigned>(M5Cardputer.Keyboard.keyList().size()));
            Serial.printf("[act] 0x%lX scr=%d mode=%d sel=%d\n",
                          static_cast<unsigned long>(pressed), static_cast<int>(screen_),
                          static_cast<int>(mode_), homeSel_);
#endif
            handleAction(pressed);
        }
    }

    const uint32_t now = millis();

    // Post-mortem from the previous run, once.  Deliberately not on the first
    // loop iteration: USB CDC has not enumerated yet and the dump would be lost.
    // Not behind RF_DIAG - this is the only output that survives a panic.
    static bool traceDumped = false;
    if (!traceDumped && now > 1500) {
        traceDumped = true;
        trace::dumpAfterReset();
    }

    // Batched hit-counter write.  An NVS commit disables the flash cache, so
    // this is only safe because the receiver ISR reads the pad straight from
    // the GPIO register instead of calling digitalRead() - see
    // OokReceiver::readPin().
    store_.flush();

#if RF_GDO_TEST
    {
        static bool gdoTestDone = false;
        if (!gdoTestDone && now > 2500) {
            gdoTestDone = true;
            runGdoTest();
        }
    }
#endif

#if RF_RX_SELFTEST
    // Loopback bring-up aid: a few seconds after boot, transmit a known EV1527
    // frame with the RMT backend and decode it back through the interrupt
    // receiver.  Needs no remote and no radio link - it verifies the whole
    // capture chain (edges -> ring -> framing -> decode).
    static bool rxTestDone = false;
    if (!rxTestDone && now > 3000) {
        rxTestDone = true;
        const uint16_t t    = 501;          // a typical EV1527 base time
        const uint32_t code = 0x123456;     // placeholder, not a real remote

        uint16_t train[64];
        int n = 0;
        train[n++] = t;
        train[n++] = 31 * t;
        for (int i = 23; i >= 0; --i) {
            const bool one = ((code >> i) & 1u) != 0;
            train[n++] = one ? 3 * t : t;
            train[n++] = one ? t : 3 * t;
        }

        Serial.printf("[rxst] tx %d pulses x6 back to back\n", n);
        rf_.enterTx();
        delay(3);
        tx_.send(train, n, 6, 0);
        delay(40);   // let the idle timeout close the burst

        RfFrame f;
        int shown  = 0;
        int frames = 0;
        // poll() drains at most 256 edges per call, and the burst is only closed
        // once the line has been idle for a while - so keep polling across a few
        // tens of milliseconds instead of stopping at the first empty call.
        for (int iter = 0; iter < 30; ++iter) {
            while (rx_.poll(f)) {
            const Decoded d = decodeFrame(f);
            frames++;
            if (shown < 3) {
                Serial.printf("[rxst] frame n=%u first8:", static_cast<unsigned>(f.count));
                for (int i = 0; i < 8 && i < f.count; ++i) {
                    Serial.printf(" %u/%u", static_cast<unsigned>(f.pulses[i].us),
                                  static_cast<unsigned>(f.pulses[i].level));
                }
                Serial.printf(" -> valid=%d bits=%u code=%06lX\n", d.valid ? 1 : 0,
                              static_cast<unsigned>(d.bits),
                              static_cast<unsigned long>(d.code & 0xFFFFFFu));
                shown++;
            }
            }
            if (frames > 0 && iter > 6) break;
            delay(4);
        }
        Serial.printf("[rxst] frames=%d pulses=%lu (expect code 123456)\n", frames,
                      static_cast<unsigned long>(rx_.pulseCount()));
        rf_.enterRx();
        applyBand();
    }
#endif

#if RF_SCAN_SELFTEST
    // Bring-up aid: sweep the scanner table once a few seconds after boot and
    // print the levels, so tuning and the RSSI conversion can be checked without
    // touching the UI.
    static bool scanTestDone = false;
    if (!scanTestDone && now > 3000) {
        scanTestDone = true;
        rx_.suspend();
        Serial.println("[scan] selftest");
        for (int i = 0; i < rfcfg::kScanCount; ++i) {
            const rfcfg::ScanPoint& pt = rfcfg::kScanPoints[i];
            rf_.tuneTo(pt.band, pt.hz);
            delay(2);
            Serial.printf("[scan] %u.%02u MHz -> %d dBm\n",
                          static_cast<unsigned>(pt.hz / 1000000u),
                          static_cast<unsigned>((pt.hz % 1000000u) / 10000u), rf_.rssiDbm());
        }
        applyBand();
        rx_.resume();
    }
#endif

#if RF_DIAG
    // 1 Hz blink in the header so a frozen picture is distinguishable from a
    // live one.  This also forces a full redraw every second, which is why the
    // keyboard freeze is not currently visible: removing it is the experiment
    // described in docs/DEV-STATE.md section 4.1.
    if (now - lastTickMs_ >= 1000) {
        lastTickMs_ = now;
        tick_       = !tick_;
        dirty_      = true;
    }
#endif

    if (now - lastBatteryMs_ > 3000) {
        lastBatteryMs_ = now;
        const int level = M5.Power.getBatteryLevel();
        if (level != batteryLevel_) {
            batteryLevel_ = level;
            dirty_        = true;
        }
    }

    // Signal strength.  Sampled a few times a second so the capture page can show
    // whether a remote reaches us at all (aim, angle, walls) or is simply too far
    // away - much more useful than guessing why a send did not work.
    if (screen_ == Screen::kScan) {
        scanTick(now);
    } else if (now - lastRssiMs_ > 200) {
        lastRssiMs_ = now;
        const int dbm = rf_.rssiDbm();
        if (dbm != rssiDbm_) {
            rssiDbm_ = dbm;
            dirty_   = true;
        }
        if (dbm > rssiPeak_) {
            rssiPeak_   = dbm;
            rssiPeakMs_ = now;
        } else if (now - rssiPeakMs_ > 3000) {
            rssiPeak_   = dbm;  // let the marker drift back down
            rssiPeakMs_ = now;
        }
    }

    if (toastActive_ && static_cast<int32_t>(now - toastUntil_) >= 0) {
        toastActive_ = false;
        dirty_       = true;
    }
    if (clearArmed_ && now - clearArmedAt_ > 4000) {
        clearArmed_ = false;
        dirty_      = true;
    }
    if (restoreArmed_ && now - restoreArmedAt_ > 4000) {
        restoreArmed_ = false;
        dirty_        = true;
    }

    if (dirty_ && now - lastDrawMs_ >= 33) {
        render();
        dirty_      = false;
        lastDrawMs_ = now;
    }

#if RF_DIAG
    if (now - lastHeartbeatMs_ > 5000) {
        lastHeartbeatMs_ = now;
        const uint8_t tcaCfg = m5::In_I2C.readRegister8(0x34, 0x01, 400000);  // CFG
        const uint8_t tcaInt = m5::In_I2C.readRegister8(0x34, 0x02, 400000);  // INT_STAT
        if (trace::takeDirty()) {
            trace::dump();
        }
        Serial.printf(
            "[hb] up=%lus scr=%d mode=%d sel=%d band=%s captured=%d saved=%d rejected=%lu "
            "keys=%u acts=0x%lX ev=%lu evAge=%lums cfg=0x%02X int=0x%02X intpin=%d "
            "heap=%u min=%u loops=%lu rssi=%d susp=%d rp=%lu rd=%lu pend=%d ms=0x%02X iocfg0=0x%02X gdo=%d isr=%lu mind=%u\n",
            static_cast<unsigned long>(now / 1000), static_cast<int>(screen_),
            static_cast<int>(mode_), homeSel_, rfcfg::bandName(band_), viewCount_, saved_.size(),
            static_cast<unsigned long>(rejected_),
            static_cast<unsigned>(M5Cardputer.Keyboard.keyList().size()),
            static_cast<unsigned long>(actions_), static_cast<unsigned long>(kbdEventCount()),
            kbdLastEventMs() ? static_cast<unsigned long>(now - kbdLastEventMs()) : 0ul,
            static_cast<unsigned>(tcaCfg), static_cast<unsigned>(tcaInt), digitalRead(RF_KBD_INT),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
            static_cast<unsigned long>(loops_), rssiDbm_, rx_.suspended() ? 1 : 0,
            static_cast<unsigned long>(rx_.pulseCount()),
            static_cast<unsigned long>(rx_.droppedEdges()), rx_.pending(),
            static_cast<unsigned>(rf_.marcState()), static_cast<unsigned>(rf_.iocfg0()),
            digitalRead(rfcfg::gdo0Pin),
            static_cast<unsigned long>(rx_.isrEdges()),
            static_cast<unsigned>(rx_.isrMinDelta()));
    }
#endif

    delay(2);
    loops_++;
}

uint32_t App::readActions()
{
    uint32_t a = 0;

    for (const auto& pos : M5Cardputer.Keyboard.keyList()) {
        const uint8_t code =
            static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first);
        switch (code) {
            // Orange arrow legends.  Left/right move between horizontally laid
            // out choices (the YES / NO buttons); they must NOT mean back/enter -
            // everyone tries to move onto "NO" with the right arrow, and doing
            // that used to confirm instead.
            case ';': case 'k': case 'K': a |= kActUp; break;
            case '.': case 'j': case 'J': a |= kActDown; break;
            case ',': a |= kActLeft; break;
            case '/': a |= kActRight; break;
            // The key in the top left corner is labelled "esc" - it reports a
            // backtick, so treat it as ESC directly (no Fn needed).
            case '`': case '~': a |= kActBack; break;
            case 0x1B: a |= kActBack; break;  // in case a future map reports ESC
            case 's': case 'S': a |= kActSend; break;
            case 'f': case 'F': a |= kActBand; break;
            case 'd': case 'D': a |= kActDelete; break;
            case 'c': case 'C': a |= kActClear; break;
            case 'n': case 'N': a |= kActRename; break;
            case 'b': case 'B': a |= kActBind; break;
            case KEY_ENTER: a |= kActEnter; break;
            case KEY_BACKSPACE: a |= kActBack; break;
            default: break;
        }
    }
    return a;
}

void App::handleAction(uint32_t action)
{
    // ---- popup: confirm save / delete --------------------------------------
    if (mode_ == Mode::kConfirm) {
        // The buttons are laid out horizontally, so left/right select them;
        // up/down are accepted as well for convenience.
        if (action & (kActUp | kActLeft)) confirmYes_ = true;
        if (action & (kActDown | kActRight)) confirmYes_ = false;
        if (action & kActBack) {
            mode_  = Mode::kBrowse;
            dirty_ = true;
        }
        if (action & kActEnter) {
            if (confirmYes_) {
                switch (confirmKind_) {
                    case ConfirmKind::kSave: confirmSave(); break;
                    case ConfirmKind::kDeleteCapture: deleteCapture(); break;
                    case ConfirmKind::kDeleteSaved: deleteSaved(); break;
                }
            } else {
                toast("CANCELLED");
            }
            mode_  = Mode::kBrowse;
            dirty_ = true;
        }
        return;
    }

    // ---- home --------------------------------------------------------------
    if (screen_ == Screen::kHome) {
        // Leaving the RF feature returns to the launcher's RF/NFC chooser.  The
        // two features need incompatible chip configurations, so this is a reboot
        // rather than a mode switch (see launcher.h).
        if (action & kActBack) {
            launcher::rebootInto(launcher::kAppMenu);
        }
        if (action & kActUp) {
            homeSel_ = (homeSel_ + kHomeItems - 1) % kHomeItems;
            dirty_   = true;
        }
        if (action & kActDown) {
            homeSel_ = (homeSel_ + 1) % kHomeItems;
            dirty_   = true;
        }
        if (action & kActEnter) {
            // CAPTURE / SEND / SCAN all drive the CC1101; the TF card entries
            // only touch NVS and SD, so they stay usable without a radio.
            if (homeSel_ <= 2 && !rf_.ready()) {
                toast("NO RADIO");
                beep(120, 120);
                return;
            }
            switch (homeSel_) {
                case 0:
                    screen_ = Screen::kCapture;
                    dirty_  = true;
                    break;
                case 1:
                    screen_ = Screen::kSend;
                    dirty_  = true;
                    break;
                case 2:
                    enterScan();
                    break;
            }
        }
        return;
    }

    if (action & kActBack) {
        if (screen_ == Screen::kScan) exitScan();
        screen_ = Screen::kHome;
        dirty_  = true;
        return;
    }

    // The scanner owns the tune register while it runs; cycling the band here
    // would fight it.
    if ((action & kActBand) && screen_ != Screen::kScan) {
        cycleBand();
    }

    if (screen_ == Screen::kScan) {
        if (action & kActEnter) applyScanResult();
        return;
    }

    // ---- capture page ------------------------------------------------------
    if (screen_ == Screen::kCapture) {
        if (action & kActUp) moveSelection(-1);
        if (action & kActDown) moveSelection(1);
        if (action & kActSend) sendCapture();
        if (action & kActDelete) {
            if (viewCount_ > 0) {
                confirmKind_ = ConfirmKind::kDeleteCapture;
                confirmYes_  = false;  // destructive: default to NO
                mode_        = Mode::kConfirm;
                dirty_       = true;
            }
        }
        if (action & kActEnter) {
            if (viewCount_ == 0) {
                toast("NOTHING CAPTURED");
            } else {
                mode_        = Mode::kConfirm;
                confirmKind_ = ConfirmKind::kSave;
                confirmYes_  = true;
                dirty_       = true;
            }
        }
        if (action & kActClear) {
            if (clearArmed_) {
                store_.clear();
                rebuildView();
                sel_        = 0;
                scroll_     = 0;
                clearArmed_ = false;
                toast("CAPTURES CLEARED");
            } else {
                clearArmed_   = true;
                clearArmedAt_ = millis();
                toast("PRESS C AGAIN");
            }
            dirty_ = true;
        }
        return;
    }

    // ---- send page ---------------------------------------------------------
    if (screen_ == Screen::kSend) {
        if (action & (kActUp | kActDown)) {
            if (!saved_.empty()) {
                const int n = saved_.size();
                sendSel_    = (action & kActUp) ? (sendSel_ + n - 1) % n : (sendSel_ + 1) % n;
                if (sendSel_ < sendScroll_) sendScroll_ = sendSel_;
                if (sendSel_ >= sendScroll_ + kRows) sendScroll_ = sendSel_ - kRows + 1;
            }
            dirty_ = true;
        }
        if (action & (kActSend | kActEnter)) sendSaved();
        if (action & kActBind) {
            if (saved_.empty()) {
                toast("NO SAVED SIGNAL");
            } else {
                // Modal picker: grid key, then icon, then color.
                bindpick::run(quickbind::kRf, static_cast<uint8_t>(sendSel_),
                              saved_.at(sendSel_).name);
                dirty_ = true;
            }
        }
        if (action & kActDelete) {
            if (!saved_.empty()) {
                confirmKind_ = ConfirmKind::kDeleteSaved;
                confirmYes_  = false;
                mode_        = Mode::kConfirm;
                dirty_       = true;
            }
        }
        if (action & kActRename) {
            if (saved_.empty()) {
                toast("NO SAVED SIGNAL");
            } else {
                mode_         = Mode::kRename;
                renameTarget_ = sendSel_;
                snprintf(renameBuf_, sizeof(renameBuf_), "%s", saved_.at(sendSel_).name);
                renameLen_ = static_cast<int>(strlen(renameBuf_));
                prevWord_.clear();
                dirty_ = true;
            }
        }
        return;
    }
}

void App::handleRenameInput()
{
    const auto& word = M5Cardputer.Keyboard.keysState().word;

    for (char c : word) {
        if (c < 32 || c >= 127) continue;
        // ; , . / are the navigation arrows, not name punctuation.
        if (c == ';' || c == ',' || c == '.' || c == '/') continue;
        if (c == '`' || c == '~') continue;  // that key is the ESC key
        bool already = false;
        for (char p : prevWord_) {
            if (p == c) {
                already = true;
                break;
            }
        }
        if (already) continue;
        if (renameLen_ >= static_cast<int>(sizeof(renameBuf_)) - 1) break;
        renameBuf_[renameLen_++] = c;
        renameBuf_[renameLen_]   = 0;
        dirty_                   = true;
    }
    prevWord_.assign(word.begin(), word.end());

    const uint32_t a     = readActions();
    const uint32_t pressed = a & ~prevActions_;
    prevActions_           = a;

    if (pressed & kActBack) {  // backspace = delete a character
        if (renameLen_ > 0) {
            renameBuf_[--renameLen_] = 0;
            dirty_                   = true;
        }
    }
    if (pressed & kActEnter) {
        if (renameLen_ > 0) {
            saved_.rename(renameTarget_, renameBuf_);
            toast("RENAMED");
        }
        mode_  = Mode::kBrowse;
        dirty_ = true;
    }
}

void App::pollReceiver()
{
    RfFrame frame;
    while (rx_.poll(frame)) {
        lastPulseMs_ = millis();

        const Decoded decoded = decodeFrame(frame);
        trace::record(frame, static_cast<uint8_t>(band_), decoded.bits,
                      static_cast<uint8_t>(decoded.proto), decoded.valid, decoded.baseUs);
        // A burst that does not decode is still worth keeping: replaying the raw
        // waveform works for any OOK remote, and with a marginal link the decode
        // is often what fails while the pulses themselves are fine.  Only the
        // code value is missing - the row is shown as RAW.
        if (!decoded.valid) {
            rejected_++;
            // Keep the waveform even without a code, but only when it actually
            // looks like a remote: otherwise the list fills with noise bursts.
            if (!looksLikeOok(frame)) continue;
#if RF_DIAG
            if (frame.count >= 20 && (rejected_ % 5) == 0) {
                Serial.printf("[rej] %s n=%u:", rfcfg::bandName(band_),
                              static_cast<unsigned>(frame.count));
                for (int k = 0; k < frame.count && k < 48; ++k) {
                    Serial.printf(" %u", static_cast<unsigned>(frame.pulses[k].us));
                }
                Serial.println();
            }
#endif
        }

        bool isNew      = false;
        const int index = store_.ingest(frame, decoded, band_, &isNew);
        if (index < 0) continue;

        rebuildView();
        if (isNew) {
            beep(1760, 25);
            toast("CAPTURED");
            for (int i = 0; i < viewCount_; ++i) {
                if (view_[i] == index) {
                    sel_ = i;
                    break;
                }
            }
            if (sel_ >= scroll_ + kRows) scroll_ = sel_ - kRows + 1;
            Serial.printf("[cap] %s %s %06lX bits=%u T=%u hits=%u pulses=%u\n",
                          rfcfg::bandName(band_), protocolName(decoded.proto),
                          static_cast<unsigned long>(decoded.code & 0xFFFFFFu),
                          static_cast<unsigned>(decoded.bits),
                          static_cast<unsigned>(decoded.baseUs),
                          static_cast<unsigned>(store_.at(index).hits),
                          static_cast<unsigned>(store_.at(index).count));
        }
        dirty_ = true;
    }
}

void App::rebuildView()
{
    viewCount_ = 0;
    for (int i = 0; i < store_.size(); ++i) {
        if (store_.at(i).band != band_) continue;
        if (viewCount_ < rfcfg::maxCaptures) view_[viewCount_++] = i;
    }
    if (sel_ >= viewCount_) sel_ = viewCount_ > 0 ? viewCount_ - 1 : 0;
    if (scroll_ > sel_) scroll_ = sel_;
}

void App::applyBand()
{
    rf_.setBand(band_);
    rf_.enterRx();
}

void App::cycleBand()
{
    switch (band_) {
        case RfBand::k315: band_ = RfBand::k433; break;
        case RfBand::k433: band_ = RfBand::k868; break;
        case RfBand::k868: band_ = RfBand::k315; break;
    }
    applyBand();
    rebuildView();
    sel_    = 0;
    scroll_ = 0;

    char buf[24];
    snprintf(buf, sizeof(buf), "BAND %s MHz", rfcfg::bandName(band_));
    toast(buf);
    dirty_ = true;
}

void App::moveSelection(int delta)
{
    if (viewCount_ == 0) return;
    sel_ = (sel_ + delta + viewCount_) % viewCount_;
    if (sel_ < scroll_) scroll_ = sel_;
    if (sel_ >= scroll_ + kRows) scroll_ = sel_ - kRows + 1;
    dirty_ = true;
}

App::TxReport App::transmit(const uint16_t* pulses, int count, uint16_t baseUs, RfBand band)
{
    TxReport r;

    if (band_ != band) {
        band_ = band;
        applyBand();
        rebuildView();
    }

    rx_.suspend();
    rf_.enterTx();
    delay(3);

    // Stay on air for roughly as long as a person holds the remote.  The
    // receiver needs several consecutive frames, and at long range every extra
    // frame is another chance to punch through the noise; the repeat count is
    // derived from the length of the stored train so the duration is fixed.
    int trainStart = 0;
    int trainEnd   = count;
    rfreplay::wholeFrameRange(pulses, count, baseUs, &trainStart, &trainEnd);
    const uint16_t* train = pulses + trainStart;
    r.trainCount          = trainEnd - trainStart;

    r.reps = rfreplay::planRepeats(train, r.trainCount, &r.durMs);
    r.ok   = tx_.send(train, r.trainCount, r.reps, 0);

    // The PA stage can leave the chip in an odd state; re-apply the whole band
    // configuration before listening again.
    rf_.enterRx();
    delay(2);
    applyBand();
    rx_.resume();

    // The transmit blocks for a few hundred ms; drain the keyboard events that
    // piled up meanwhile and treat anything still held as already handled.
    for (int i = 0; i < 10; ++i) {
        M5Cardputer.update();
        delay(1);
    }
    prevActions_ = readActions();

    return r;
}

void App::sendCapture()
{
    if (viewCount_ == 0) {
        toast("NOTHING TO SEND");
        return;
    }
    const Capture& c = store_.at(view_[sel_]);
    const TxReport r = transmit(c.pulses, c.count, c.baseUs, c.band);

    toast(r.ok ? "SENT" : "SEND FAILED");
    Serial.printf("[tx] %s %06lX %s x%d -> %s\n", rfcfg::bandName(band_),
                  static_cast<unsigned long>(c.code & 0xFFFFFFu),
                  c.valid ? "frame" : "raw", r.reps, r.ok ? "ok" : "failed");
    Serial.printf("[tx] on air %lu ms (%d reps of %d pulses)\n",
                  static_cast<unsigned long>(r.durMs), r.reps, r.trainCount);
    dirty_ = true;
}

void App::sendSaved()
{
    if (saved_.empty()) {
        toast("NO SAVED SIGNAL");
        return;
    }
    const SavedSignal& s = saved_.at(sendSel_);
    const TxReport r     = transmit(s.pulses, s.count, s.baseUs, s.band);

    toast(r.ok ? "SENT" : "SEND FAILED");
    Serial.printf("[tx] saved '%s' %s %06lX x%d -> %s\n", s.name, rfcfg::bandName(s.band),
                  static_cast<unsigned long>(s.code & 0xFFFFFFu), r.reps, r.ok ? "ok" : "failed");
    Serial.printf("[tx] on air %lu ms (%d reps of %d pulses)\n",
                  static_cast<unsigned long>(r.durMs), r.reps, r.trainCount);
    dirty_ = true;
}

void App::deleteCapture()
{
    if (viewCount_ == 0) return;
    store_.remove(view_[sel_]);
    rebuildView();
    toast("DELETED");
    dirty_ = true;
}

void App::deleteSaved()
{
    if (saved_.empty()) return;
    saved_.remove(sendSel_);
    if (sendSel_ >= saved_.size()) sendSel_ = saved_.size() > 0 ? saved_.size() - 1 : 0;
    if (sendScroll_ > 0 && sendScroll_ >= saved_.size()) {
        sendScroll_ = saved_.size() > kRows ? saved_.size() - kRows : 0;
    }
    toast("DELETED");
    dirty_ = true;
}

void App::confirmSave()
{
    if (viewCount_ == 0) return;
    int index = -1;
    if (saved_.add(store_.at(view_[sel_]), &index)) {
        toast("SAVED");
        Serial.printf("[save] #%d '%s'\n", index, saved_.at(index).name);
    } else if (index >= 0) {
        toast("ALREADY SAVED");
    } else {
        toast("SAVE LIST FULL");
    }
}

void App::backupSignals()
{
    if (saved_.empty()) {
        toast("NOTHING TO BACKUP");
        return;
    }

    // Card access blocks for a few hundred milliseconds; pause the receiver so
    // its edge ring buffer does not overflow meanwhile.
    rx_.suspend();
    int count              = 0;
    const TfResult result  = tfbackup::saveSignals(saved_, &count);
    rx_.resume();
    prevActions_ = readActions();

    char buf[28];
    switch (result) {
        case TfResult::kOk:
            snprintf(buf, sizeof(buf), "BACKED UP %d", count);
            break;
        case TfResult::kNoCard:
            snprintf(buf, sizeof(buf), "NO TF CARD");
            break;
        default:
            snprintf(buf, sizeof(buf), "TF WRITE FAILED");
            break;
    }
    Serial.printf("[tf] backup result=%d n=%d\n", static_cast<int>(result), count);
    toast(buf);
    dirty_ = true;
}

void App::restoreSignals()
{
    const uint32_t now = millis();
    if (!restoreArmed_ || now - restoreArmedAt_ > 4000) {
        restoreArmed_   = true;
        restoreArmedAt_ = now;
        toast("ENTER AGAIN TO RESTORE");
        dirty_ = true;
        return;
    }
    restoreArmed_ = false;

    rx_.suspend();
    int count             = 0;
    const TfResult result = tfbackup::loadSignals(saved_, &count);
    rx_.resume();
    prevActions_ = readActions();

    sendSel_    = 0;
    sendScroll_ = 0;

    char buf[28];
    switch (result) {
        case TfResult::kOk:
            snprintf(buf, sizeof(buf), "RESTORED %d", count);
            break;
        case TfResult::kPartial:
            snprintf(buf, sizeof(buf), "RESTORED %d, SKIPPED", count);
            break;
        case TfResult::kNoCard:
            snprintf(buf, sizeof(buf), "NO TF CARD");
            break;
        case TfResult::kNoFile:
            snprintf(buf, sizeof(buf), "NO BACKUP FILE");
            break;
        case TfResult::kEmpty:
            snprintf(buf, sizeof(buf), "BACKUP EMPTY");
            break;
        case TfResult::kBadFile:
            snprintf(buf, sizeof(buf), "BAD BACKUP FILE");
            break;
        default:
            snprintf(buf, sizeof(buf), "TF READ FAILED");
            break;
    }
    Serial.printf("[tf] restore result=%d n=%d\n", static_cast<int>(result), count);
    toast(buf);
    dirty_ = true;
}

void App::toast(const char* text)
{
    snprintf(toastBuf_, sizeof(toastBuf_), "%s", text);
    toastActive_ = true;
    toastUntil_  = millis() + 1500;
    dirty_       = true;
}

void App::beep(uint16_t freq, uint16_t durationMs)
{
    M5.Speaker.tone(freq, durationMs);
}

void App::render()
{
    canvas_.fillScreen(kColBg);

    switch (screen_) {
        case Screen::kHome: renderHome(); break;
        case Screen::kCapture: renderCapture(); break;
        case Screen::kSend: renderSend(); break;
        case Screen::kScan: renderScan(); break;
    }

    if (mode_ == Mode::kConfirm) renderConfirm();
    if (mode_ == Mode::kRename) renderRename();

    canvas_.pushSprite(0, 0);
}

void App::renderHeader(const char* title, bool showBand)
{
    const int w = canvas_.width();
    canvas_.fillRect(0, 0, w, kHeaderH, kColHead);

    // Without a radio the band readout means nothing and every RF screen is
    // inert, so the header says so instead of the screen title.
    if (!rf_.ready()) {
        canvas_.setTextColor(kColWarn, kColHead);
        canvas_.drawString("NO RADIO", 3, 2);
    } else {
#if RF_DIAG
        // The title changes colour once per second so a frozen picture is
        // unmistakable.
        canvas_.setTextColor(tick_ ? kColAccent : kColDim, kColHead);
#else
        canvas_.setTextColor(kColDim, kColHead);
#endif
        canvas_.drawString(title, 3, 2);

        if (showBand) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%sMHz", rfcfg::bandName(band_));
            canvas_.setTextColor(kColText, kColHead);
            canvas_.drawString(buf, 120, 2);
        }
    }

    if (lastPulseMs_ != 0 && millis() - lastPulseMs_ < 400) {
        canvas_.fillRect(160, 4, 5, 5, kColOk);
    }

#if RF_DIAG
    // 1 Hz blink in the header so a frozen picture is distinguishable from a
    // live one.
    canvas_.fillRect(168, 4, 8, 8, tick_ ? kColOk : kColBg);
#endif

    char battery[12];
    if (batteryLevel_ < 0) {
        snprintf(battery, sizeof(battery), "--%%");
    } else {
        snprintf(battery, sizeof(battery), "%d%%", batteryLevel_);
    }
    canvas_.setTextColor(kColText, kColHead);
    canvas_.drawRightString(battery, w - 3, 2);
}

void App::renderFooter(const char* hint)
{
    canvas_.drawFastHLine(0, kFooterY, canvas_.width(), kColDim);
    if (toastActive_) {
        canvas_.setTextColor(kColAccent, kColBg);
        canvas_.drawString(toastBuf_, 3, kFooterY + 4);
    } else {
        canvas_.setTextColor(kColDim, kColBg);
        canvas_.drawString(hint, 3, kFooterY + 4);
    }
}

// Footer with the key names picked out in green: "ENT save  S send ...".
void App::renderFooterHints(const FooterHint* hints, int count)
{
    canvas_.drawFastHLine(0, kFooterY, canvas_.width(), kColDim);
    if (toastActive_) {
        canvas_.setTextColor(kColAccent, kColBg);
        canvas_.drawString(toastBuf_, 3, kFooterY + 4);
        return;
    }

    const int y = kFooterY + 4;
    int x       = 3;
    for (int i = 0; i < count; ++i) {
        if (hints[i].key == nullptr) break;

        // Skip what does not fit: a string that crosses the right edge wraps to
        // the next line, which is off-screen -- the hint vanishes entirely.
        const int need = canvas_.textWidth(hints[i].key) +
                         ((hints[i].text != nullptr && hints[i].text[0] != 0)
                              ? 2 + canvas_.textWidth(hints[i].text)
                              : 0);
        if (x + need > canvas_.width()) break;

        canvas_.setTextColor(kColOk, kColBg);
        canvas_.drawString(hints[i].key, x, y);
        x += canvas_.textWidth(hints[i].key);

        if (hints[i].text != nullptr && hints[i].text[0] != 0) {
            canvas_.setTextColor(kColDim, kColBg);
            canvas_.drawString(hints[i].text, x + 2, y);
            x += 2 + canvas_.textWidth(hints[i].text);
        }
        x += 6;
    }
}

void App::renderHome()
{
    renderHeader("RF CAPTURE", true);

    canvas_.setTextSize(2);
    const char* items[kHomeItems] = {"CAPTURE", "SEND", "SCAN RF"};
    for (int i = 0; i < kHomeItems; ++i) {
        // Three items spread across the whole 13..118 body rather than stacked
        // at the top: y = 30/58/86 leaves the selection boxes centred with the
        // footer rule at 118.
        const int y         = 30 + i * 28;
        const bool selected = (i == homeSel_);
        if (selected) {
            canvas_.fillRect(4, y - 4, canvas_.width() - 8, 22, kColBar);
        }
        canvas_.setTextColor(selected ? kColAccent : kColText, selected ? kColBar : kColBg);
        canvas_.setCursor(26, y);
        canvas_.printf("%s %s", selected ? ">" : " ", items[i]);
    }

    canvas_.setTextSize(1);
    static const FooterHint kHomeHints[] = {{"ENTER", "open"}};
    renderFooterHints(kHomeHints, 1);
}

void App::renderCapture()
{
    char title[32];
    snprintf(title, sizeof(title), "CAPTURE  n=%d", viewCount_);
    renderHeader(title, true);

    if (viewCount_ == 0) {
        char line[40];
        canvas_.setTextColor(kColDim, kColBg);
        snprintf(line, sizeof(line), "Listening on %s MHz ...", rfcfg::bandName(band_));
        canvas_.drawString(line, 6, 20);

        // Live signal strength.  Sweeping the remote around while watching this
        // tells an aim/angle problem apart from a distance problem in seconds.
        canvas_.setTextSize(2);
        canvas_.setTextColor(kColAccent, kColBg);
        snprintf(line, sizeof(line), "%d dBm", rssiDbm_);
        canvas_.drawString(line, 6, 32);
        canvas_.setTextSize(1);

        snprintf(line, sizeof(line), "peak %d", rssiPeak_);
        canvas_.setTextColor(kColDim, kColBg);
        canvas_.drawRightString(line, canvas_.width() - 6, 38);

        const int bx = 6;
        const int bw = canvas_.width() - 12;
        const int by = 54;
        canvas_.drawRect(bx, by, bw, 9, kColDim);

        auto level2x = [bw](int dbm) {
            if (dbm < -105) dbm = -105;
            if (dbm > -35) dbm = -35;
            return (dbm + 105) * (bw - 2) / 70;
        };
        const int fill = level2x(rssiDbm_);
        if (fill > 0) canvas_.fillRect(bx + 1, by + 1, fill, 7, kColOk);
        const int peakX = bx + 1 + level2x(rssiPeak_);
        canvas_.drawFastVLine(peakX, by - 1, 11, kColAccent);

        canvas_.setTextColor(kColDim, kColBg);
        canvas_.drawString("noise floor ~ -100, remote ~ -60", 6, 70);
        canvas_.drawString("move/rotate the remote to peak it", 6, 82);
        renderFooter("");
        return;
    }

    // ---- list first: one compact RSSI strip above the rows --------------------
    char strip[32];
    snprintf(strip, sizeof(strip), "%ddBm pk%ddBm", rssiDbm_, rssiPeak_);
    canvas_.setTextColor(kColDim, kColBg);
    canvas_.drawString(strip, 3, kRssiY);

    const int bw = 118;
    const int bx = canvas_.width() - 3 - bw;
    canvas_.drawRect(bx, kRssiY + 1, bw, 5, kColDim);
    int dbm = rssiDbm_;
    if (dbm < -105) dbm = -105;
    if (dbm > -35) dbm = -35;
    const int fill = (dbm + 105) * (bw - 2) / 70;
    if (fill > 0) {
        canvas_.fillRect(bx + 1, kRssiY + 2, fill, 3,
                         dbm >= RF_SCAN_ACTIVE_DBM ? kColOk : kColDim);
    }

    for (int r = 0; r < kRows; ++r) {
        const int index = scroll_ + r;
        if (index >= viewCount_) break;

        const Capture& c    = store_.at(view_[index]);
        const bool selected = (index == sel_);
        const int y         = kListY + r * kRowH;

        if (selected) canvas_.fillRect(0, y - 1, canvas_.width(), kRowH - 2, kColBar);

        const uint16_t fg = selected ? kColAccent : kColText;
        const uint16_t bg = selected ? kColBar : kColBg;

        char line[40];
        char right[12];
        const unsigned q = store_.quality(view_[index]);
        if (c.valid) {
            snprintf(line, sizeof(line), "%02d %-6s %06lX q%02u x%u", index + 1,
                     protocolName(static_cast<RfProtocol>(c.proto)),
                     static_cast<unsigned long>(c.code & 0xFFFFFFu), q,
                     static_cast<unsigned>(c.hits));
            snprintf(right, sizeof(right), "%3uus", static_cast<unsigned>(c.baseUs));
        } else {
            // No usable code: show how many pulses were stored instead, so the
            // row still means something and can be replayed.
            snprintf(line, sizeof(line), "%02d %-6s %4up q%02u x%u", index + 1, "RAW",
                     static_cast<unsigned>(c.count), q, static_cast<unsigned>(c.hits));
            snprintf(right, sizeof(right), "%3uus", static_cast<unsigned>(c.baseUs));
        }
        // The score is the only on-screen hint that a stored entry may replay
        // badly (docs/DEV-STATE.md section 8.3), so it goes on the row itself.
        canvas_.setTextColor(c.valid ? fg : (selected ? kColAccent : kColDim), bg);
        canvas_.drawString(line, 3, y);
        canvas_.setTextColor(kColDim, bg);
        canvas_.drawRightString(right, canvas_.width() - 3, y);
    }

    if (clearArmed_) {
        static const FooterHint kClearHints[] = {{"C", "again to clear all"}};
        renderFooterHints(kClearHints, 1);
    } else {
        static const FooterHint kCapHints[] = {{"ENT", "save"}, {"S", "send"},  {"F", "band"},
                                               {"C", "clr"},   {"esc", "back"}};
        renderFooterHints(kCapHints, 5);
    }
}

void App::renderSend()
{
    char title[32];
    snprintf(title, sizeof(title), "SEND  n=%d", saved_.size());
    renderHeader(title, true);

    if (saved_.empty()) {
        canvas_.setTextColor(kColDim, kColBg);
        canvas_.drawString("No saved signal yet.", 6, 42);
        canvas_.drawString("Capture one, then press ENTER", 6, 56);
        renderFooter("");
        return;
    }

    for (int r = 0; r < kRows; ++r) {
        const int index = sendScroll_ + r;
        if (index >= saved_.size()) break;

        const SavedSignal& s = saved_.at(index);
        const bool selected  = (index == sendSel_);
        const int y          = kHeaderH + 2 + r * kRowH;

        if (selected) canvas_.fillRect(0, y - 1, canvas_.width(), kRowH - 2, kColBar);

        const uint16_t fg = selected ? kColAccent : kColText;
        const uint16_t bg = selected ? kColBar : kColBg;

        char line[40];
        snprintf(line, sizeof(line), "%02d %-12s", index + 1, s.name);
        canvas_.setTextColor(fg, bg);
        canvas_.drawString(line, 3, y);

        char right[24];
        snprintf(right, sizeof(right), "%s %06lX q%02u", rfcfg::bandName(s.band),
                 static_cast<unsigned long>(s.code & 0xFFFFFFu),
                 static_cast<unsigned>(saved_.quality(index)));
        canvas_.drawRightString(right, canvas_.width() - 3, y);
    }

    static const FooterHint kSendHints[] = {{"S", "send"},  {"N", "rename"}, {"B", "bind"},
                                            {"del", "del"}, {"esc", "back"}};
    renderFooterHints(kSendHints, 5);
}

void App::renderConfirm()
{
    const int w = 190;
    const int h = 62;
    const int x = (canvas_.width() - w) / 2;
    const int y = (canvas_.height() - h) / 2;

    canvas_.fillRect(x, y, w, h, kColBox);
    canvas_.drawRect(x, y, w, h, kColAccent);

    char title[32];
    if (confirmKind_ == ConfirmKind::kSave) {
        snprintf(title, sizeof(title), "Save this signal?");
    } else if (confirmKind_ == ConfirmKind::kDeleteSaved && sendSel_ < saved_.size()) {
        snprintf(title, sizeof(title), "Delete '%s'?", saved_.at(sendSel_).name);
    } else if (confirmKind_ == ConfirmKind::kDeleteCapture && viewCount_ > 0) {
        const Capture& c = store_.at(view_[sel_]);
        if (c.valid) {
            snprintf(title, sizeof(title), "Delete %06lX?",
                     static_cast<unsigned long>(c.code & 0xFFFFFFu));
        } else {
            snprintf(title, sizeof(title), "Delete RAW %up?", static_cast<unsigned>(c.count));
        }
    } else {
        snprintf(title, sizeof(title), "Delete?");
    }
    canvas_.setTextColor(kColText, kColBox);
    canvas_.drawString(title, x + 8, y + 8);

    canvas_.setTextColor(confirmYes_ ? kColAccent : kColDim, kColBox);
    canvas_.drawString(confirmYes_ ? "[YES]" : " YES ", x + 16, y + 26);
    canvas_.setTextColor(confirmYes_ ? kColDim : kColAccent, kColBox);
    canvas_.drawString(confirmYes_ ? " NO " : "[NO]", x + 100, y + 26);

    canvas_.setTextColor(kColDim, kColBox);
    canvas_.drawString("< > choose   ENTER ok", x + 8, y + 44);
}

void App::renderRename()
{
    const int w = 210;
    const int h = 56;
    const int x = (canvas_.width() - w) / 2;
    const int y = (canvas_.height() - h) / 2;

    canvas_.fillRect(x, y, w, h, kColBox);
    canvas_.drawRect(x, y, w, h, kColAccent);

    canvas_.setTextColor(kColDim, kColBox);
    canvas_.drawString("Rename signal", x + 8, y + 6);

    canvas_.setTextColor(kColText, kColBox);
    char buf[32];
    snprintf(buf, sizeof(buf), "%s_", renameBuf_);
    canvas_.drawString(buf, x + 8, y + 22);

    canvas_.setTextColor(kColDim, kColBox);
    canvas_.drawString("ENTER ok   DEL delete", x + 8, y + 38);
}

// How many times the stored train has to be repeated to stay on air for
// RF_TX_TARGET_MS.  Deriving it from the measured length of the train keeps the
// on-air time constant whether the capture holds one 50 pulse frame or a
// multi-frame train from an older firmware.

// ---------------------------------------------------------------- RSSI scan --

void App::enterScan()
{
    for (int i = 0; i < rfcfg::kScanCount; ++i) scanRssi_[i] = -110;
    scanIndex_   = 0;
    scanBest_    = -1;
    scanSweeps_  = 0;
    lastScanMs_  = 0;

    // The scanner retunes the chip non stop; keep the edge receiver out of the
    // way so it does not fill its ring with noise from every hop.
    rx_.suspend();

    screen_ = Screen::kScan;
    dirty_  = true;
}

void App::exitScan()
{
    applyBand();   // restore the configuration for the selected band
    rx_.resume();
    dirty_ = true;
}

void App::scanTick(uint32_t now)
{
    if (now - lastScanMs_ < 15) return;
    lastScanMs_ = now;

    if (scanIndex_ >= rfcfg::kScanCount) {
        // Sweep finished: peak pick and start over.
        scanIndex_ = 0;
        scanSweeps_++;
        int best = -120;
        int bestIndex = -1;
        for (int i = 0; i < rfcfg::kScanCount; ++i) {
            if (scanRssi_[i] > best) {
                best      = scanRssi_[i];
                bestIndex = i;
            }
        }
        scanBest_ = bestIndex;
        dirty_    = true;
        return;
    }

    const rfcfg::ScanPoint& pt = rfcfg::kScanPoints[scanIndex_];
    rf_.tuneTo(pt.band, pt.hz);
    delay(1);
    scanRssi_[scanIndex_] = rf_.rssiDbm();
    scanTunedMs_          = now;
    scanIndex_++;
    dirty_ = true;
}

void App::applyScanResult()
{
    if (scanBest_ < 0) return;

    band_ = rfcfg::kScanPoints[scanBest_].band;
    exitScan();
    rebuildView();
    sel_    = 0;
    scroll_ = 0;

    char buf[24];
    snprintf(buf, sizeof(buf), "BAND %s MHz", rfcfg::bandName(band_));
    toast(buf);
    screen_ = Screen::kCapture;
    dirty_  = true;
}

void App::renderScan()
{
    char title[32];
    snprintf(title, sizeof(title), "SCAN RF  sweep %d", scanSweeps_);
    renderHeader(title, false);

    constexpr int kRowH = 14;
    constexpr int kBarX = 62;
    constexpr int kBarW = 118;

    for (int i = 0; i < rfcfg::kScanCount; ++i) {
        const int y = 16 + i * kRowH;
        const rfcfg::ScanPoint& pt = rfcfg::kScanPoints[i];
        const bool active = scanRssi_[i] >= RF_SCAN_ACTIVE_DBM;
        const bool best   = (i == scanBest_);

        char label[12];
        snprintf(label, sizeof(label), "%u.%02u", static_cast<unsigned>(pt.hz / 1000000u),
                 static_cast<unsigned>((pt.hz % 1000000u) / 10000u));
        canvas_.setTextColor(best ? kColAccent : (active ? kColOk : kColText), kColBg);
        canvas_.drawString(label, 4, y);

        canvas_.drawRect(kBarX, y, kBarW, 8, kColDim);
        int dbm = scanRssi_[i];
        if (dbm < -105) dbm = -105;
        if (dbm > -40) dbm = -40;
        const int w = (dbm + 105) * (kBarW - 2) / 65;
        if (w > 0) {
            canvas_.fillRect(kBarX + 1, y + 1, w, 6, active ? kColOk : kColDim);
        }

        char val[8];
        snprintf(val, sizeof(val), "%d", scanRssi_[i]);
        canvas_.setTextColor(active ? kColOk : kColDim, kColBg);
        canvas_.drawRightString(val, canvas_.width() - 3, y);

        // Underline the point currently being measured.
        if (i == scanIndex_ && (millis() - scanTunedMs_) < 400) {
            canvas_.drawFastHLine(kBarX, y + 9, kBarW, kColAccent);
        }
    }

    char foot[40];
    if (scanBest_ >= 0) {
        const rfcfg::ScanPoint& b = rfcfg::kScanPoints[scanBest_];
        snprintf(foot, sizeof(foot), "best %u.%02u %ddBm  ENT use",
                 static_cast<unsigned>(b.hz / 1000000u),
                 static_cast<unsigned>((b.hz % 1000000u) / 10000u), scanRssi_[scanBest_]);
    } else {
        snprintf(foot, sizeof(foot), "sweeping %d points...", rfcfg::kScanCount);
    }
    renderFooter(foot);
}

// A stored train from an older firmware usually ends in the middle of a frame
// and can even be an odd number of pulses - which means it ends on a HIGH level.
// Repeating such a train splices a partial frame into the stream, and the high it
// ends on merges with the next repetition's sync high, so every repetition starts
// with a corrupted edge.  A receiver that needs *consecutive identical* frames
// then only ever sees the few that sit inside one repetition, which is why such a
// signal only works every few presses.
//
// Cutting the train back to its last sync pulse makes every repetition a whole
// number of frames, so the repetitions chain into one continuous stream.  The
// head is deliberately left alone: older records carry a few leading pulses that
// the receiver tolerates, and trimming there would change signals that currently
// work.

#if RF_GDO_TEST
// Does GDO0 follow IOCFG0 at all?  On this module 0x2F drives the pin low and
// 0x6F (0x2F | invert) drives it high - the same quirk documented for GDO2.
void App::runGdoTest()
{
    Serial.println("[gdo] static GDO0 test");
    rf_.debugSetIocfg0(0x2F);
    delay(3);
    const int low = digitalRead(rfcfg::gdo0Pin);
    rf_.debugSetIocfg0(0x6F);
    delay(3);
    const int high = digitalRead(rfcfg::gdo0Pin);
    Serial.printf("[gdo] iocfg0=0x2F -> %d, iocfg0=0x6F -> %d (expect 0 then 1)\n", low, high);
    Serial.printf("[gdo] %s\n", (low == 0 && high == 1) ? "GDO0 wiring OK" : "GDO0 NOT FOLLOWING");
    rf_.enterRx();
}
#endif
