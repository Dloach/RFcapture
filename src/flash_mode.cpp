#include "flash_mode.h"

#include <M5Cardputer.h>
#include <USB.h>

#include <esp_event.h>
#include <esp32-hal-tinyusb.h>  // restart_type_t, usb_persist_restart

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "flash_ui.h"

namespace flashmode {
namespace {

auto& lcd = M5Cardputer.Display;
bool   armed = false;

// Painted once, right before the reset: in the ROM bootloader the firmware
// is gone, so this frame -- frozen on the LCD across the reset -- is the only
// UI the device has for the whole host-side upload.
void enterRomBootloader()
{
    // Every app in this firmware draws from the Arduino loop task (each
    // feature's whole life runs there).  Suspend it first so no in-flight
    // redraw -- HARNESS blinks its caret every 500 ms, the launcher redraws
    // on keys -- can overwrite the flashing page between our draw and the
    // reset.  Suspended also means no keyboard polling: with the loop task
    // parked, no key is read and no action runs until the chip resets.
    const TaskHandle_t loop = xTaskGetHandle("loopTask");
    if (loop != nullptr) vTaskSuspend(loop);

    Serial.println("[flash] 1200 bps touch -> ROM bootloader");
    Serial.flush();

    flashui::draw(lcd, 0, "PC IS UPLOADING", "USB FLASH");
    delay(120);  // let the frame transfer over SPI settle before the reset

    usb_persist_restart(RESTART_BOOTLOADER);
}

// esp_event_handler_t, dispatched by the Arduino USB event task.
void onCdcEvent(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    (void)arg;
    if (base != ARDUINO_USB_CDC_EVENTS) return;
    if (id != ARDUINO_USB_CDC_LINE_CODING_EVENT) return;
    const auto* d = static_cast<const arduino_usb_cdc_event_data_t*>(data);
    if (d->line_coding.bit_rate != 1200) return;
    if (armed) return;  // the restart is one shot; never re-enter
    armed = true;
    enterRomBootloader();
}

}  // namespace

void begin(USBCDC& cdc)
{
    // Disable the core's own silent 1200 bps restart.  With it off, the
    // request falls through to the LINE_CODING event we listen for above
    // instead of resetting the chip behind our back.
    cdc.enableReboot(false);
    cdc.onEvent(onCdcEvent);
}

}  // namespace flashmode
