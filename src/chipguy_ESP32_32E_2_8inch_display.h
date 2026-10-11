/*
 * ILI9341 / ST7789 SPI display driver for the ESP32-32E 2.8" touch LCD
 *
 * This is a generic ESP32 display board sold by several vendors; the PCB
 * silkscreen reads: 2.8" LCD Display, ESP32-32E 240x320, Resistance Touch.
 * It is a "Cheap Yellow Display" family board: the display half is wired
 * identically to the ESP32-2432S028 CYD, and what differs is around it
 * (battery + TP4054 charger, a switchable audio amp on GPIO4, the red LED on
 * GPIO22, battery sense on GPIO34, different headers).  See README-power.md /
 * README-sound.md for those.
 *
 * The board ships with either of two 240x320 panels behind the same
 * silkscreen: an ILI9341 (BGR, columns mirrored, SDO wired to GPIO12) or an
 * ST7789 (RGB, columns not mirrored, SDO not wired).  begin() tells them apart
 * by reading the ILI9341's ID over GPIO12: an answer means ILI9341, silence
 * means ST7789.  setPanel() overrides the detection.
 *
 * Drives the panel through the ESP-IDF SPI master driver
 * (every transaction goes through spi_device_polling_transmit, so the bus is
 * shared cleanly with any other ESP-IDF SPI peripheral).  Designed to feed an
 * LVGL display in PARTIAL render mode: LVGL hands us a rendered region and we
 * push it with flushRegion().
 *
 * Board wiring (fixed on the ESP32-32E 2.8"):
 *   TFT_MOSI 13   TFT_SCLK 14   TFT_CS 15   TFT_DC 2
 *   TFT_MISO 12 (ILI9341 boards only; read once at begin() for detection)
 *   TFT_BL   21 (backlight, active HIGH)
 *   Panel reset is not on a GPIO (GPIO12 was tested and does not reset either
 *   panel), so the driver issues a software reset instead.
 *
 * Touch lives in the companion chipguy_ESP32_32E_2_8inch_touch driver (XPT2046).
 */

#pragma once

#include <Arduino.h>
#include <driver/spi_master.h>

// ── Board-option guard ─────────────────────────────────────────────────────
// This board has 4 MB of flash and no PSRAM.  The Arduino IDE does not persist
// board options reliably, and "ESP32 Dev Module" offers larger flash sizes and
// PSRAM in its menus.  Either mistake is a bad time: a flash size larger than
// the chip produces an image the bootloader won't run, and PSRAM enabled on a
// board without it makes the core probe for it at boot, log about the PSRAM
// GPIOs (16/17 — this board's LED pins) being claimed by something else, and
// tie up those pins.  So refuse at compile time when ARDUINO_FQBN — which the
// core defines from the build's FQBN — names a wrong value.  An FQBN that
// omits an option is left alone: it gets the board's default, which is right
// for both.  Option matching is exact: the name=value must be followed by a
// comma or the end of the string.
static constexpr bool e32e28_fqbnHasOption(const char *s, const char *opt) {
    for (; *s; ++s) {
        const char *a = s; const char *b = opt;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b && (*a == ',' || *a == 0)) return true;
    }
    return false;
}
#ifdef ARDUINO_FQBN
static_assert(!e32e28_fqbnHasOption(ARDUINO_FQBN, "FlashSize=8M") &&
              !e32e28_fqbnHasOption(ARDUINO_FQBN, "FlashSize=16M") &&
              !e32e28_fqbnHasOption(ARDUINO_FQBN, "FlashSize=32M"),
    "Flash size is set larger than 4 MB. This board has 4 MB of flash: select "
    "Tools > Flash Size > 4MB (or FlashSize=4M in the --fqbn).");
static_assert(!e32e28_fqbnHasOption(ARDUINO_FQBN, "PSRAM=enabled"),
    "PSRAM is enabled. This board has no PSRAM, and enabling it claims GPIO16/17 and "
    "logs errors at boot: select Tools > PSRAM > Disabled (or PSRAM=disabled in the --fqbn).");
