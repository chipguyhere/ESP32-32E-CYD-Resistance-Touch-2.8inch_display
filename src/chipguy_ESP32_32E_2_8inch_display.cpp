/*
 * ILI9341 / ST7789 SPI display driver for the ESP32-32E 2.8" CYD.
 * See chipguy_ESP32_32E_2_8inch_display.h for the wiring and usage overview.
 */

#include "chipguy_ESP32_32E_2_8inch_display.h"
#include <driver/gpio.h>
#include <esp_heap_caps.h>

// ── Board pin assignments (fixed on the ESP32-32E 2.8") ────────────────────
#define E32E28_TFT_MOSI  13
#define E32E28_TFT_SCLK  14
#define E32E28_TFT_CS    15
#define E32E28_TFT_DC     2
#define E32E28_TFT_MISO  12   // panel SDO; read only by _detectPanel(), then left off the bus
#define E32E28_TFT_BL    21   // backlight, active HIGH
// No reset GPIO: holding GPIO12 LOW resets neither panel (bench, 2026-10-10),
// so the init sequences start with a software reset.

// The display shares no bus with anything else on this board, so we clock it
// fast.  Both panels take 40 MHz comfortably over the short CYD traces.
#define E32E28_TFT_SPI_HZ  40000000
#define E32E28_TFT_SPI_HOST  SPI2_HOST   // HSPI

// The ILI9341 panel is BGR with its columns mirrored relative to the ST7789
// panel, which is RGB.  setRotation() builds the ILI9341 MADCTL and, for the
// ST7789, clears BGR and flips MX.  Rotation bits (MY/MX/MV) are OR'd in per
// rotation.
#define E32E28_MADCTL_BGR  0x08
#define ILI9341_MADCTL_MY  0x80
#define ILI9341_MADCTL_MX  0x40
#define ILI9341_MADCTL_MV  0x20

// Largest single SPI transfer we size DMA descriptors for: a full screen, so
// any partial flush region fits in one queued transaction.
static const size_t E32E28_MAX_TRANSFER =
    (size_t)E32E28_TFT_NATIVE_WIDTH * E32E28_TFT_NATIVE_HEIGHT * 2;

// CS is driven by the SPI peripheral (hardware CS), but DC is a plain GPIO.
// The driver calls this just before each transaction goes on the bus — for
// queued transactions that's in ISR context — and we set DC from the
// transaction's `user` field: 0 = command, 1 = data.
static void _e32e28_spi_pre_cb(spi_transaction_t *t) {
    gpio_set_level((gpio_num_t)E32E28_TFT_DC, (int)(intptr_t)t->user);
}

chipguy_ESP32_32E_2_8inch_display::chipguy_ESP32_32E_2_8inch_display() {}

// ── Low-level SPI helpers ──────────────────────────────────────────────────
// DC and CS are handled by the pre-callback and hardware CS respectively, so
// these just submit the bytes.  All command/parameter traffic is synchronous
// (polling); only the pixel blit is queued asynchronously (see flushRegion).

void chipguy_ESP32_32E_2_8inch_display::_cmd(uint8_t cmd) {
    if (!_spi) return;
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_data[0] = cmd;
    t.flags = SPI_TRANS_USE_TXDATA;
    t.user = (void *)0;          // DC = command
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_ESP32_32E_2_8inch_display::_data(const uint8_t *data, size_t len) {
    if (!_spi || len == 0) return;
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

void chipguy_ESP32_32E_2_8inch_display::_data8(uint8_t val) {
    _data(&val, 1);
}

// ── Panel detection ────────────────────────────────────────────────────────
// Runs before the SPI peripheral owns the pins, so it bit-bangs (~200 kHz,
// mode 0).  Both boards wire the panel's SDO to GPIO12.  The ILI9341's ID
// only comes back through the undocumented index register: write 0xD9 with
// 0x10+n, then a one-byte 0xD3 read returns ID byte n (00 93 41 for n=1..3) —
// the readcommand8() method Adafruit and TFT_eSPI use.  The ST7789 has no
// 0xD3, leaves SDO high-Z, and GPIO12 just follows its pull.  Reading under
// both pulls separates "the panel answered" from "the pin floated".
//
// An ST7789 that did answer 0xD3 with something would land in neither case,
// so the fallback is RDDID 0x04, which the ST7789 does answer: 85 85 52 per
// the datasheet, 81 81 B3 on the board probed (bench, 2026-10-11).  The
// ILI9341s return 00 00 00 there (no module ID programmed).

static uint8_t _e32e28_bbXfer(uint8_t out) {
    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)E32E28_TFT_MOSI, (out >> i) & 1);
        delayMicroseconds(2);
        gpio_set_level((gpio_num_t)E32E28_TFT_SCLK, 1);
        in = (in << 1) | gpio_get_level((gpio_num_t)E32E28_TFT_MISO);
        delayMicroseconds(2);
        gpio_set_level((gpio_num_t)E32E28_TFT_SCLK, 0);
    }
    return in;
}

