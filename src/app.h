#pragma once

#include <M5Cardputer.h>
#include <M5GFX.h>

#include <vector>

#include "capture_store.h"
#include "cc1101.h"
#include "ook_decoder.h"
#include "ook_rx.h"
#include "ook_tx.h"
#include "saved_store.h"

// One footer entry: the key that is pressed, and what it does.  The key is drawn
// in the accent colour so it stands out from the description.
struct FooterHint {
    const char* key;
    const char* text;
};

class App {
public:
    void begin();
    void update();

private:
    enum class Screen : uint8_t { kHome, kCapture, kSend, kScan };
    enum class Mode : uint8_t { kBrowse, kConfirm, kRename };
    enum class ConfirmKind : uint8_t { kSave, kDeleteCapture, kDeleteSaved };

    enum Action : uint32_t {
        kActUp     = 1u << 0,
        kActDown   = 1u << 1,
        kActEnter  = 1u << 2,
        kActBack   = 1u << 3,
        kActSend   = 1u << 4,
        kActBand   = 1u << 5,
        kActDelete = 1u << 6,
        kActClear  = 1u << 7,
        kActRename = 1u << 8,
        kActLeft   = 1u << 9,
        kActRight  = 1u << 10,
        kActBind   = 1u << 11,
        kActCancel = 1u << 12,
    };

    uint32_t readActions();
    void handleAction(uint32_t action);
    void handleRenameInput();
    void pollReceiver();
#if RF_RX_SELFTEST
    void runRxSelfTest();
#endif
#if RF_GDO_TEST
    void runGdoTest();
#endif

    void rebuildView();
    void applyBand();
    void cycleBand();
    void moveSelection(int delta);
    void sendCapture();
    void sendSaved();
    void deleteCapture();
    void deleteSaved();
    void confirmSave();

    /// What one transmit() call actually put on air, for the caller's log line.
    struct TxReport {
        bool ok         = false;
        int reps        = 0;
        int trainCount  = 0;
        uint32_t durMs  = 0;
    };

    /// The whole send sequence shared by the capture and the saved list: retune
    /// when the entry lives on another band, narrow the train to whole frames,
    /// transmit, restore the receiver, then swallow the key events that piled up
    /// while the transmission blocked the loop.
    TxReport transmit(const uint16_t* pulses, int count, uint16_t baseUs, RfBand band);

    void enterScan();
    void exitScan();
    void scanTick(uint32_t now);
    void applyScanResult();

    void render();
    void renderHeader(const char* title, bool showBand);
    void renderHome();
    void renderScan();
    void renderCapture();
    void renderSend();
    void renderConfirm();
    void renderRename();
    void renderFooter(const char* hint);
    void renderFooterHints(const FooterHint* hints, int count);
    void toast(const char* text);
    void beep(uint16_t freq, uint16_t ms);

    M5Canvas canvas_{&M5Cardputer.Display};

    Cc1101& rf_ = g_cc1101;
    OokReceiver rx_;
    OokTransmitter tx_;
    CaptureStore store_;
    SavedStore saved_;

    Screen screen_ = Screen::kHome;
    Mode mode_     = Mode::kBrowse;
    int homeSel_   = 0;

    RfBand band_   = RfBand::k315;
    int view_[rfcfg::maxCaptures];
    int viewCount_ = 0;
    int sel_       = 0;
    int scroll_    = 0;

    int sendSel_    = 0;
    int sendScroll_ = 0;

    bool confirmYes_   = true;
    ConfirmKind confirmKind_ = ConfirmKind::kSave;

    char renameBuf_[16] = {0};
    int renameLen_      = 0;
    int renamePos_      = 0;
    bool renameCursorOn_ = true;
    uint32_t renameCursorMs_ = 0;
    int renameTarget_   = -1;
    std::vector<char> prevWord_;

    uint32_t actions_     = 0;
    uint32_t prevActions_ = 0;
    bool dirty_           = true;
    bool clearArmed_      = false;
    uint32_t clearArmedAt_ = 0;

    uint32_t lastDrawMs_      = 0;
    uint32_t lastBatteryMs_   = 0;
    uint32_t lastPulseMs_     = 0;
    uint32_t lastHeartbeatMs_ = 0;
    uint32_t lastTickMs_      = 0;
    bool tick_                = false;
    int batteryLevel_         = -1;

    // Live signal strength, sampled from the CC1101 RSSI register.
    int rssiDbm_         = -110;
    int rssiPeak_        = -110;
    uint32_t lastRssiMs_ = 0;
    uint32_t rssiPeakMs_ = 0;

    // RSSI scanner state.
    int scanRssi_[rfcfg::kScanCount];
    int scanIndex_       = 0;
    int scanBest_        = -1;
    int scanSweeps_      = 0;
    uint32_t lastScanMs_ = 0;
    uint32_t scanTunedMs_ = 0;
    uint32_t rejected_        = 0;
    uint32_t loops_           = 0;

    char toastBuf_[48] = {0};
    bool toastActive_  = false;
    uint32_t toastUntil_ = 0;
};