#endif
#ifdef BOARD_HAS_PSRAM
#error "BOARD_HAS_PSRAM is defined: this board has no PSRAM. Select Tools > PSRAM > Disabled."
#endif

// Native panel geometry (portrait).  width()/height() reflect the active
// rotation; these are the unrotated physical dimensions.
#define E32E28_TFT_NATIVE_WIDTH   240
#define E32E28_TFT_NATIVE_HEIGHT  320

// Normalize a rotation argument to an index 0..3.  Accepts either the index
// directly (0..3) or degrees (0/90/180/270) as aliases for 0/1/2/3.
static inline uint8_t e32e28_normalize_rotation(uint16_t rotation) {
    switch (rotation) {
        case 90:  return 1;
        case 180: return 2;
        case 270: return 3;
        default:  return (uint8_t)(rotation & 0x03);
    }
}

// Which panel controller is fitted.  PANEL_AUTO (the default) detects it in
// begin().
enum e32e28_panel_t : uint8_t {
    E32E28_PANEL_AUTO = 0,
    E32E28_PANEL_ILI9341,
    E32E28_PANEL_ST7789,
};

class chipguy_ESP32_32E_2_8inch_display {
public:
    chipguy_ESP32_32E_2_8inch_display();

    // Force the panel type instead of detecting it.  Call before begin().
    void setPanel(e32e28_panel_t panel) { _panel = panel; }

    // The panel in use: after begin(), the detected (or forced) type.
    e32e28_panel_t panel() const { return _panel; }
    const char *panelName() const;

    // Initialize the SPI bus, detect and initialize the panel, and turn on
    // the backlight.
    // rotation: 0 = portrait (240x320, native), 1 = landscape (320x240),
    //           2 = portrait flipped, 3 = landscape flipped.  Degrees
    //           (0/90/180/270) are accepted as aliases for 0/1/2/3.
    // Returns false if the SPI bus/device could not be set up.
    bool begin(uint16_t rotation = 0);

    // Change rotation after begin().  Updates width()/height() and the
    // panel's MADCTL.  Accepts an index 0..3 or degrees 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Logical dimensions for the current rotation.
    int16_t width()  const { return _w; }
    int16_t height() const { return _h; }

    // Current rotation (0..3).
    uint8_t rotation() const { return _rotation; }

    // Backlight brightness, 0..100 (active HIGH on this board).
    void setBacklight(int percentage);

    // Push a rectangular region of RGB565 pixels to the panel.  Coordinates
    // are inclusive.  `pixels` is in LVGL's native little-endian byte order;
    // this call byte-swaps it in place to the big-endian order the panel
    // expects, so the buffer is modified.
    //
    // Asynchronous: it byte-swaps, waits for the PREVIOUS flush's DMA to finish,
    // sends the address window, then QUEUES this region's pixel DMA and returns
    // before it completes.  With double buffering, the caller can render the
    // next region into the other buffer while this one transmits.  `pixels`
    // must stay valid and untouched until the next flushRegion()/waitFlush().
    void flushRegion(int x1, int y1, int x2, int y2, uint16_t *pixels);

    // Block until the most recently queued flush has finished transmitting.
    // Call before tearing down or doing direct drawing after a flush.
    void waitFlush();

    // Fill the whole screen with one RGB565 color (handy for bring-up tests).
    void fillScreen(uint16_t color);

private:
    spi_device_handle_t _spi = nullptr;
    e32e28_panel_t _panel = E32E28_PANEL_AUTO;
    int16_t _w = E32E28_TFT_NATIVE_WIDTH;
    int16_t _h = E32E28_TFT_NATIVE_HEIGHT;
    uint8_t _rotation = 0;

    spi_transaction_t _pixelTrans = {};   // persistent: queued DMA reads it async
    bool _pixelBusy = false;              // true while a queued flush is in flight

    void _cmd(uint8_t cmd);
    void _data(const uint8_t *data, size_t len);
    void _data8(uint8_t val);
    e32e28_panel_t _detectPanel();
    void _initILI9341();
    void _initST7789();
    void _setAddrWindow(int x0, int y0, int x1, int y1);
    void _pushPixels(const uint8_t *data, size_t bytes);
    void _waitPixelDone();
};