// Write the 0xD9 index, then read one byte of 0xD3.
static uint8_t _e32e28_bbReadIdx(uint8_t idx) {
    gpio_set_level((gpio_num_t)E32E28_TFT_CS, 0);
    gpio_set_level((gpio_num_t)E32E28_TFT_DC, 0);
    _e32e28_bbXfer(0xD9);
    gpio_set_level((gpio_num_t)E32E28_TFT_DC, 1);
    _e32e28_bbXfer(0x10 + idx);
    gpio_set_level((gpio_num_t)E32E28_TFT_CS, 1);
    delayMicroseconds(5);
    gpio_set_level((gpio_num_t)E32E28_TFT_CS, 0);
    gpio_set_level((gpio_num_t)E32E28_TFT_DC, 0);
    _e32e28_bbXfer(0xD3);
    gpio_set_level((gpio_num_t)E32E28_TFT_DC, 1);
    uint8_t v = _e32e28_bbXfer(0x00);
    gpio_set_level((gpio_num_t)E32E28_TFT_CS, 1);
    delayMicroseconds(5);
    return v;
}

// RDDID 0x04: one dummy clock after the command, then three ID bytes.
static void _e32e28_bbReadRDDID(uint8_t out[3]) {
    gpio_set_level((gpio_num_t)E32E28_TFT_CS, 0);
    gpio_set_level((gpio_num_t)E32E28_TFT_DC, 0);
    _e32e28_bbXfer(0x04);
    gpio_set_level((gpio_num_t)E32E28_TFT_DC, 1);
    gpio_set_level((gpio_num_t)E32E28_TFT_SCLK, 1);
    delayMicroseconds(2);
    gpio_set_level((gpio_num_t)E32E28_TFT_SCLK, 0);
    delayMicroseconds(2);
    for (int i = 0; i < 3; i++) out[i] = _e32e28_bbXfer(0x00);
    gpio_set_level((gpio_num_t)E32E28_TFT_CS, 1);
    delayMicroseconds(5);
}

e32e28_panel_t chipguy_ESP32_32E_2_8inch_display::_detectPanel() {
    gpio_set_direction((gpio_num_t)E32E28_TFT_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)E32E28_TFT_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)E32E28_TFT_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)E32E28_TFT_SCLK, 0);
    gpio_set_level((gpio_num_t)E32E28_TFT_CS, 1);
    gpio_set_direction((gpio_num_t)E32E28_TFT_MISO, GPIO_MODE_INPUT);

    uint8_t id[2][2];
    uint8_t rddid[2][3];
    const gpio_pull_mode_t pulls[2] = {GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY};
    for (int p = 0; p < 2; p++) {
        gpio_set_pull_mode((gpio_num_t)E32E28_TFT_MISO, pulls[p]);
        delayMicroseconds(50);
        id[p][0] = _e32e28_bbReadIdx(2);
        id[p][1] = _e32e28_bbReadIdx(3);
        _e32e28_bbReadRDDID(rddid[p]);
    }
    // Leave GPIO12 as the chip's reset default: an input with pull-down.
    // It is the MTDI strapping pin, and the panel only drives it during reads.

    if (id[0][0] == 0x93 && id[0][1] == 0x41 && id[1][0] == 0x93 && id[1][1] == 0x41)
        return E32E28_PANEL_ILI9341;
    if (id[0][0] == 0xFF && id[0][1] == 0xFF && id[1][0] == 0x00 && id[1][1] == 0x00)
        return E32E28_PANEL_ST7789;
    // Something answered 0xD3, but not with the ILI9341's ID.  Match RDDID
    // bytes 2 and 3 (version, driver ID) under both pulls; byte 1 is the
    // module vendor and varies.
    static const uint8_t st7789Ids[][2] = {{0x85, 0x52}, {0x81, 0xB3}};
    for (const auto &st : st7789Ids) {
        if (rddid[0][1] == st[0] && rddid[0][2] == st[1] &&
            rddid[1][1] == st[0] && rddid[1][2] == st[1])
            return E32E28_PANEL_ST7789;
    }
    Serial.printf("e32e28 display: unrecognised panel ID %02X %02X / %02X %02X, "
                  "RDDID %02X %02X %02X / %02X %02X %02X; assuming ILI9341\n",
                  id[0][0], id[0][1], id[1][0], id[1][1],
                  rddid[0][0], rddid[0][1], rddid[0][2], rddid[1][0], rddid[1][1], rddid[1][2]);
    return E32E28_PANEL_ILI9341;
}

