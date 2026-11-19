#pragma once

// Dedicated flashing screen shared by SETTINGS and HARNESS.
//
// Once ArduinoOTA.onStart fires, the app must show nothing but this page: a
// pair of plain text lines and one progress bar for the whole image transfer.
// The caller is responsible for suspending its normal UI (and, in HARNESS,
// every other subsystem) while otaUploading is true.
//
// The template accepts either the panel (M5GFX) or the HARNESS backbuffer
// (M5Canvas); both expose the same drawing API.

#include <cstdint>
#include <cstdio>

namespace flashui {

constexpr uint16_t kBg    = 0x0000;
constexpr uint16_t kText  = 0xFFFF;
constexpr uint16_t kDim   = 0x8410;
constexpr uint16_t kOk    = 0x07E0;
constexpr uint16_t kWarn  = 0xF800;

template <typename GFX>
void drawCenter(GFX& g, const char* text, int y)
{
    g.drawString(text, (g.width() - g.textWidth(text)) / 2, y);
}

/// The one screen shown for the whole upload.  `percent` is 0..100 and comes
/// from ArduinoOTA.onProgress; `note` is a short line under the title;
/// `title` distinguishes the wired (ROM bootloader) and wireless (OTA)
/// flashing screens.
template <typename GFX>
void draw(GFX& g, int percent, const char* note, const char* title = "FLASHING")
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    g.fillScreen(kBg);

    g.setTextFont(1);
    g.setTextSize(2);
    g.setTextColor(kText, kBg);
    drawCenter(g, title, 10);

    g.setTextSize(1);
    g.setTextColor(kDim, kBg);
    drawCenter(g, note, 36);

    const int barX = 14;
    const int barY = 60;
    const int barW = g.width() - 28;
    const int barH = 22;
    g.drawRect(barX, barY, barW, barH, kDim);
    if (percent > 0) {
        // Keep a visible gap inside the 1 px border on both sides.
        g.fillRect(barX + 2, barY + 2, (barW - 4) * percent / 100, barH - 4, kOk);
    }

    g.setTextSize(2);
    g.setTextColor(kText, kBg);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", percent);
    drawCenter(g, pct, 88);

    g.setTextSize(1);
    g.setTextColor(kWarn, kBg);
    drawCenter(g, "DO NOT POWER OFF", 118);

    // Leave the shared text state at 1x for whichever screen draws next.
    g.setTextSize(1);
}

/// Minimal failure page: the flash did not complete, the app is alive and esc
/// leaves this page.  Everything else stays off the screen.
template <typename GFX>
void drawError(GFX& g, const char* detail)
{
    g.fillScreen(kBg);
    g.setTextFont(1);
    g.setTextSize(2);
    g.setTextColor(kWarn, kBg);
    drawCenter(g, "FLASH FAILED", 24);

    g.setTextSize(1);
    g.setTextColor(kText, kBg);
    drawCenter(g, detail, 56);

    g.setTextColor(kDim, kBg);
    drawCenter(g, "esc back - retry from the PC", 104);
    g.setTextSize(1);
}

}  // namespace flashui
