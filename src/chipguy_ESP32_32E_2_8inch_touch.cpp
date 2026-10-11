/*
 * XPT2046 resistive touch driver for the ESP32-32E 2.8" CYD.
 * See chipguy_ESP32_32E_2_8inch_touch.h for wiring and usage.
 */

#include "chipguy_ESP32_32E_2_8inch_touch.h"
#include "chipguy_ESP32_32E_2_8inch_display.h"   // for E32E28_TFT_NATIVE_WIDTH/HEIGHT

#include <Preferences.h>

// NVS namespace + keys for the persisted calibration rectangle.
#define E32E28_TOUCH_NVS_NS   "e32e28touch"
#define E32E28_TOUCH_NVS_XMIN "xmin"
#define E32E28_TOUCH_NVS_XMAX "xmax"
#define E32E28_TOUCH_NVS_YMIN "ymin"
#define E32E28_TOUCH_NVS_YMAX "ymax"
#define E32E28_TOUCH_NVS_MINP "minp"   // minimum pressure filter
#define E32E28_TOUCH_NVS_JUMP "jump"   // max jump (px) filter

// ── Board pin assignments (fixed on the ESP32-32E 2.8") ────────────────────
#define E32E28_TOUCH_CLK   25
#define E32E28_TOUCH_MOSI  32
#define E32E28_TOUCH_MISO  39
#define E32E28_TOUCH_CS    33
#define E32E28_TOUCH_IRQ   36   // active LOW while the panel is being touched

// XPT2046 control bytes: start bit + channel select, 12-bit, differential.
#define XPT2046_CMD_X   0xD0   // read X position
#define XPT2046_CMD_Y   0x90   // read Y position
#define XPT2046_CMD_Z1  0xB0   // read Z1 (pressure)
#define XPT2046_CMD_Z2  0xC0   // read Z2 (pressure)

// Scale for the position-normalized pressure metric (see _readRawSample).  It
// just sets the numeric range of pressure() / the minPressure slider; raise it
// if firm presses read too low, lower it if they saturate at 4095.
#define E32E28_TOUCH_P_SCALE  100.0f

// Default raw-ADC calibration rectangle (typical CYD; tune with readRaw()).
#define E32E28_TOUCH_X_MIN  200
#define E32E28_TOUCH_X_MAX  3900
#define E32E28_TOUCH_Y_MIN  200
#define E32E28_TOUCH_Y_MAX  3900

// Samples taken per axis per read.  We return the MEDIAN of these (not the
// mean), which discards the occasional spike instead of blending it in.  Odd
// so the median is a single middle element.
static const int E32E28_TOUCH_SAMPLES = 7;

chipguy_ESP32_32E_2_8inch_touch::chipguy_ESP32_32E_2_8inch_touch()
    : _xMin(E32E28_TOUCH_X_MIN), _xMax(E32E28_TOUCH_X_MAX),
      _yMin(E32E28_TOUCH_Y_MIN), _yMax(E32E28_TOUCH_Y_MAX) {}

void chipguy_ESP32_32E_2_8inch_touch::begin() {
    pinMode(E32E28_TOUCH_CLK, OUTPUT);
    pinMode(E32E28_TOUCH_MOSI, OUTPUT);
    pinMode(E32E28_TOUCH_MISO, INPUT);
    pinMode(E32E28_TOUCH_CS, OUTPUT);
    // IRQ is GPIO36, an input-only pin with NO internal pull resistor; the
    // XPT2046's PENIRQ drives it high when idle, low when touched.
    pinMode(E32E28_TOUCH_IRQ, INPUT);

    digitalWrite(E32E28_TOUCH_CS, HIGH);   // deselect
    digitalWrite(E32E28_TOUCH_CLK, LOW);
}

void chipguy_ESP32_32E_2_8inch_touch::setRotation(uint16_t rotation) {
    _rotation = e32e28_normalize_rotation(rotation);
}

void chipguy_ESP32_32E_2_8inch_touch::setCalibration(uint16_t xMin, uint16_t xMax,
                                               uint16_t yMin, uint16_t yMax) {
    _xMin = xMin; _xMax = xMax;
    _yMin = yMin; _yMax = yMax;
}

void chipguy_ESP32_32E_2_8inch_touch::getCalibration(uint16_t &xMin, uint16_t &xMax,
                                               uint16_t &yMin, uint16_t &yMax) const {
    xMin = _xMin; xMax = _xMax;
    yMin = _yMin; yMax = _yMax;
}