const char *chipguy_ESP32_32E_2_8inch_display::panelName() const {
    switch (_panel) {
        case E32E28_PANEL_ILI9341: return "ILI9341";
        case E32E28_PANEL_ST7789:  return "ST7789";
        default:                   return "unknown";
    }
}

// ── Power-on init sequences ────────────────────────────────────────────────
// MADCTL is written here as a placeholder and then re-issued by setRotation().

// Standard ILI9341 startup (gamma, power, frame rate).
void chipguy_ESP32_32E_2_8inch_display::_initILI9341() {
    _cmd(0x01);                            // Software reset
    delay(150);
    _cmd(0xEF); { const uint8_t d[] = {0x03, 0x80, 0x02}; _data(d, 3); }
    _cmd(0xCF); { const uint8_t d[] = {0x00, 0xC1, 0x30}; _data(d, 3); }
    _cmd(0xED); { const uint8_t d[] = {0x64, 0x03, 0x12, 0x81}; _data(d, 4); }
    _cmd(0xE8); { const uint8_t d[] = {0x85, 0x00, 0x78}; _data(d, 3); }
    _cmd(0xCB); { const uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02}; _data(d, 5); }
    _cmd(0xF7); _data8(0x20);
    _cmd(0xEA); { const uint8_t d[] = {0x00, 0x00}; _data(d, 2); }
    _cmd(0xC0); _data8(0x23);              // Power control 1
    _cmd(0xC1); _data8(0x10);              // Power control 2
    _cmd(0xC5); { const uint8_t d[] = {0x3E, 0x28}; _data(d, 2); }  // VCOM 1
    _cmd(0xC7); _data8(0x86);              // VCOM 2
    _cmd(0x36); _data8(E32E28_MADCTL_BGR);    // MADCTL (overwritten by setRotation)
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565
    _cmd(0xB1); { const uint8_t d[] = {0x00, 0x18}; _data(d, 2); }  // Frame rate
    _cmd(0xB6); { const uint8_t d[] = {0x08, 0x82, 0x27}; _data(d, 3); } // Display func
    _cmd(0xF2); _data8(0x00);              // 3Gamma disable
    _cmd(0x26); _data8(0x01);              // Gamma curve selected
    _cmd(0xE0); { const uint8_t d[] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E,
                  0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}; _data(d, 15); }
    _cmd(0xE1); { const uint8_t d[] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31,
                  0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}; _data(d, 15); }
    _cmd(0x11);                            // Sleep out
    delay(120);
    _cmd(0x29);                            // Display on
    delay(20);
}

