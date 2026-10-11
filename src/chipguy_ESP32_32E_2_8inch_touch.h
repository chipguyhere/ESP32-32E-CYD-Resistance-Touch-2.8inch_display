/*
 * XPT2046 resistive touch driver for the ESP32-32E 2.8" CYD.
 *
 * The XPT2046 on this board sits on its OWN set of SPI pins (separate from the
 * display bus), so this driver bit-bangs it directly — no bus sharing,
 * no extra library, and it stays well under the XPT2046's ~2.5 MHz limit.
 *
 * Board wiring (fixed on the ESP32-32E 2.8"):
 *   XPT2046_CLK 25   XPT2046_MOSI 32   XPT2046_MISO 39
 *   XPT2046_CS  33   XPT2046_IRQ  36 (active LOW while touched)
 *
 * Resistive panels vary unit-to-unit, so raw ADC readings are mapped to screen
 * pixels through a calibration rectangle.  The defaults work on typical CYDs;
 * call setCalibration() to tune, or readRaw() to gather values for your panel.
 * The calibration rectangle and the read() noise filters can be persisted to
 * NVS (Preferences) and reloaded at startup — see the TouchCalibrate example,
 * which produces and saves both for you.
 */

#pragma once

#include <Arduino.h>

class chipguy_ESP32_32E_2_8inch_touch {
public:
    chipguy_ESP32_32E_2_8inch_touch();

    // Configure the touch GPIOs.  Safe to call before the display is up.
    void begin();

    // Match the touch coordinate mapping to the display rotation, using the
    // same convention as chipguy_ESP32_32E_2_8inch_display: an index 0..3 or degrees
    // 0/90/180/270.
    void setRotation(uint16_t rotation);

    // Override the raw-ADC calibration rectangle.  xMin/xMax map across the
    // panel's physical X axis (240 px), yMin/yMax across the physical Y axis
    // (320 px), both before rotation is applied.  Usually set via
    // loadCalibration() or produced by the TouchCalibrate example.
    void setCalibration(uint16_t xMin, uint16_t xMax, uint16_t yMin, uint16_t yMax);

    // Read the current calibration rectangle (e.g. to display or re-save it).
    void getCalibration(uint16_t &xMin, uint16_t &xMax,
                        uint16_t &yMin, uint16_t &yMax) const;

    // Load the calibration rectangle from NVS (Preferences).  Returns true and
    // applies it if a saved calibration exists; returns false and leaves the
    // current (default) calibration in place otherwise.  Safe to call in setup.
    bool loadCalibration();

    // Persist the current calibration rectangle to NVS (Preferences) so future
    // boots can loadCalibration() it.
    void saveCalibration();

    // Noise filters applied by read() (not readRaw()):
    //   minPressure - reject touches lighter than this (0 = off).  Compare
    //                 against pressure() values gathered on your panel.
    //   maxJump     - reject a reading that teleports more than this many pixels
    //                 from the previous accepted point (0 = off); brief outliers
    //                 are dropped, a sustained move is still let through.
    void setFilters(uint16_t minPressure, uint16_t maxJump);
    void getFilters(uint16_t &minPressure, uint16_t &maxJump) const;

    // Load/save the filter settings in NVS (Preferences), same store as the
    // calibration.  loadFilters() returns true if saved settings were found.
    bool loadFilters();
    void saveFilters();

    // Pressure of the most recent sample: a position-normalized touch-resistance
    // metric (XPT2046 Z and X), higher = firmer press, and roughly uniform
    // across the panel.  Watch it live (e.g. the readout in TouchCalibrate) to
    // choose a minPressure threshold.
    uint16_t pressure() const { return _lastPressure; }

    // Read the touch point.  Returns true if the screen is currently pressed,
    // with x,y mapped to screen pixels for the active rotation.
    bool read(uint16_t &x, uint16_t &y);

    // Read median-filtered raw 12-bit ADC values (no calibration/rotation
    // applied).  Returns true if pressed.  Useful for gathering calibration
    // numbers.
    bool readRaw(uint16_t &rawX, uint16_t &rawY);

private:
    uint8_t  _rotation = 0;
    uint16_t _xMin, _xMax, _yMin, _yMax;

    // Noise-filter settings and state (see setFilters()).
    uint16_t _minPressure = 0;
    uint16_t _maxJump = 0;
    uint16_t _lastPressure = 0;
    uint16_t _lastX = 0, _lastY = 0;
    bool     _hasLast = false;
    uint8_t  _jumpCount = 0;

    uint16_t _transferByteAndRead12(uint8_t cmd);
    uint16_t _readChannelMedian(uint8_t cmd);
    bool     _readRawSample(uint16_t &rawX, uint16_t &rawY);
};