bool chipguy_ESP32_32E_2_8inch_touch::loadCalibration() {
    Preferences prefs;
    if (!prefs.begin(E32E28_TOUCH_NVS_NS, /*readOnly=*/true)) return false;
    // Treat the calibration as present only if all four keys exist.
    bool ok = prefs.isKey(E32E28_TOUCH_NVS_XMIN) && prefs.isKey(E32E28_TOUCH_NVS_XMAX) &&
              prefs.isKey(E32E28_TOUCH_NVS_YMIN) && prefs.isKey(E32E28_TOUCH_NVS_YMAX);
    if (ok) {
        _xMin = prefs.getUShort(E32E28_TOUCH_NVS_XMIN, _xMin);
        _xMax = prefs.getUShort(E32E28_TOUCH_NVS_XMAX, _xMax);
        _yMin = prefs.getUShort(E32E28_TOUCH_NVS_YMIN, _yMin);
        _yMax = prefs.getUShort(E32E28_TOUCH_NVS_YMAX, _yMax);
    }
    prefs.end();
    return ok;
}

void chipguy_ESP32_32E_2_8inch_touch::saveCalibration() {
    Preferences prefs;
    if (!prefs.begin(E32E28_TOUCH_NVS_NS, /*readOnly=*/false)) return;
    prefs.putUShort(E32E28_TOUCH_NVS_XMIN, _xMin);
    prefs.putUShort(E32E28_TOUCH_NVS_XMAX, _xMax);
    prefs.putUShort(E32E28_TOUCH_NVS_YMIN, _yMin);
    prefs.putUShort(E32E28_TOUCH_NVS_YMAX, _yMax);
    prefs.end();
}

void chipguy_ESP32_32E_2_8inch_touch::setFilters(uint16_t minPressure, uint16_t maxJump) {
    _minPressure = minPressure;
    _maxJump = maxJump;
}

void chipguy_ESP32_32E_2_8inch_touch::getFilters(uint16_t &minPressure, uint16_t &maxJump) const {
    minPressure = _minPressure;
    maxJump = _maxJump;
}

bool chipguy_ESP32_32E_2_8inch_touch::loadFilters() {
    Preferences prefs;
    if (!prefs.begin(E32E28_TOUCH_NVS_NS, /*readOnly=*/true)) return false;
    bool ok = prefs.isKey(E32E28_TOUCH_NVS_MINP) && prefs.isKey(E32E28_TOUCH_NVS_JUMP);
    if (ok) {
        _minPressure = prefs.getUShort(E32E28_TOUCH_NVS_MINP, _minPressure);
        _maxJump     = prefs.getUShort(E32E28_TOUCH_NVS_JUMP, _maxJump);
    }
    prefs.end();
    return ok;
}

void chipguy_ESP32_32E_2_8inch_touch::saveFilters() {
    Preferences prefs;
    if (!prefs.begin(E32E28_TOUCH_NVS_NS, /*readOnly=*/false)) return;
    prefs.putUShort(E32E28_TOUCH_NVS_MINP, _minPressure);
    prefs.putUShort(E32E28_TOUCH_NVS_JUMP, _maxJump);
    prefs.end();
}

// Bit-bang one XPT2046 conversion: send the 8-bit command, then clock out the
// 12-bit result (MSB first).  SPI mode 0, sampled on the rising edge.
uint16_t chipguy_ESP32_32E_2_8inch_touch::_transferByteAndRead12(uint8_t cmd) {
    // Send command byte.
    for (int i = 7; i >= 0; i--) {
        digitalWrite(E32E28_TOUCH_MOSI, (cmd >> i) & 1);
        digitalWrite(E32E28_TOUCH_CLK, HIGH);
        digitalWrite(E32E28_TOUCH_CLK, LOW);
    }

    // One busy clock between command and result.
    digitalWrite(E32E28_TOUCH_CLK, HIGH);
    digitalWrite(E32E28_TOUCH_CLK, LOW);

    // Clock in 12 result bits, MSB first.
    uint16_t value = 0;
    for (int i = 0; i < 12; i++) {
        digitalWrite(E32E28_TOUCH_CLK, HIGH);
        value = (value << 1) | (digitalRead(E32E28_TOUCH_MISO) & 1);
        digitalWrite(E32E28_TOUCH_CLK, LOW);
    }
    return value;
}

// Read one channel E32E28_TOUCH_SAMPLES times and return the MEDIAN.  The first
// conversion right after switching channels is the least settled, so we take a
// throwaway read before sampling.  The median rejects outliers (the spikes that
// make a plain average jump around) far better than a mean.  Call with CS
// already asserted (LOW) — _readRawSample holds it for the whole sample block.
uint16_t chipguy_ESP32_32E_2_8inch_touch::_readChannelMedian(uint8_t cmd) {
    uint16_t buf[E32E28_TOUCH_SAMPLES];
    _transferByteAndRead12(cmd);                              // throwaway: let it settle
    for (int i = 0; i < E32E28_TOUCH_SAMPLES; i++) buf[i] = _transferByteAndRead12(cmd);

    // Insertion sort (tiny fixed array), then take the middle element.
    for (int i = 1; i < E32E28_TOUCH_SAMPLES; i++) {
        uint16_t v = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
        buf[j + 1] = v;
    }
    return buf[E32E28_TOUCH_SAMPLES / 2];
}