// ST7789: the power-on defaults are right for this panel, so only the pixel
// format and display state are set.  Inversion stays OFF — this panel shows
// correct colors without INVON, unlike most ST7789 IPS modules.
void chipguy_ESP32_32E_2_8inch_display::_initST7789() {
    _cmd(0x01);                            // Software reset
    delay(150);
    _cmd(0x11);                            // Sleep out
    delay(120);
    _cmd(0x3A); _data8(0x55);              // Pixel format: 16-bit RGB565
    _cmd(0x36); _data8(0x00);              // MADCTL (overwritten by setRotation)
    _cmd(0x20);                            // Inversion off
    _cmd(0x13);                            // Normal display mode
    _cmd(0x29);                            // Display on
    delay(20);
}

void chipguy_ESP32_32E_2_8inch_display::_setAddrWindow(int x0, int y0, int x1, int y1) {
    uint8_t col[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    _cmd(0x2A);              // CASET — Column Address Set
    _data(col, 4);

    uint8_t row[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };
    _cmd(0x2B);              // RASET — Row Address Set
    _data(row, 4);

    _cmd(0x2C);              // RAMWR — Memory Write
}

// Synchronous (blocking) pixel push, used by fillScreen().  The interactive
// path uses the queued/async flushRegion() instead.
void chipguy_ESP32_32E_2_8inch_display::_pushPixels(const uint8_t *data, size_t bytes) {
    if (!_spi || bytes == 0) return;
    spi_transaction_t t = {};
    t.length = bytes * 8;
    t.tx_buffer = data;
    t.user = (void *)1;          // DC = data
    spi_device_polling_transmit(_spi, &t);
}

// Block until the in-flight async pixel DMA (if any) has completed, so the bus
// is idle before we touch it directly.
void chipguy_ESP32_32E_2_8inch_display::_waitPixelDone() {
    if (_pixelBusy) {
        spi_transaction_t *result = nullptr;
        spi_device_get_trans_result(_spi, &result, portMAX_DELAY);
        _pixelBusy = false;
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

bool chipguy_ESP32_32E_2_8inch_display::begin(uint16_t rotation) {
    // DC is driven by the pre-transaction callback; configure it as output.
    // CS is owned by the SPI peripheral (hardware CS) once the bus is up.
    gpio_set_direction((gpio_num_t)E32E28_TFT_DC, GPIO_MODE_OUTPUT);

    // Which panel?  Must run before spi_bus_initialize() takes the pins.
    if (_panel == E32E28_PANEL_AUTO) _panel = _detectPanel();

    // Register the SPI bus with the ESP-IDF master driver.
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = E32E28_TFT_MOSI;
    buscfg.miso_io_num = -1;            // write-only blitting; GPIO12 was only needed to detect
    buscfg.sclk_io_num = E32E28_TFT_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = E32E28_MAX_TRANSFER;

    esp_err_t err = spi_bus_initialize(E32E28_TFT_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("e32e28 display: spi_bus_initialize failed: 0x%x\n", err);
        return false;
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = E32E28_TFT_SPI_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = E32E28_TFT_CS;   // hardware-managed CS
    devcfg.queue_size = 1;              // one async pixel transfer in flight
    devcfg.pre_cb = _e32e28_spi_pre_cb;    // sets DC from each transaction's user flag
    devcfg.flags = SPI_DEVICE_NO_DUMMY;

    err = spi_bus_add_device(E32E28_TFT_SPI_HOST, &devcfg, &_spi);
    if (err != ESP_OK) {
        Serial.printf("e32e28 display: spi_bus_add_device failed: 0x%x\n", err);
        _spi = nullptr;
        return false;
    }

    if (_panel == E32E28_PANEL_ST7789) _initST7789();
    else                               _initILI9341();
    setRotation(rotation);

    // Backlight on at full brightness.
    setBacklight(100);

    return true;
}

void chipguy_ESP32_32E_2_8inch_display::setRotation(uint16_t rotation) {
    _waitPixelDone();           // bus must be idle before we send a command
    _rotation = e32e28_normalize_rotation(rotation);
    uint8_t madctl = E32E28_MADCTL_BGR;

    switch (_rotation) {
        case 0:  // portrait, native
            madctl |= ILI9341_MADCTL_MX;
            _w = E32E28_TFT_NATIVE_WIDTH;
            _h = E32E28_TFT_NATIVE_HEIGHT;
            break;
        case 1:  // landscape
            madctl |= ILI9341_MADCTL_MV;
            _w = E32E28_TFT_NATIVE_HEIGHT;
            _h = E32E28_TFT_NATIVE_WIDTH;
            break;
        case 2:  // portrait flipped
            madctl |= ILI9341_MADCTL_MY;
            _w = E32E28_TFT_NATIVE_WIDTH;
            _h = E32E28_TFT_NATIVE_HEIGHT;
            break;
        case 3:  // landscape flipped
            madctl |= ILI9341_MADCTL_MV | ILI9341_MADCTL_MX | ILI9341_MADCTL_MY;
            _w = E32E28_TFT_NATIVE_HEIGHT;
            _h = E32E28_TFT_NATIVE_WIDTH;
            break;
    }
    // The ST7789 panel is RGB, and its columns run the other way.
    if (_panel == E32E28_PANEL_ST7789)
        madctl = (madctl & ~E32E28_MADCTL_BGR) ^ ILI9341_MADCTL_MX;

    _cmd(0x36);          // MADCTL
    _data8(madctl);
}

void chipguy_ESP32_32E_2_8inch_display::setBacklight(int percentage) {
    if (percentage < 0)   percentage = 0;
    if (percentage > 100) percentage = 100;
    // Active HIGH: higher duty = brighter.
    analogWrite(E32E28_TFT_BL, percentage * 255 / 100);
}

// Asynchronous flush.  Returns once this region's pixel DMA has been *queued*,
// not when it finishes — the caller (LVGL via lv_setup) is then free to render
// the next region into the other buffer while this one transmits.  The bus is
// serialized by blocking on the *previous* flush's DMA at the top of this one.
void chipguy_ESP32_32E_2_8inch_display::flushRegion(int x1, int y1, int x2, int y2,
                                              uint16_t *pixels) {
    if (!_spi) return;

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    size_t count = (size_t)w * h;

    // The swap below mutates THIS region's buffer.  With double buffering LVGL
    // hands us a different buffer than the one the previous flush is still
    // DMA'ing, so the swap safely overlaps that transfer.  Guard the (not
    // expected) case where we're handed the in-flight buffer: wait first.
    if (_pixelBusy && (const void *)pixels == _pixelTrans.tx_buffer) {
        _waitPixelDone();
    }

    // LVGL renders RGB565 little-endian (native ESP32); the panel expects
    // big-endian (MSB first).  Swap in place — overlaps the previous region's
    // in-flight DMA (different buffer) for free.
    for (size_t i = 0; i < count; i++) {
        pixels[i] = __builtin_bswap16(pixels[i]);
    }

    // Now wait for the previous flush's DMA to finish, so the SPI bus is free
    // for this region's address window and pixel transfer.
    _waitPixelDone();

    // Address window (synchronous commands), then queue the pixel blit.
    _setAddrWindow(x1, y1, x2, y2);

    _pixelTrans = {};
    _pixelTrans.length = count * 16;        // bits (2 bytes per pixel)
    _pixelTrans.tx_buffer = pixels;
    _pixelTrans.user = (void *)1;           // DC = data
    if (spi_device_queue_trans(_spi, &_pixelTrans, portMAX_DELAY) == ESP_OK) {
        _pixelBusy = true;
    }
}

void chipguy_ESP32_32E_2_8inch_display::waitFlush() {
    _waitPixelDone();
}

void chipguy_ESP32_32E_2_8inch_display::fillScreen(uint16_t color) {
    if (!_spi) return;
    _waitPixelDone();           // drain any queued async flush first
    uint16_t be = __builtin_bswap16(color);

    // One-row buffer pushed for every row.  Must be DMA-capable: the ESP-IDF
    // SPI driver requires DMA memory for transfers larger than the FIFO.
    uint16_t *line = (uint16_t *)heap_caps_malloc((size_t)_w * 2, MALLOC_CAP_DMA);
    if (!line) return;
    for (int i = 0; i < _w; i++) line[i] = be;

    _setAddrWindow(0, 0, _w - 1, _h - 1);
    for (int y = 0; y < _h; y++) {
        _pushPixels((const uint8_t *)line, (size_t)_w * 2);
    }
    heap_caps_free(line);
}