bool chipguy_ESP32_32E_2_8inch_touch::_readRawSample(uint16_t &rawX, uint16_t &rawY) {
    // IRQ is pulled low by the controller only while the panel is pressed.
    if (digitalRead(E32E28_TOUCH_IRQ) != LOW) return false;

    digitalWrite(E32E28_TOUCH_CS, LOW);
    // Median of several conversions per axis rejects the occasional spike far
    // better than a mean (which blends it in) — resistive jitter is mostly
    // impulse noise.  See _readChannelMedian.
    rawX = _readChannelMedian(XPT2046_CMD_X);
    rawY = _readChannelMedian(XPT2046_CMD_Y);

    // Pressure via the XPT2046 datasheet touch-resistance formula:
    //   R_touch ∝ (xpos / 4096) * (z2/z1 - 1)
    // The (xpos/4096) term divides out the touch position, so the result is
    // roughly uniform across the panel — unlike the raw z1+4096-z2 proxy, which
    // reads much lighter near the low-rawX edge.  We report its inverse
    // (conductance), scaled, so higher = firmer press (matching minPressure).
    uint16_t z1 = _transferByteAndRead12(XPT2046_CMD_Z1);
    uint16_t z2 = _transferByteAndRead12(XPT2046_CMD_Z2);
    digitalWrite(E32E28_TOUCH_CS, HIGH);

    uint16_t xpos = rawX;                  // median X position for the formula
    float p = 0.0f;
    if (z1 > 0 && xpos > 0) {
        if (z2 <= z1) {
            p = 4095.0f;                       // resistance ~0 (or noise): treat as firmest
        } else {
            float r = ((float)xpos / 4096.0f) * ((float)z2 / (float)z1 - 1.0f);
            p = (r > 0.0f) ? E32E28_TOUCH_P_SCALE / r : 4095.0f;
        }
    }
    _lastPressure = (uint16_t)(p > 4095.0f ? 4095.0f : p);

    // Still pressed after sampling?  Rejects spurious single-edge IRQs.
    if (digitalRead(E32E28_TOUCH_IRQ) != LOW) return false;

    return true;
}

bool chipguy_ESP32_32E_2_8inch_touch::readRaw(uint16_t &rawX, uint16_t &rawY) {
    return _readRawSample(rawX, rawY);
}

bool chipguy_ESP32_32E_2_8inch_touch::read(uint16_t &x, uint16_t &y) {
    uint16_t rawX, rawY;
    if (!_readRawSample(rawX, rawY)) {
        _hasLast = false;       // released: forget the last point for jump filter
        _jumpCount = 0;
        return false;
    }

    // Pressure filter: reject touches lighter than the configured threshold.
    if (_minPressure > 0 && _lastPressure < _minPressure) return false;

    // Clamp to the calibration rectangle, then map to physical pixels
    // (240 wide x 320 tall, before rotation).
    long px = map((long)constrain(rawX, _xMin, _xMax), _xMin, _xMax,
                  0, E32E28_TFT_NATIVE_WIDTH - 1);
    long py = map((long)constrain(rawY, _yMin, _yMax), _yMin, _yMax,
                  0, E32E28_TFT_NATIVE_HEIGHT - 1);

    // The XPT2046's raw X increases opposite the panel's X axis (same on the ILI9341 and ST7789 boards) on this
    // board, so mirror it.  (Done in physical space so every rotation inherits
    // the correction.)
    px = (E32E28_TFT_NATIVE_WIDTH - 1) - px;

    const long W = E32E28_TFT_NATIVE_WIDTH - 1;    // 239
    const long H = E32E28_TFT_NATIVE_HEIGHT - 1;   // 319

    switch (_rotation) {
        case 0:  x = px;          y = py;          break;  // portrait
        case 1:  x = py;          y = W - px;      break;  // landscape
        case 2:  x = W - px;      y = H - py;      break;  // portrait flipped
        case 3:  x = H - py;      y = px;          break;  // landscape flipped
    }

    // Jump filter: drop a reading that teleports more than _maxJump pixels from
    // the last accepted point.  A real fast drag survives because we let the
    // reading through after a couple of consecutive "jumps".
    if (_maxJump > 0 && _hasLast) {
        long d = labs((long)x - _lastX) + labs((long)y - _lastY);
        if (d > _maxJump && ++_jumpCount < 3) return false;
    }
    _jumpCount = 0;
    _lastX = x; _lastY = y; _hasLast = true;
    return true;
}
